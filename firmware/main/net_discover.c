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
#define ND_TYPE_MAX 16               /* device-class label buffer incl. NUL */
#define ND_RANK_NONE 0xff            /* no mDNS/SSDP answer yet (ranks are k_labels indices) */
#define ND_PEND_MAX 8                /* mDNS/SSDP answers held for an IP not in the table yet */
#define ND_QUERY_DRAIN_MS 1000       /* listen for mDNS/SSDP answers after sending the queries */
#define ND_QUERY_SENDS 2             /* send the queries after this many passes (power-save misses) */
#define ND_RX_MAX 1500               /* one mDNS/SSDP datagram (a UDP truncate would lose records) */

typedef struct {
    uint8_t mac[6];
    uint32_t ip;         /* network byte order */
    bool emitted;        /* IP line sent */
    uint8_t name_tries;  /* reverse-DNS/NBNS attempts so far (capped at ND_NAME_MAX_TRIES) */
    uint8_t nrank;       /* rank of the mDNS answer behind host[], ND_RANK_NONE if not from mDNS */
    uint8_t trank;       /* rank of the mDNS/SSDP answer behind dtype[], ND_RANK_NONE if none */
    char host[ND_HOST_MAX];
    char dtype[ND_TYPE_MAX]; /* device class ("Chromecast", "Printer", ...) or empty */
} nd_host_t;

static nd_host_t s_hosts[ND_MAX_HOSTS];
static int s_host_count;

/* mDNS/SSDP answers are matched to a row by source IP. One can arrive before the
 * ARP sweep has mapped that IP to a MAC, so park it here (only ip/host/dtype/
 * nrank/trank are used) and let nd_add_locked fold it in when the host appears. */
static nd_host_t s_pend[ND_PEND_MAX];
static int s_pend_count, s_pend_next;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_snapshot_done;
static TaskHandle_t s_task;

static struct netif * volatile s_netif; /* lwip netif backing the STA (volatile pointer) */
static volatile uint32_t s_my_ip, s_netmask, s_dns; /* network byte order */
static volatile bool s_connected;

/* net_discover_on_gotip (event task) publishes the subnet config above as a set;
 * a sweep captures it as a coherent copy below. s_cfg_mux makes the publish and
 * the capture mutually exclusive so a DHCP renew mid-capture can't tear across
 * fields. The sweep then reads only s_sweep_*, so the whole sweep -- its range,
 * the per-chunk filters and the tcpip-thread callbacks -- agrees on one subnet.
 * (The elicit path keeps reading the live s_netif; it is independent of a sweep.) */
static portMUX_TYPE s_cfg_mux = portMUX_INITIALIZER_UNLOCKED;
static struct netif *s_sweep_netif;
static volatile uint32_t s_sweep_myip, s_sweep_mask, s_sweep_dns;

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

/* Reset a row's names for a (new) address and fold in any answer already parked
 * for that IP. */
static void nd_reset_names_locked(nd_host_t *h)
{
    h->name_tries = 0;
    h->nrank = ND_RANK_NONE;
    h->trank = ND_RANK_NONE;
    h->host[0] = '\0';
    h->dtype[0] = '\0';
    for (int i = 0; i < s_pend_count; i++) {
        if (s_pend[i].ip == h->ip) {
            h->nrank = s_pend[i].nrank;
            h->trank = s_pend[i].trank;
            memcpy(h->host, s_pend[i].host, sizeof(h->host));
            memcpy(h->dtype, s_pend[i].dtype, sizeof(h->dtype));
            s_pend[i] = s_pend[--s_pend_count];
            break;
        }
    }
}

static void nd_add_locked(const uint8_t mac[6], uint32_t ip)
{
    for (int i = 0; i < s_host_count; i++) {
        if (memcmp(s_hosts[i].mac, mac, 6) == 0) {
            if (s_hosts[i].ip != ip) {
                s_hosts[i].ip = ip; /* address changed: re-announce and re-resolve */
                s_hosts[i].emitted = false;
                nd_reset_names_locked(&s_hosts[i]);
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
    nd_reset_names_locked(h);
}

/* --- tcpip-thread callbacks (lwip core is only safe here) --- */

static void tcpip_send_chunk(void *ctx)
{
    (void)ctx;
    struct netif *nif = s_sweep_netif;
    if (!nif) {
        return;
    }
    for (uint32_t h = s_chunk_lo; h <= s_chunk_hi; h++) {
        const uint32_t addr_n = lwip_htonl(h);
        if (addr_n == s_sweep_myip) {
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
        if (nif != s_sweep_netif) {
            continue;
        }
        const uint32_t ip_n = ip4_addr_get_u32(ipaddr);
        if ((ip_n & s_sweep_mask) != (s_sweep_myip & s_sweep_mask)) {
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

static void emit_ip(const uint8_t mac[6], uint32_t ip_net, const char *name, const char *dtype)
{
    ip4_addr_t ipa;
    ip4_addr_set_u32(&ipa, ip_net);
    char ipstr[16];
    snprintf(ipstr, sizeof(ipstr), "%u.%u.%u.%u", (unsigned)ip4_addr1(&ipa),
             (unsigned)ip4_addr2(&ipa), (unsigned)ip4_addr3(&ipa), (unsigned)ip4_addr4(&ipa));
    char hex[2 * (ND_HOST_MAX - 1) + 1] = "";
    if (name && name[0]) {
        proto_hex_encode(name, strlen(name), hex, sizeof(hex));
    }
    /* IP,<mac>,<ip>[,<host_hex>[,<type>]]: the type needs the host slot, left
     * empty when only a type is known. */
    if (dtype && dtype[0]) {
        proto_sendf("IP,%02x:%02x:%02x:%02x:%02x:%02x,%s,%s,%s", mac[0], mac[1], mac[2], mac[3],
                    mac[4], mac[5], ipstr, hex, dtype);
    } else if (hex[0]) {
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
        /* Include the name and type if we already resolved them, so re-announcing
         * (e.g. for a freshly reopened client list) doesn't downgrade a row. */
        emit_ip(h.mac, h.ip, h.host[0] ? h.host : NULL, h.dtype);
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
        if (!reverse_dns(h.ip, s_sweep_dns, name, sizeof(name))) {
            nbns_name(h.ip, name, sizeof(name));
        }
        if (name[0]) {
            char dtype[ND_TYPE_MAX] = "";
            xSemaphoreTake(s_lock, portMAX_DELAY);
            for (int i = 0; i < s_host_count; i++) {
                if (memcmp(s_hosts[i].mac, h.mac, 6) == 0) {
                    snprintf(s_hosts[i].host, sizeof(s_hosts[i].host), "%s", name);
                    memcpy(dtype, s_hosts[i].dtype, sizeof(dtype));
                    break;
                }
            }
            xSemaphoreGive(s_lock);
            emit_ip(h.mac, h.ip, name, dtype);
        }
    }
}

/* --- mDNS / SSDP name & device-type discovery (discovery task) ---
 *
 * One multicast question reaches every host at once, and consumer gear (phones,
 * TVs, speakers, printers, cameras) answers these where reverse DNS / NBNS give
 * nothing. Queries go out from ephemeral ports, so no group join or port-5353
 * bind is needed: mDNS responders reply by legacy unicast straight back to us
 * (and the QU bit asks for it), SSDP replies are unicast anyway. Answers are
 * matched to a host row by source IP. */

/* Device-class labels; the enum value is the rank (a lower rank is more specific
 * and wins, so a Chromecast that also advertises _http stays "Chromecast"). */
enum {
    L_CAST, L_SONOS, L_ROKU, L_TV, L_AIRPLAY, L_HOMEKIT, L_SPEAKER,
    L_PRINTER, L_MEDIA, L_APPLE, L_ROUTER, L_NAS, L_COMPUTER, L_WEB,
};
static const char *const k_labels[] = {
    "Chromecast", "Sonos", "Roku", "Smart TV", "AirPlay", "HomeKit", "Speaker",
    "Printer", "Media player", "Apple device", "Router", "NAS", "Computer", "Web UI",
};

/* DNS-SD service types asked for in one packet; each answers with its instance
 * name (the PTR target). */
static const struct {
    const char *svc;
    uint8_t label;
} k_mdns[] = {
    {"_googlecast._tcp", L_CAST},       {"_sonos._tcp", L_SONOS},
    {"_airplay._tcp", L_AIRPLAY},       {"_raop._tcp", L_AIRPLAY},
    {"_hap._tcp", L_HOMEKIT},           {"_spotify-connect._tcp", L_SPEAKER},
    {"_ipp._tcp", L_PRINTER},           {"_pdl-datastream._tcp", L_PRINTER},
    {"_printer._tcp", L_PRINTER},       {"_companion-link._tcp", L_APPLE},
    {"_smb._tcp", L_NAS},               {"_afpovertcp._tcp", L_NAS},
    {"_ssh._tcp", L_COMPUTER},          {"_http._tcp", L_WEB},
};
#define MDNS_SVC_COUNT (sizeof(k_mdns) / sizeof(k_mdns[0]))

/* SSDP: targeted searches (not ssdp:all, which makes every device answer once per
 * service and would overflow the small UDP receive queue). A reply is classified
 * by a case-insensitive substring of its ST/USN/SERVER headers. */
static const char *const k_ssdp_st[] = {
    "urn:dial-multiscreen-org:service:dial:1",
    "urn:schemas-upnp-org:device:MediaRenderer:1",
    "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
};
static const struct {
    const char *needle;
    uint8_t label;
} k_ssdp[] = {
    {"roku", L_ROKU},
    {"sonos", L_SONOS},
    {"dial-multiscreen", L_TV},
    {"MediaRenderer", L_MEDIA},
    {"InternetGatewayDevice", L_ROUTER},
};

static int s_mdns_sock = -1, s_ssdp_sock = -1;
/* Shared by the discovery task only (static: the 4 KB task stack is tight). */
static uint8_t s_rx[ND_RX_MAX + 1];
static uint8_t s_tx[640];

/* Fold one answer (name may be empty, as for SSDP) into the row for `ip`, or park
 * it in s_pend if no row has that IP yet. A lower rank replaces a higher one. */
static void nd_apply_answer(uint32_t ip, const char *name, uint8_t rank)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    nd_host_t *h = NULL;
    bool parked = false;
    for (int i = 0; i < s_host_count && !h; i++) {
        if (s_hosts[i].ip == ip) {
            h = &s_hosts[i];
        }
    }
    for (int i = 0; i < s_pend_count && !h; i++) {
        if (s_pend[i].ip == ip) {
            h = &s_pend[i];
            parked = true;
        }
    }
    if (!h) {
        const int slot = s_pend_count < ND_PEND_MAX ? s_pend_count++ : s_pend_next++ % ND_PEND_MAX;
        h = &s_pend[slot];
        memset(h, 0, sizeof(*h));
        h->ip = ip;
        h->nrank = ND_RANK_NONE;
        h->trank = ND_RANK_NONE;
        parked = true;
    }
    bool changed = false;
    if (rank < h->trank) {
        snprintf(h->dtype, sizeof(h->dtype), "%s", k_labels[rank]);
        h->trank = rank;
        changed = true;
    }
    if (name[0] && rank < h->nrank) {
        snprintf(h->host, sizeof(h->host), "%s", name);
        h->nrank = rank;
        changed = true;
    }
    if (changed && !parked) {
        h->emitted = false; /* the caller's emit_pending_ips re-sends the row */
    }
    xSemaphoreGive(s_lock);
}

/* Copy `n` bytes of a device-supplied name into `out`: control bytes are dropped
 * and the typographic apostrophe Apple puts in default names ("Max\xe2\x80\x99s
 * iPhone", U+2019) becomes a plain one, which the Flipper font can draw. */
static void nd_copy_name(const uint8_t *src, int n, char *out, size_t out_sz)
{
    size_t o = 0;
    for (int i = 0; i < n && o + 1 < out_sz; i++) {
        if (src[i] == 0xe2 && i + 2 < n && src[i + 1] == 0x80 &&
            (src[i + 2] == 0x98 || src[i + 2] == 0x99)) {
            out[o++] = '\'';
            i += 2;
        } else if (src[i] >= 0x20) {
            out[o++] = (char)src[i];
        }
    }
    out[o] = '\0';
}

/* The first label of the (possibly compressed) name at `off`, verbatim: an
 * instance name may itself contain dots, which dns_read_name would conflate with
 * label separators. Control bytes are dropped. */
static bool dns_first_label(const uint8_t *pkt, int len, int off, char *out, size_t out_sz)
{
    for (int guard = 0; off < len && guard < 16; guard++) {
        const uint8_t c = pkt[off];
        if ((c & 0xc0) == 0xc0) {
            if (off + 1 >= len) {
                return false;
            }
            off = ((c & 0x3f) << 8) | pkt[off + 1];
            continue;
        }
        if (c == 0 || (c & 0xc0) || off + 1 + c > len) {
            return false;
        }
        nd_copy_name(pkt + off + 1, c, out, out_sz);
        return out[0] != '\0';
    }
    return false;
}

/* The value of the "key=value" string in a TXT rdata block, if present. */
static void txt_find(const uint8_t *t, int len, const char *key, char *out, size_t out_sz)
{
    const int kl = (int)strlen(key);
    for (int i = 0; i < len;) {
        const int l = t[i++];
        if (i + l > len) {
            break;
        }
        if (l > kl && memcmp(t + i, key, kl) == 0) {
            nd_copy_name(t + i + kl, l - kl, out, out_sz);
            return;
        }
        i += l;
    }
}

static uint8_t mdns_rank(const char *owner)
{
    for (size_t i = 0; i < MDNS_SVC_COUNT; i++) {
        const size_t l = strlen(k_mdns[i].svc);
        if (strncmp(owner, k_mdns[i].svc, l) == 0 && (owner[l] == '.' || owner[l] == '\0')) {
            return k_mdns[i].label;
        }
    }
    return ND_RANK_NONE;
}

/* A DNS response (mDNS is plain DNS wire format): walk every record, take the
 * best-ranked service's PTR instance name, and prefer a TXT friendly name
 * ("fn=", set by Chromecast) over it. */
static void mdns_handle(const uint8_t *p, int len, uint32_t src)
{
    if (len < 12 || !(p[2] & 0x80)) {
        return; /* not a response */
    }
    const int qd = (p[4] << 8) | p[5];
    int records = ((p[6] << 8) | p[7]) + ((p[8] << 8) | p[9]) + ((p[10] << 8) | p[11]);
    char owner[48];
    int off = 12;
    for (int i = 0; i < qd; i++) { /* a legacy-unicast reply echoes our questions */
        const int used = dns_read_name(p, len, off, owner, sizeof(owner));
        if (used < 0) {
            return;
        }
        off += used + 4; /* + QTYPE, QCLASS */
    }
    uint8_t best = ND_RANK_NONE;
    char inst[ND_HOST_MAX] = "", fn[ND_HOST_MAX] = "";
    for (; records > 0 && off < len; records--) {
        const int used = dns_read_name(p, len, off, owner, sizeof(owner));
        if (used < 0 || off + used + 10 > len) {
            break;
        }
        const int type = (p[off + used] << 8) | p[off + used + 1];
        const int rdlen = (p[off + used + 8] << 8) | p[off + used + 9];
        const int rd = off + used + 10;
        if (rd + rdlen > len) {
            break;
        }
        if (type == 12) { /* PTR: service type -> instance */
            const uint8_t r = mdns_rank(owner);
            char tmp[ND_HOST_MAX];
            if (r < best && dns_first_label(p, len, rd, tmp, sizeof(tmp))) {
                best = r;
                memcpy(inst, tmp, sizeof(inst));
            }
        } else if (type == 16) { /* TXT */
            txt_find(p + rd, rdlen, "fn=", fn, sizeof(fn));
        }
        off = rd + rdlen;
    }
    if (best == ND_RANK_NONE) {
        return;
    }
    char *name = fn[0] ? fn : inst;
    if (strlen(name) > 13 && name[12] == '@') { /* AirPlay (_raop): "AABBCCDDEEFF@Name" */
        bool hex = true;
        for (int i = 0; i < 12; i++) {
            hex = hex && ((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'A' && name[i] <= 'F') ||
                          (name[i] >= 'a' && name[i] <= 'f'));
        }
        if (hex) {
            name += 13;
        }
    }
    nd_apply_answer(src, name, best);
}

static bool ci_contains(const char *hay, const char *needle)
{
    const size_t nl = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nl && hay[i] && (hay[i] | 0x20) == (needle[i] | 0x20)) {
            i++;
        }
        if (i == nl) {
            return true;
        }
    }
    return false;
}

/* An SSDP "HTTP/1.1 200 OK" reply: classify it by its headers. It names no
 * friendly name (that needs the device-description XML), only a type. */
static void ssdp_handle(const char *msg, uint32_t src)
{
    if (strncmp(msg, "HTTP/1.1 200", 12) != 0) {
        return;
    }
    uint8_t best = ND_RANK_NONE;
    for (size_t i = 0; i < sizeof(k_ssdp) / sizeof(k_ssdp[0]); i++) {
        if (k_ssdp[i].label < best && ci_contains(msg, k_ssdp[i].needle)) {
            best = k_ssdp[i].label;
        }
    }
    if (best != ND_RANK_NONE) {
        nd_apply_answer(src, "", best);
    }
}

static int nd_udp_socket(uint8_t mcast_ttl)
{
    const int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock >= 0) {
#ifdef IP_MULTICAST_TTL
        setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &mcast_ttl, sizeof(mcast_ttl));
#else
        (void)mcast_ttl;
#endif
    }
    return sock;
}

/* One mDNS packet carrying a PTR question per service type (QU bit set). */
static int mdns_build_query(uint8_t *q, size_t cap)
{
    int n = 0;
    q[n++] = 0x57; /* ID (echoed by a legacy-unicast responder, otherwise unused) */
    q[n++] = 0x48;
    q[n++] = 0x00; /* flags: standard query */
    q[n++] = 0x00;
    q[n++] = 0x00;
    q[n++] = (uint8_t)MDNS_SVC_COUNT;
    for (int i = 0; i < 6; i++) {
        q[n++] = 0x00; /* AN/NS/AR counts */
    }
    for (size_t s = 0; s < MDNS_SVC_COUNT; s++) {
        const char *svc = k_mdns[s].svc;
        while (*svc) { /* "_name._tcp" labels, then "local" */
            const char *dot = strchr(svc, '.');
            const int l = dot ? (int)(dot - svc) : (int)strlen(svc);
            if (n + 1 + l > (int)cap) {
                return -1;
            }
            q[n++] = (uint8_t)l;
            memcpy(q + n, svc, l);
            n += l;
            svc += l + (dot ? 1 : 0);
        }
        if (n + 6 + 5 > (int)cap) {
            return -1;
        }
        q[n++] = 5;
        memcpy(q + n, "local", 5);
        n += 5;
        q[n++] = 0x00; /* root */
        q[n++] = 0x00;
        q[n++] = 0x0c; /* QTYPE = PTR */
        q[n++] = 0x80; /* QCLASS = IN, QU (unicast response wanted) */
        q[n++] = 0x01;
    }
    return n;
}

/* Fire the mDNS question and the SSDP searches. The sockets stay open (they
 * collect answers across the drain and the pass gaps) until nd_query_close. */
static void nd_query_send(void)
{
    if (s_mdns_sock < 0) {
        s_mdns_sock = nd_udp_socket(255);
    }
    if (s_ssdp_sock < 0) {
        s_ssdp_sock = nd_udp_socket(2);
    }
    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    if (s_mdns_sock >= 0) {
        const int n = mdns_build_query(s_tx, sizeof(s_tx));
        dst.sin_port = lwip_htons(5353);
        dst.sin_addr.s_addr = lwip_htonl(0xe00000fbu); /* 224.0.0.251 */
        if (n > 0) {
            sendto(s_mdns_sock, s_tx, n, 0, (struct sockaddr *)&dst, sizeof(dst));
        }
    }
    if (s_ssdp_sock >= 0) {
        dst.sin_port = lwip_htons(1900);
        dst.sin_addr.s_addr = lwip_htonl(0xeffffffau); /* 239.255.255.250 */
        for (size_t i = 0; i < sizeof(k_ssdp_st) / sizeof(k_ssdp_st[0]); i++) {
            const int n = snprintf((char *)s_tx, sizeof(s_tx),
                                   "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
                                   "MAN: \"ssdp:discover\"\r\nMX: 1\r\nST: %s\r\n\r\n",
                                   k_ssdp_st[i]);
            sendto(s_ssdp_sock, s_tx, n, 0, (struct sockaddr *)&dst, sizeof(dst));
        }
    }
}

static void nd_query_close(void)
{
    if (s_mdns_sock >= 0) {
        close(s_mdns_sock);
        s_mdns_sock = -1;
    }
    if (s_ssdp_sock >= 0) {
        close(s_ssdp_sock);
        s_ssdp_sock = -1;
    }
}

static void nd_query_recv(int sock, bool is_mdns)
{
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    const int n = recvfrom(sock, s_rx, ND_RX_MAX, 0, (struct sockaddr *)&from, &fl);
    if (n <= 0) {
        return;
    }
    if (is_mdns) {
        mdns_handle(s_rx, n, from.sin_addr.s_addr);
    } else {
        s_rx[n] = '\0';
        ssdp_handle((const char *)s_rx, from.sin_addr.s_addr);
    }
}

/* Wait `ms`, consuming mDNS/SSDP answers as they arrive and announcing the rows
 * they update. With no sockets open this is just a delay (it also stands in for
 * the pause between sweep passes). */
static void nd_query_drain(uint32_t ms)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t total = pdMS_TO_TICKS(ms);
    for (;;) {
        const TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= total || !s_connected) {
            break;
        }
        if (s_mdns_sock < 0 && s_ssdp_sock < 0) {
            vTaskDelay(total - elapsed);
            break;
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        if (s_mdns_sock >= 0) {
            FD_SET(s_mdns_sock, &rfds);
            maxfd = s_mdns_sock;
        }
        if (s_ssdp_sock >= 0) {
            FD_SET(s_ssdp_sock, &rfds);
            maxfd = s_ssdp_sock > maxfd ? s_ssdp_sock : maxfd;
        }
        const uint32_t left_ms = (total - elapsed) * portTICK_PERIOD_MS;
        struct timeval tv = {.tv_sec = left_ms / 1000, .tv_usec = (left_ms % 1000) * 1000};
        const int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            vTaskDelay(pdMS_TO_TICKS(20)); /* don't spin on a socket error */
            continue;
        }
        if (r == 0) {
            continue; /* timed out: the time check above ends the loop */
        }
        if (s_mdns_sock >= 0 && FD_ISSET(s_mdns_sock, &rfds)) {
            nd_query_recv(s_mdns_sock, true);
        }
        if (s_ssdp_sock >= 0 && FD_ISSET(s_ssdp_sock, &rfds)) {
            nd_query_recv(s_ssdp_sock, false);
        }
        emit_pending_ips();
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

/* Whether a chunk [lo, hi] has anything worth probing: any address that is not
 * ours and, on a retry pass, not already in the table. Lets sweep_pass skip the
 * reply wait + snapshot for chunks that would send no ARP at all (on a populated
 * subnet the retry passes are then nearly instant). Mirrors the same skip test
 * in tcpip_send_chunk, against the s_known_bits snapshot this pass was built on. */
static bool chunk_has_unknown(uint32_t lo, uint32_t hi)
{
    if (!s_skip_known) {
        return true; /* first pass: probe everything */
    }
    for (uint32_t h = lo; h <= hi; h++) {
        const uint32_t addr_n = lwip_htonl(h);
        if (addr_n == s_sweep_myip) {
            continue;
        }
        if (!(s_known_bits[(h & 0xff) >> 3] & (1u << (h & 7)))) {
            return true;
        }
    }
    return false;
}

/* One pass over [first_h, last_h]: send a small batch, let it answer, read the
 * cache before the next batch evicts it, announcing IPs as they are learned.
 * Names (slow, best-effort) are left to sweep_once, after the mDNS/SSDP queries. */
static void sweep_pass(uint32_t first_h, uint32_t last_h)
{
    for (uint32_t lo = first_h; lo <= last_h; lo += ND_SWEEP_CHUNK) {
        const uint32_t hi = (lo + ND_SWEEP_CHUNK - 1 <= last_h) ? lo + ND_SWEEP_CHUNK - 1 : last_h;
        if (!chunk_has_unknown(lo, hi)) {
            continue; /* retry pass: every address here is already known */
        }
        s_chunk_lo = lo;
        s_chunk_hi = hi;
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
}

static void sweep_once(void)
{
    /* Capture the subnet config once, as a coherent set, under s_cfg_mux (pairs
     * with the publish in net_discover_on_gotip). Everything this sweep does --
     * the range below, the per-chunk filters, the tcpip callbacks -- then reads
     * the s_sweep_* copy, so a lease change mid-sweep can't mix old/new fields. */
    portENTER_CRITICAL(&s_cfg_mux);
    struct netif *nif = s_netif;
    const uint32_t my = s_my_ip, mask = s_netmask, dns = s_dns;
    portEXIT_CRITICAL(&s_cfg_mux);
    if (!nif || mask == 0) {
        return;
    }
    s_sweep_netif = nif;
    s_sweep_myip = my;
    s_sweep_mask = mask;
    s_sweep_dns = dns;
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
     * missing (and give still-unnamed hosts another resolve attempt).
     *
     * The mDNS/SSDP queries go out after the first passes' ARP chunks, so most
     * answering IPs are already in the table, and before the slow PTR/NBNS
     * lookups, which then skip every host the multicast answers named. The gap
     * between passes listens for stragglers instead of idling. */
    for (int pass = 0; pass < ND_SWEEP_PASSES && s_connected; pass++) {
        if (pass > 0) {
            nd_query_drain(ND_PASS_GAP_MS);
            if (!s_connected) {
                break;
            }
            build_known_bitmap(net24_h);
            s_skip_known = true;
        } else {
            s_skip_known = false;
        }
        sweep_pass(first_h, last_h);
        if (!s_connected) {
            break;
        }
        if (pass < ND_QUERY_SENDS) {
            nd_query_send();
            nd_query_drain(ND_QUERY_DRAIN_MS);
        }
        resolve_pending_names();
    }
    nd_query_close();
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
    struct netif *nif = s_netif;
    const uint32_t tgt = s_arp_target;
    if (!nif || tgt == 0) {
        return;
    }
    ip4_addr_t ip;
    ip4_addr_set_u32(&ip, tgt);
    etharp_request(nif, &ip); /* the target answers -> the hunt sniffs the reply */
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
    /* Publish the config as a set under s_cfg_mux so a sweep's capture can't read
     * a half-updated subnet. Resolve the netif impl before the lock to keep the
     * critical section to four stores. */
    struct netif *nif = (struct netif *)esp_netif_get_netif_impl(netif);
    portENTER_CRITICAL(&s_cfg_mux);
    s_netif = nif;
    s_my_ip = ip;
    s_netmask = mask;
    s_dns = dns;
    portEXIT_CRITICAL(&s_cfg_mux);
    s_connected = true; /* set last: a reader seeing connected then sees full config */
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
        s_pend_count = 0;
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
