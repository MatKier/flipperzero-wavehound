#include "net_discover.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_netif_net_stack.h"
#include "lwip/def.h"
#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"

#include "proto.h"

#define ND_MAX_HOSTS 64
/* lwip's ARP cache holds only ARP_TABLE_SIZE (10) entries, so a wide sweep would
 * evict hosts before we read them. Sweep in small chunks that fit the cache and
 * snapshot after each, accumulating into our own table. */
#define ND_SWEEP_CHUNK 6             /* ARP requests per chunk (< ARP_TABLE_SIZE) */
#define ND_CHUNK_REPLY_MS 150        /* wait for a chunk's replies before snapshotting */
#define ND_ARP_SLOTS 32              /* ARP-cache slots to scan (>= ARP_TABLE_SIZE) */
#define ND_RESOLVE_TIMEOUT_MS 400    /* per reverse-DNS / NBNS attempt */
#define ND_HOST_MAX 33               /* hostname buffer incl. NUL */
#define ND_SWEEP_PASSES 3            /* probe the subnet this many times per trigger */
#define ND_PASS_GAP_MS 1500          /* pause between passes so a sleeping host can wake */
#define ND_NAME_MAX_TRIES 3          /* reverse-DNS/NBNS attempts per host before giving up */

typedef struct {
    uint8_t mac[6];
    uint32_t ip;         /* network byte order */
    bool emitted;        /* IP line sent */
    uint8_t name_tries;  /* reverse-DNS/NBNS attempts so far (capped at ND_NAME_MAX_TRIES) */
    char host[ND_HOST_MAX];
} nd_host_t;

static nd_host_t s_hosts[ND_MAX_HOSTS];
static int s_host_count;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_snapshot_done;
static TaskHandle_t s_task;

static struct netif *s_netif; /* lwip netif backing the STA */
static volatile uint32_t s_my_ip, s_netmask, s_dns; /* network byte order */
static volatile bool s_connected;

/* Host-order address range handed to the tcpip-thread send callback. Only the
 * discovery task writes these, and only while a chunk callback is not queued. */
static uint32_t s_chunk_lo, s_chunk_hi;

/* On a retry pass, skip the addresses we already have. s_known_bits is a 256-bit
 * map of the swept /24 (keyed by the last octet), rebuilt by the discovery task
 * before each retry pass and read lock-free by the send callback. */
static uint8_t s_known_bits[32];
static volatile bool s_skip_known;

/* Elicitation: a dedicated task pokes one target every interval so it emits
 * reply frames the hunt can sample. */
static TaskHandle_t s_elicit_task;
static volatile bool s_elicit_active;
static volatile uint32_t s_elicit_interval_ms;
static volatile uint32_t s_elicit_ip; /* resolved target IP (network order), 0 = unknown */
static uint8_t s_elicit_mac[6]; /* guarded by s_lock: a retarget rewrites all 6 bytes */
static volatile uint32_t s_arp_target; /* IP handed to the tcpip-thread ARP sender */

/* --- shared host table (hold s_lock) --- */

static void nd_add_locked(const uint8_t mac[6], uint32_t ip)
{
    for (int i = 0; i < s_host_count; i++) {
        if (memcmp(s_hosts[i].mac, mac, 6) == 0) {
            if (s_hosts[i].ip != ip) {
                s_hosts[i].ip = ip; /* address changed: re-announce and re-resolve */
                s_hosts[i].emitted = false;
                s_hosts[i].name_tries = 0;
                s_hosts[i].host[0] = '\0';
            }
            return;
        }
    }
    if (s_host_count >= ND_MAX_HOSTS) {
        return;
    }
    nd_host_t *h = &s_hosts[s_host_count++];
    memset(h, 0, sizeof(*h));
    memcpy(h->mac, mac, 6);
    h->ip = ip;
}

/* --- tcpip-thread callbacks (lwip core is only safe here) --- */

static void tcpip_send_chunk(void *ctx)
{
    (void)ctx;
    struct netif *nif = s_netif;
    if (!nif) {
        return;
    }
    for (uint32_t h = s_chunk_lo; h <= s_chunk_hi; h++) {
        const uint32_t addr_n = lwip_htonl(h);
        if (addr_n == s_my_ip) {
            continue;
        }
        if (s_skip_known && (s_known_bits[(h & 0xff) >> 3] & (1u << (h & 7)))) {
            continue; /* a retry pass: this address is already in the table */
        }
        ip4_addr_t ip;
        ip4_addr_set_u32(&ip, addr_n);
        etharp_request(nif, &ip);
    }
}

/* ARP entries captured by the last tcpip_snapshot, handed from the tcpip thread
 * to the discovery task. Written only on the tcpip thread; read by the task only
 * after s_snapshot_done, so neither side needs s_lock. Keeping the shared host
 * table off the tcpip thread means that high-priority lwip thread never blocks
 * on s_lock (which the discovery task also holds). */
static struct {
    uint8_t mac[6];
    uint32_t ip;
} s_snap[ND_ARP_SLOTS];
static int s_snap_count;

static void tcpip_snapshot(void *ctx)
{
    (void)ctx;
    ip4_addr_t *ipaddr;
    struct netif *nif;
    struct eth_addr *eth;
    int n = 0;
    /* Read the ARP table here (lwip core is only safe on this thread), but copy
     * the matches into s_snap rather than touching the shared host table under
     * s_lock — the discovery task merges them once we signal done. Scan every
     * slot (empty ones return 0): entries aren't packed, so don't stop at the
     * first gap. */
    for (size_t i = 0; i < ND_ARP_SLOTS && n < (int)ND_ARP_SLOTS; i++) {
        if (!etharp_get_entry(i, &ipaddr, &nif, &eth)) {
            continue;
        }
        if (nif != s_netif) {
            continue;
        }
        const uint32_t ip_n = ip4_addr_get_u32(ipaddr);
        if ((ip_n & s_netmask) != (s_my_ip & s_netmask)) {
            continue; /* off our subnet (e.g. the gateway's own entries) */
        }
        memcpy(s_snap[n].mac, eth->addr, 6);
        s_snap[n].ip = ip_n;
        n++;
    }
    s_snap_count = n;
    xSemaphoreGive(s_snapshot_done);
}

/* --- best-effort hostname resolution (BSD sockets, discovery task) --- */

/* Decode a (possibly compressed) DNS name at `off` into `out`; returns the
 * bytes consumed at `off` (not through pointers), or -1 on malformed input. */
static int dns_read_name(const uint8_t *pkt, int len, int off, char *out, size_t out_sz)
{
    size_t o = 0;
    int consumed = 0;
    bool jumped = false;
    int guard = 0;
    int cur = off;
    while (cur < len) {
        const uint8_t c = pkt[cur];
        if ((c & 0xc0) == 0xc0) {
            if (cur + 1 >= len) {
                return -1;
            }
            if (!jumped) {
                consumed = cur + 2 - off;
            }
            const int ptr = ((c & 0x3f) << 8) | pkt[cur + 1];
            if (ptr >= len || ++guard > 16) {
                return -1;
            }
            cur = ptr;
            jumped = true;
            continue;
        }
        if (c == 0) {
            if (!jumped) {
                consumed = cur + 1 - off;
            }
            break;
        }
        const int l = c;
        cur++;
        if (cur + l > len) {
            return -1;
        }
        for (int i = 0; i < l; i++) {
            if (o + 1 < out_sz) {
                out[o++] = (char)pkt[cur + i];
            }
        }
        cur += l;
        if (o + 1 < out_sz) {
            out[o++] = '.';
        }
    }
    if (o > 0 && out[o - 1] == '.') {
        o--;
    }
    out[o] = '\0';
    return consumed ? consumed : -1;
}

static bool reverse_dns(uint32_t ip_net, uint32_t dns_net, char *out, size_t out_sz)
{
    if (dns_net == 0 || dns_net == 0xffffffffu) {
        return false;
    }
    ip4_addr_t ipa;
    ip4_addr_set_u32(&ipa, ip_net);
    const uint8_t oct[4] = {ip4_addr1(&ipa), ip4_addr2(&ipa), ip4_addr3(&ipa), ip4_addr4(&ipa)};

    uint8_t q[72];
    int n = 0;
    static uint16_t id = 1;
    const uint16_t qid = id++;
    q[n++] = qid >> 8;
    q[n++] = qid & 0xff;
    q[n++] = 0x01; /* flags: RD */
    q[n++] = 0x00;
    q[n++] = 0x00;
    q[n++] = 0x01; /* QDCOUNT = 1 */
    for (int i = 0; i < 6; i++) {
        q[n++] = 0x00; /* AN/NS/AR counts */
    }
    for (int i = 3; i >= 0; i--) { /* d.c.b.a */
        char b[4];
        const int l = snprintf(b, sizeof(b), "%u", oct[i]);
        q[n++] = (uint8_t)l;
        memcpy(q + n, b, l);
        n += l;
    }
    static const char *const suffix[2] = {"in-addr", "arpa"};
    for (int s = 0; s < 2; s++) {
        const int l = (int)strlen(suffix[s]);
        q[n++] = (uint8_t)l;
        memcpy(q + n, suffix[s], l);
        n += l;
    }
    q[n++] = 0x00; /* root label */
    q[n++] = 0x00;
    q[n++] = 0x0c; /* QTYPE = PTR */
    q[n++] = 0x00;
    q[n++] = 0x01; /* QCLASS = IN */

    const int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return false;
    }
    const struct timeval tv = {.tv_sec = ND_RESOLVE_TIMEOUT_MS / 1000,
                               .tv_usec = (ND_RESOLVE_TIMEOUT_MS % 1000) * 1000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = lwip_htons(53);
    dst.sin_addr.s_addr = dns_net;

    bool ok = false;
    if (sendto(sock, q, n, 0, (struct sockaddr *)&dst, sizeof(dst)) == n) {
        uint8_t r[256];
        const int rl = recvfrom(sock, r, sizeof(r), 0, NULL, NULL);
        if (rl >= 12 && ((r[6] << 8) | r[7]) >= 1) {
            /* The server echoes our header + question verbatim, so the first
             * answer starts at our query length n. */
            char name[64];
            const int used = dns_read_name(r, rl, n, name, sizeof(name));
            if (used > 0 && n + used + 10 <= rl) {
                const int p = n + used;
                const int type = (r[p] << 8) | r[p + 1];
                const int rdlen = (r[p + 8] << 8) | r[p + 9];
                const int rdoff = p + 10;
                if (type == 12 && rdoff + rdlen <= rl &&
                    dns_read_name(r, rl, rdoff, out, out_sz) > 0 && out[0]) {
                    ok = true;
                }
            }
        }
    }
    close(sock);
    return ok;
}

static bool nbns_name(uint32_t ip_net, char *out, size_t out_sz)
{
    uint8_t q[50];
    int n = 0;
    static uint16_t id = 0x4242;
    const uint16_t qid = id++;
    q[n++] = qid >> 8;
    q[n++] = qid & 0xff;
    q[n++] = 0x00; /* flags */
    q[n++] = 0x00;
    q[n++] = 0x00;
    q[n++] = 0x01; /* QDCOUNT = 1 */
    for (int i = 0; i < 6; i++) {
        q[n++] = 0x00;
    }
    /* First-level-encoded wildcard name "*": length 0x20, then 32 bytes, then
     * the root label. "*" (0x2a) -> 'C','K'; each trailing NUL -> 'A','A'. */
    q[n++] = 0x20;
    q[n++] = 'C';
    q[n++] = 'K';
    for (int i = 0; i < 15; i++) {
        q[n++] = 'A';
        q[n++] = 'A';
    }
    q[n++] = 0x00;
    q[n++] = 0x00;
    q[n++] = 0x21; /* QTYPE = NBSTAT */
    q[n++] = 0x00;
    q[n++] = 0x01; /* QCLASS = IN */

    const int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return false;
    }
    const struct timeval tv = {.tv_sec = ND_RESOLVE_TIMEOUT_MS / 1000,
                               .tv_usec = (ND_RESOLVE_TIMEOUT_MS % 1000) * 1000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = lwip_htons(137);
    dst.sin_addr.s_addr = ip_net;

    bool ok = false;
    if (sendto(sock, q, n, 0, (struct sockaddr *)&dst, sizeof(dst)) == n) {
        uint8_t r[256];
        const int rl = recvfrom(sock, r, sizeof(r), 0, NULL, NULL);
        int off = 12; /* skip header */
        if (rl > off) {
            if ((r[off] & 0xc0) == 0xc0) {
                off += 2; /* compressed answer name */
            } else {
                off += 1 + r[off] + 1; /* length + label + root */
            }
        }
        /* type(2) class(2) ttl(4) rdlength(2), then rdata: num_names(1) + entries. */
        if (off + 10 < rl) {
            const int rdoff = off + 10;
            const int num = r[rdoff];
            int p = rdoff + 1;
            for (int i = 0; i < num && p + 18 <= rl; i++, p += 18) {
                const uint8_t suffix = r[p + 15];
                const uint16_t flags = (r[p + 16] << 8) | r[p + 17];
                if (!(flags & 0x8000) && suffix == 0x00) { /* unique workstation name */
                    int l = 0;
                    for (int k = 0; k < 15; k++) {
                        const char ch = (char)r[p + k];
                        if (ch == ' ' || ch == '\0') {
                            break;
                        }
                        if (l + 1 < (int)out_sz) {
                            out[l++] = ch;
                        }
                    }
                    out[l] = '\0';
                    if (l > 0) {
                        ok = true;
                        break;
                    }
                }
            }
        }
    }
    close(sock);
    return ok;
}

/* --- sweep orchestration (discovery task) --- */

static void emit_ip(const uint8_t mac[6], uint32_t ip_net, const char *name)
{
    ip4_addr_t ipa;
    ip4_addr_set_u32(&ipa, ip_net);
    char ipstr[16];
    snprintf(ipstr, sizeof(ipstr), "%u.%u.%u.%u", (unsigned)ip4_addr1(&ipa),
             (unsigned)ip4_addr2(&ipa), (unsigned)ip4_addr3(&ipa), (unsigned)ip4_addr4(&ipa));
    if (name && name[0]) {
        char hex[2 * (ND_HOST_MAX - 1) + 1];
        proto_hex_encode(name, strlen(name), hex, sizeof(hex));
        proto_sendf("IP,%02x:%02x:%02x:%02x:%02x:%02x,%s,%s", mac[0], mac[1], mac[2], mac[3], mac[4],
                    mac[5], ipstr, hex);
    } else {
        proto_sendf("IP,%02x:%02x:%02x:%02x:%02x:%02x,%s", mac[0], mac[1], mac[2], mac[3], mac[4],
                    mac[5], ipstr);
    }
}

/* Announce every host found but not yet reported (IP only). */
static void emit_pending_ips(void)
{
    for (;;) {
        nd_host_t h;
        bool found = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < s_host_count; i++) {
            if (!s_hosts[i].emitted) {
                s_hosts[i].emitted = true;
                h = s_hosts[i];
                found = true;
                break;
            }
        }
        xSemaphoreGive(s_lock);
        if (!found) {
            break;
        }
        /* Include the name if we already resolved it, so re-announcing (e.g. for
         * a freshly reopened client list) doesn't downgrade a named row to IP. */
        emit_ip(h.mac, h.ip, h.host[0] ? h.host : NULL);
    }
}

/* Best-effort hostnames (slow sockets), re-emitting the line with the name.
 * Called once per sweep pass: each still-unnamed host gets one more attempt, up
 * to ND_NAME_MAX_TRIES across passes, so a transient DNS/NBNS miss is retried
 * (but a host that simply has no PTR/NBNS name isn't hammered forever). The
 * index advances monotonically so no host is tried twice in one pass. */
static void resolve_pending_names(void)
{
    int i = 0;
    for (;;) {
        nd_host_t h;
        bool found = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (; i < s_host_count; i++) {
            if (s_hosts[i].host[0] == '\0' && s_hosts[i].name_tries < ND_NAME_MAX_TRIES) {
                s_hosts[i].name_tries++;
                h = s_hosts[i];
                found = true;
                i++; /* don't re-pick this host this pass */
                break;
            }
        }
        xSemaphoreGive(s_lock);
        if (!found || !s_connected) {
            break;
        }
        char name[ND_HOST_MAX] = "";
        if (!reverse_dns(h.ip, s_dns, name, sizeof(name))) {
            nbns_name(h.ip, name, sizeof(name));
        }
        if (name[0]) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            for (int i = 0; i < s_host_count; i++) {
                if (memcmp(s_hosts[i].mac, h.mac, 6) == 0) {
                    snprintf(s_hosts[i].host, sizeof(s_hosts[i].host), "%s", name);
                    break;
                }
            }
            xSemaphoreGive(s_lock);
            emit_ip(h.mac, h.ip, name);
        }
    }
}

/* Fill s_known_bits with the last octet of every host we already have on the
 * swept /24, so a retry pass can skip them. net24_h is the /24 base, host order. */
static void build_known_bitmap(uint32_t net24_h)
{
    memset(s_known_bits, 0, sizeof(s_known_bits));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_host_count; i++) {
        const uint32_t h = lwip_ntohl(s_hosts[i].ip);
        if ((h & 0xffffff00u) != net24_h) {
            continue; /* a stored host off the range we are sweeping */
        }
        const uint8_t oct = h & 0xff;
        s_known_bits[oct >> 3] |= (uint8_t)(1u << (oct & 7));
    }
    xSemaphoreGive(s_lock);
}

/* Merge the entries captured by the last tcpip_snapshot into the host table.
 * Runs on the discovery task after s_snapshot_done, so s_snap is stable. */
static void merge_snapshot(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_snap_count; i++) {
        nd_add_locked(s_snap[i].mac, s_snap[i].ip);
    }
    xSemaphoreGive(s_lock);
}

/* One pass over [first_h, last_h]: send a small batch, let it answer, read the
 * cache before the next batch evicts it, announcing IPs as they are learned.
 * Names (slow, best-effort) are attempted once at the end. */
static void sweep_pass(uint32_t first_h, uint32_t last_h)
{
    for (uint32_t lo = first_h; lo <= last_h; lo += ND_SWEEP_CHUNK) {
        s_chunk_lo = lo;
        s_chunk_hi = (lo + ND_SWEEP_CHUNK - 1 <= last_h) ? lo + ND_SWEEP_CHUNK - 1 : last_h;
        tcpip_callback(tcpip_send_chunk, NULL);
        vTaskDelay(pdMS_TO_TICKS(ND_CHUNK_REPLY_MS));
        if (!s_connected) {
            return;
        }
        xSemaphoreTake(s_snapshot_done, 0); /* drain any stale signal */
        tcpip_callback(tcpip_snapshot, NULL);
        /* Merge only when the snapshot actually completed: on a timeout s_snap
         * may still be mid-write on the tcpip thread. */
        if (xSemaphoreTake(s_snapshot_done, pdMS_TO_TICKS(1000)) == pdTRUE) {
            merge_snapshot();
        }
        emit_pending_ips();
    }
    resolve_pending_names();
}

static void sweep_once(void)
{
    const uint32_t my = s_my_ip, mask = s_netmask;
    if (!s_netif || mask == 0) {
        return;
    }
    const uint32_t net_n = my & mask;
    uint32_t first_h = lwip_ntohl(net_n) + 1;
    uint32_t last_h = lwip_ntohl(net_n | ~mask);
    last_h = last_h > first_h ? last_h - 1 : first_h;
    if (last_h - first_h > 254) { /* bound a wide subnet to the /24 around us */
        const uint32_t my_h = lwip_ntohl(my);
        first_h = (my_h & 0xffffff00u) + 1;
        last_h = (my_h & 0xffffff00u) + 254;
    }
    const uint32_t net24_h = first_h & 0xffffff00u; /* the range lies within one /24 */

    /* Probe a few times: a device in power-save often misses its one ARP in a
     * pass, so re-probe after a gap. The first pass covers everything; later
     * passes skip the hosts already found and re-probe only what is still
     * missing (and give still-unnamed hosts another resolve attempt). */
    for (int pass = 0; pass < ND_SWEEP_PASSES && s_connected; pass++) {
        if (pass > 0) {
            vTaskDelay(pdMS_TO_TICKS(ND_PASS_GAP_MS));
            if (!s_connected) {
                break;
            }
            build_known_bitmap(net24_h);
            s_skip_known = true;
        } else {
            s_skip_known = false;
        }
        sweep_pass(first_h, last_h);
    }
    s_skip_known = false;
}

static void netscan_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_connected) {
            sweep_once();
        }
    }
}

/* --- elicitation (its own task; ARP via the tcpip thread, ICMP via a raw socket) --- */

static void tcpip_send_arp_one(void *ctx)
{
    (void)ctx;
    const uint32_t tgt = s_arp_target;
    if (!s_netif || tgt == 0) {
        return;
    }
    ip4_addr_t ip;
    ip4_addr_set_u32(&ip, tgt);
    etharp_request(s_netif, &ip); /* the target answers -> the hunt sniffs the reply */
}

static void icmp_echo(int sock, uint32_t ip_net)
{
    if (sock < 0) {
        return;
    }
    static uint16_t seq;
    uint8_t pkt[16]; /* 8-byte ICMP header + 8-byte payload */
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 8; /* echo request */
    pkt[4] = 0xaa;
    pkt[5] = 0x55; /* identifier */
    const uint16_t s = seq++;
    pkt[6] = s >> 8;
    pkt[7] = s & 0xff;
    uint32_t sum = 0;
    for (size_t i = 0; i < sizeof(pkt); i += 2) {
        sum += (pkt[i] << 8) | pkt[i + 1];
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    const uint16_t csum = (uint16_t)~sum;
    pkt[2] = csum >> 8;
    pkt[3] = csum & 0xff;
    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = ip_net;
    sendto(sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&dst, sizeof(dst));
}

static void elicit_task(void *arg)
{
    (void)arg;
    int raw = -1;
    for (;;) {
        if (!s_elicit_active || !s_connected) {
            if (raw >= 0) {
                close(raw);
                raw = -1;
            }
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (raw < 0) {
            raw = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP); /* best-effort; ARP alone still works */
        }
        /* Snapshot the target under s_lock: a retarget (net_discover_elicit_start)
         * rewrites all 6 MAC bytes, so reading the array unguarded could tear it
         * mid-write. Take a coherent (mac, ip) pair, then look it up unlocked
         * (net_discover_lookup takes s_lock itself, which isn't recursive). */
        uint8_t mac[6];
        uint32_t ip;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        memcpy(mac, s_elicit_mac, 6);
        ip = s_elicit_ip;
        xSemaphoreGive(s_lock);
        /* The command carries only the MAC, so resolve it to an IP from the
         * sweep table (the app only lets elicitation start once that IP is
         * known, so the lookup succeeds); cache it for the next ticks, unless a
         * retarget swapped the target while we looked this one up. */
        if (ip == 0 && net_discover_lookup(mac, &ip) && ip) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (memcmp(mac, s_elicit_mac, 6) == 0) {
                s_elicit_ip = ip;
            }
            xSemaphoreGive(s_lock);
        }
        if (ip) {
            /* TODO (see TODO.md): this sends BOTH an ARP request and an ICMP echo
             * every interval, so a responsive target replies up to twice per
             * interval -- ~2x the on-air frames. Kept for now (more reply frames
             * = steadier hunt signal); the intended design was ARP-primary with
             * ICMP only as a fallback. */
            s_arp_target = ip;
            tcpip_callback(tcpip_send_arp_one, NULL);
            icmp_echo(raw, ip);
        }
        uint32_t iv = s_elicit_interval_ms;
        if (iv < 50) {
            iv = 50;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(iv)); /* next tick, or wake early on a state change */
    }
}

/* --- public API --- */

void net_discover_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_snapshot_done = xSemaphoreCreateBinary();
    xTaskCreate(netscan_task, "netscan", 4096, NULL, 4, &s_task);
    xTaskCreate(elicit_task, "elicit", 3072, NULL, 4, &s_elicit_task);
}

void net_discover_on_gotip(esp_netif_t *netif, uint32_t ip, uint32_t mask, uint32_t dns)
{
    s_netif = (struct netif *)esp_netif_get_netif_impl(netif);
    s_my_ip = ip;
    s_netmask = mask;
    s_dns = dns;
    s_connected = true;
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void net_discover_on_down(void)
{
    s_connected = false;
    net_discover_elicit_stop();
    if (s_elicit_task) {
        xTaskNotifyGive(s_elicit_task); /* let the elicit task close its socket */
    }
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_host_count = 0;
        xSemaphoreGive(s_lock);
    }
}

void net_discover_rescan(void)
{
    if (!s_connected || !s_task) {
        return;
    }
    /* The app clears its client list when a scan (re)starts, so re-announce every
     * host we already know: mark them unemitted and the sweep re-sends each line
     * (with its resolved name). Hosts still turn up again on the ARP re-probe,
     * but this makes the rebuilt list carry the IPs/names immediately. */
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < s_host_count; i++) {
            s_hosts[i].emitted = false;
        }
        xSemaphoreGive(s_lock);
    }
    xTaskNotifyGive(s_task);
}

bool net_discover_lookup(const uint8_t mac[6], uint32_t *out_ip)
{
    bool ok = false;
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_host_count; i++) {
        if (memcmp(s_hosts[i].mac, mac, 6) == 0) {
            *out_ip = s_hosts[i].ip;
            ok = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return ok;
}

void net_discover_elicit_start(const uint8_t mac[6], uint32_t interval_ms)
{
    if (interval_ms < 50) {
        interval_ms = 50;
    } else if (interval_ms > 1000) {
        interval_ms = 1000;
    }
    /* Update the MAC (and clear the cached IP) under s_lock so the elicit task
     * can't read a half-rewritten MAC. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_elicit_mac, mac, 6);
    s_elicit_ip = 0; /* the task resolves it (from the sweep table) */
    xSemaphoreGive(s_lock);
    s_elicit_interval_ms = interval_ms;
    s_elicit_active = true;
    if (s_elicit_task) {
        xTaskNotifyGive(s_elicit_task);
    }
    proto_sendf("ELICIT,ON,%u", (unsigned)interval_ms);
}

void net_discover_elicit_stop(void)
{
    const bool was_active = s_elicit_active;
    s_elicit_active = false;
    if (s_elicit_task) {
        xTaskNotifyGive(s_elicit_task);
    }
    if (was_active) {
        proto_sendf("ELICIT,OFF");
    }
}
