#include "radio.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "led.h"
#include "net_discover.h"
#include "proto.h"

#define HUNT_PERIOD_MS RADIO_HUNT_PERIOD_MS
/* Defaults; HUNT/DISCOVER may override them per command. */
#define HUNT_LOST_PERIODS 80 /* 8 s without the target -> start hopping again */
#define HUNT_HOP_PERIODS 3   /* dwell per channel while searching */
#define SPECTRUM_DWELL_MS 120
#define DISCOVER_DWELL_MS 120
#define DISCOVER_MAX 192
#define DISCOVER_REPORT_MAX 96

#define UNIQUE_SLOTS 512
#define UNIQUE_LIMIT 384

#define MAX_CHANNEL CONFIG_WH_MAX_CHANNEL

typedef enum {
    MODE_IDLE,
    MODE_HUNT,
    MODE_SPECTRUM,
    MODE_DISCOVER,
} radio_mode_t;

typedef enum {
    CMD_STOP,
    CMD_HUNT,
    CMD_SPECTRUM,
    CMD_DISCOVER,
    CMD_CONNECT,
    CMD_DISCONNECT,
    CMD_WPS_START,
    CMD_WPS_STOP,
    CMD_SLEEP,
} radio_cmd_type_t;

typedef struct {
    radio_cmd_type_t type;
    uint8_t mac[6]; /* hunt target, discover CLIENTS bssid, or connect bssid */
    uint8_t channel;
    radio_discover_filter_t filter;
    bool pinned;
    uint16_t hop_periods;  /* hunt: 0 = HUNT_HOP_PERIODS */
    uint16_t lost_periods; /* hunt: 0 = HUNT_LOST_PERIODS */
    uint16_t dwell_ms;     /* discover: 0 = DISCOVER_DWELL_MS */
    bool has_bssid;        /* connect: mac[] holds a BSSID to lock onto */
    char ssid[33];         /* connect: AP SSID */
    char psk[65];          /* connect: AP passphrase/PSK ("" = open); 64 + NUL */
} radio_cmd_t;

#define DEV_FLAG_AP 0x01         /* transmitted like an access point */
#define DEV_FLAG_DS_CHANNEL 0x02 /* channel taken from the beacon's DS parameter set */

/* Security, from a beacon's RSN/WPA IEs and the Privacy capability bit. */
typedef enum {
    SEC_UNKNOWN = 0, /* not an AP, or no beacon parsed yet */
    SEC_OPEN,
    SEC_WEP,
    SEC_WPA,
    SEC_WPA2,
    SEC_WPA3,
} dev_security_t;

/* Highest PHY a beacon advertises. 2.4 GHz only, so VHT is never seen here. */
typedef enum {
    PHY_UNKNOWN = 0,
    PHY_LEGACY, /* 802.11b/g: no HT/VHT/HE element */
    PHY_HT,     /* 802.11n */
    PHY_VHT,    /* 802.11ac */
    PHY_HE,     /* 802.11ax */
} dev_phy_t;

typedef struct {
    uint8_t mac[6];
    uint8_t channel;
    int8_t rssi_max;
    uint16_t frames;
    uint8_t flags;
    uint8_t security; /* dev_security_t, set from beacons */
    uint8_t phy;      /* dev_phy_t, set from beacons */
    char ssid[33];
} discover_dev_t;

static QueueHandle_t s_cmd_queue;

/* Shared with the promiscuous callback (runs in the WiFi task). */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile radio_mode_t s_mode = MODE_IDLE;
static uint8_t s_target[6];

static struct {
    uint32_t frames;
    int32_t rssi_sum;
    int32_t noise_sum;
    int8_t rssi_last;
    int8_t rssi_max;
} s_hunt;

static struct {
    uint32_t frames;
    uint32_t bytes;
    int8_t rssi_max;
} s_spec;

static uint32_t s_unique_table[UNIQUE_SLOTS];
static uint16_t s_unique_count;

/* Written only by the promiscuous callback while in MODE_DISCOVER. The radio
 * task reads the table once promiscuous mode is off again. */
static discover_dev_t s_devs[DISCOVER_MAX];
static volatile uint16_t s_dev_count;
static volatile radio_discover_filter_t s_disc_filter;
/* Target AP for DISCOVER_FILTER_CLIENTS. Set before promiscuous mode is enabled
 * (same hand-off as s_target), then read by the callback. */
static uint8_t s_disc_bssid[6];

/* Radio-task-only state. */
static uint8_t s_channel = 1;
static bool s_hunt_locked;
static bool s_hunt_pinned;
static uint8_t s_disc_channel; /* 0 = hop */
static uint32_t s_hunt_quiet_periods;
static uint32_t s_hunt_hop_periods;
static uint32_t s_hunt_hop_limit = HUNT_HOP_PERIODS;
static uint32_t s_hunt_lost_limit = HUNT_LOST_PERIODS;
static uint32_t s_disc_dwell_ms = DISCOVER_DWELL_MS;
static uint16_t s_disc_cursor;     /* table slots already examined */
static uint16_t s_disc_streamed;   /* wanted DEV lines already sent */
static uint16_t s_disc_prev_count; /* table size seen at the previous tick */

/* Station-association state, driven by the WiFi/IP event handlers (which run in
 * the default event loop task, not the radio task). While associated the AP
 * pins our channel, so set_channel() must not call esp_wifi_set_channel() -- it
 * would fail; the hunt and client scans simply ride the connection's channel. */
static esp_netif_t *s_sta_netif;
static volatile bool s_associated;      /* L2 link up: gates set_channel() */
static volatile bool s_connecting;      /* connect requested, awaiting a result */
static volatile bool s_got_ip;          /* DHCP lease obtained */
static volatile bool s_suppress_down;   /* swallow one self-initiated STA_DISCONNECTED */
static volatile bool s_wps_active;      /* a WPS PBC exchange is in progress */
static volatile uint8_t s_conn_channel; /* the AP's channel while associated */
static char s_conn_ssid[33];            /* SSID we joined (for INFO) */
static char s_conn_ip[16];              /* our IPv4 while connected (for INFO) */

static uint32_t mac_hash(const uint8_t *mac)
{
    /* FNV-1a; 0 marks an empty slot, so never return it. */
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; i++) {
        h = (h ^ mac[i]) * 16777619u;
    }
    return h ? h : 1;
}

/* Caller holds s_mux. */
static void unique_insert(const uint8_t *mac)
{
    if (mac[0] & 0x01) {
        return; /* group address, not a real transmitter */
    }
    if (s_unique_count >= UNIQUE_LIMIT) {
        return;
    }
    uint32_t h = mac_hash(mac);
    uint32_t slot = h % UNIQUE_SLOTS;
    while (s_unique_table[slot] != 0) {
        if (s_unique_table[slot] == h) {
            return;
        }
        slot = (slot + 1) % UNIQUE_SLOTS;
    }
    s_unique_table[slot] = h;
    s_unique_count++;
}

/* Scan an RSN element's AKM suite list for an AKM that marks WPA3: SAE (8),
 * FT-SAE (9), OWE (18) or Suite-B (11, 12). Layout: version(2), group cipher(4),
 * pairwise count(2) + pairwise suites(4*n), AKM count(2) + AKM suites(4*m). Each
 * suite is a 3-byte OUI (00-0F-AC for 802.11) plus a 1-byte selector. */
static bool rsn_is_wpa3(const uint8_t *ie, uint8_t len)
{
    uint16_t pos = 2 + 4; /* version + group cipher suite */
    if (pos + 2 > len) {
        return false;
    }
    uint16_t pairwise = ie[pos] | (ie[pos + 1] << 8);
    pos += 2 + 4 * pairwise;
    if (pos + 2 > len) {
        return false;
    }
    uint16_t akm = ie[pos] | (ie[pos + 1] << 8);
    pos += 2;
    for (uint16_t i = 0; i < akm && pos + 4 <= len; i++, pos += 4) {
        if (ie[pos] == 0x00 && ie[pos + 1] == 0x0f && ie[pos + 2] == 0xac) {
            const uint8_t sel = ie[pos + 3];
            if (sel == 8 || sel == 9 || sel == 11 || sel == 12 || sel == 18) {
                return true;
            }
        }
    }
    return false;
}

/* SSIDs are raw bytes; keep the line protocol intact. */
static void copy_ssid(char *dst, const uint8_t *src, size_t len)
{
    if (len > 32) {
        len = 32;
    }
    for (size_t i = 0; i < len; i++) {
        uint8_t c = src[i];
        dst[i] = (c < 0x20 || c == 0x7f) ? '?' : (char)c;
    }
    dst[len] = '\0';
}

static void discover_frame(const uint8_t *hdr, uint16_t len, int8_t rssi, uint8_t channel,
                           bool is_mgmt)
{
    const uint8_t *tx = hdr + 10;
    if (tx[0] & 0x01) {
        return;
    }

    if (s_disc_filter == DISCOVER_FILTER_CLIENTS) {
        /* A client of the target AP: a data frame headed to the distribution
         * system (ToDS=1, FromDS=0) whose receiver (addr1) is the BSSID. The
         * transmitter (addr2 = tx) is the client; idle clients still send
         * power-save Null frames this way. Management frames (DS bits 0) and
         * AP->STA data (FromDS) fail the check and are left out. */
        if ((hdr[1] & 0x03) != 0x01 || memcmp(hdr + 4, s_disc_bssid, 6) != 0) {
            return;
        }
    }

    /* Beacon (0x80) or probe response (0x50) carry SSID and channel IEs. */
    const bool is_beacon = is_mgmt && (hdr[0] == 0x80 || hdr[0] == 0x50);
    /* Other frames that only an AP sends: management frames from the BSSID itself
     * (addr2 == addr3), and data frames going from the distribution system
     * (FromDS=1, ToDS=0). */
    const bool from_ap = is_beacon || (is_mgmt && memcmp(tx, hdr + 16, 6) == 0) ||
                         (!is_mgmt && (hdr[1] & 0x03) == 0x02);

    if (s_disc_filter == DISCOVER_FILTER_AP && !from_ap) {
        return; /* keep the table for access points */
    }

    discover_dev_t *dev = NULL;
    const uint16_t count = s_dev_count;
    for (uint16_t i = 0; i < count; i++) {
        if (memcmp(s_devs[i].mac, tx, 6) == 0) {
            dev = &s_devs[i];
            break;
        }
    }
    if (!dev) {
        if (count >= DISCOVER_MAX) {
            return;
        }
        dev = &s_devs[count];
        memset(dev, 0, sizeof(*dev));
        memcpy(dev->mac, tx, 6);
        dev->rssi_max = rssi;
        dev->channel = channel;
        s_dev_count = count + 1;
    }

    if (dev->frames < UINT16_MAX) {
        dev->frames++;
    }
    if (rssi >= dev->rssi_max) {
        /* Neighbouring channels overlap; the strongest copy is the real channel. */
        dev->rssi_max = rssi;
        if (!(dev->flags & DEV_FLAG_DS_CHANNEL)) {
            dev->channel = channel;
        }
    }
    if (from_ap) {
        dev->flags |= DEV_FLAG_AP;
    }

    if (is_beacon) {
        const uint16_t end = len > 4 ? len - 4 : 0; /* strip FCS */
        /* Capability info sits in the fixed fields at offset 34; bit 4 = Privacy. */
        const bool privacy = end >= 36 && (hdr[34] & 0x10);
        bool rsn = false, wpa3 = false, wpa1 = false;
        bool ht = false, vht = false, he = false;
        uint16_t pos = 24 + 12; /* header + fixed fields */
        while (pos + 2 <= end) {
            const uint8_t tag = hdr[pos];
            const uint8_t tag_len = hdr[pos + 1];
            if (pos + 2 + tag_len > end) {
                break;
            }
            const uint8_t *ie = hdr + pos + 2;
            if (tag == 0 && !dev->ssid[0] && tag_len && ie[0] != 0) {
                /* Hidden networks send an empty or zero-filled SSID. */
                copy_ssid(dev->ssid, ie, tag_len);
            } else if (tag == 3 && tag_len == 1 && ie[0] >= 1 && ie[0] <= MAX_CHANNEL) {
                dev->channel = ie[0]; /* DS parameter set: the AP's real channel */
                dev->flags |= DEV_FLAG_DS_CHANNEL;
            } else if (tag == 48) {
                rsn = true; /* RSN element: WPA2, or WPA3 if an SAE/OWE AKM is listed */
                wpa3 = rsn_is_wpa3(ie, tag_len);
            } else if (tag == 221 && tag_len >= 4 && ie[0] == 0x00 && ie[1] == 0x50 &&
                       ie[2] == 0xf2 && ie[3] == 0x01) {
                wpa1 = true; /* Microsoft WPA v1 vendor element */
            } else if (tag == 45) {
                ht = true; /* HT Capabilities -> 802.11n */
            } else if (tag == 191) {
                vht = true; /* VHT Capabilities -> 802.11ac */
            } else if (tag == 255 && tag_len >= 1 && ie[0] == 35) {
                he = true; /* HE Capabilities extension element -> 802.11ax */
            }
            pos += 2 + tag_len;
        }

        dev->security = rsn      ? (wpa3 ? SEC_WPA3 : SEC_WPA2)
                        : wpa1    ? SEC_WPA
                        : privacy ? SEC_WEP
                                  : SEC_OPEN;
        dev->phy = he ? PHY_HE : vht ? PHY_VHT : ht ? PHY_HT : PHY_LEGACY;
    }
}

static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const int8_t rssi = pkt->rx_ctrl.rssi;
    const uint16_t len = pkt->rx_ctrl.sig_len;
    const uint8_t *hdr = pkt->payload;
    /* Management and data frames carry the transmitter address in addr2. */
    const bool has_tx = (type == WIFI_PKT_MGMT || type == WIFI_PKT_DATA) && len >= 24;
    const uint8_t *tx = hdr + 10;

    if (s_mode == MODE_DISCOVER) {
        if (has_tx) {
            discover_frame(hdr, len, rssi, pkt->rx_ctrl.channel, type == WIFI_PKT_MGMT);
        }
        return;
    }

    portENTER_CRITICAL(&s_mux);
    if (s_mode == MODE_HUNT) {
        if (has_tx && memcmp(tx, s_target, 6) == 0) {
            s_hunt.frames++;
            s_hunt.rssi_sum += rssi;
            s_hunt.noise_sum += pkt->rx_ctrl.noise_floor;
            s_hunt.rssi_last = rssi;
            if (s_hunt.frames == 1 || rssi > s_hunt.rssi_max) {
                s_hunt.rssi_max = rssi;
            }
        }
    } else if (s_mode == MODE_SPECTRUM) {
        s_spec.frames++;
        s_spec.bytes += len;
        if (s_spec.frames == 1 || rssi > s_spec.rssi_max) {
            s_spec.rssi_max = rssi;
        }
        if (has_tx) {
            unique_insert(tx);
        }
    }
    portEXIT_CRITICAL(&s_mux);
}

/* Copy an SSID for a single-field INFO row: commas and control characters would
 * break the "one message per line, value has no comma" convention. */
static void conn_ssid_sanitized(char *dst, size_t dst_sz, const char *src)
{
    size_t i = 0;
    for (; src[i] && i + 1 < dst_sz; i++) {
        const unsigned char c = (unsigned char)src[i];
        dst[i] = (c == ',' || c < 0x20 || c == 0x7f) ? '?' : (char)c;
    }
    dst[i] = '\0';
}

/* WPS succeeded: pull the SSID/passphrase (from the event, or -- the single-AP
 * case where the driver set them and passes no data -- back out of the stored
 * config), report them for saving, and associate. Runs in the event task. */
static void wps_success(const wifi_event_sta_wps_er_success_t *e)
{
    esp_wifi_wps_disable();
    s_wps_active = false;

    char ssid[33] = {0};
    char psk[65] = {0};
    if (e && e->ap_cred_cnt >= 1) {
        const size_t sl = strnlen((const char *)e->ap_cred[0].ssid, sizeof(e->ap_cred[0].ssid));
        const size_t pl =
            strnlen((const char *)e->ap_cred[0].passphrase, sizeof(e->ap_cred[0].passphrase));
        memcpy(ssid, e->ap_cred[0].ssid, sl);
        memcpy(psk, e->ap_cred[0].passphrase, pl);
        wifi_config_t wc = {0};
        memcpy(wc.sta.ssid, e->ap_cred[0].ssid, sl);
        memcpy(wc.sta.password, e->ap_cred[0].passphrase, pl);
        wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
        esp_wifi_set_config(WIFI_IF_STA, &wc);
    } else {
        wifi_config_t wc = {0};
        if (esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK) {
            memcpy(ssid, wc.sta.ssid, strnlen((const char *)wc.sta.ssid, sizeof(wc.sta.ssid)));
            memcpy(psk, wc.sta.password,
                   strnlen((const char *)wc.sta.password, sizeof(wc.sta.password)));
        }
    }

    char ssid_hex[2 * 32 + 1], psk_hex[2 * 64 + 1];
    proto_hex_encode(ssid, strlen(ssid), ssid_hex, sizeof(ssid_hex));
    proto_hex_encode(psk, strlen(psk), psk_hex, sizeof(psk_hex));
    conn_ssid_sanitized(s_conn_ssid, sizeof(s_conn_ssid), ssid);
    s_conn_ip[0] = '\0';
    s_got_ip = false;
    s_connecting = true;
    proto_sendf("WPS,OK,%s,%s", ssid_hex, psk_hex);
    esp_wifi_connect(); /* CONN,ASSOC / CONN,GOTIP follow */
}

static void conn_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        s_connecting = false;
        s_got_ip = true;
        char ip[16], gw[16], dns[16] = "0.0.0.0";
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&e->ip_info.ip));
        snprintf(gw, sizeof(gw), IPSTR, IP2STR(&e->ip_info.gw));
        uint32_t dns_net = 0;
        esp_netif_dns_info_t di;
        if (s_sta_netif && esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &di) == ESP_OK &&
            di.ip.type == ESP_IPADDR_TYPE_V4) {
            dns_net = di.ip.u_addr.ip4.addr;
            snprintf(dns, sizeof(dns), IPSTR, IP2STR(&di.ip.u_addr.ip4));
        }
        snprintf(s_conn_ip, sizeof(s_conn_ip), "%s", ip);
        proto_sendf("CONN,GOTIP,%s,%s,%s", ip, gw, dns);
        /* Learn which hosts (and their names) live on this subnet. */
        net_discover_on_gotip(s_sta_netif, e->ip_info.ip.addr, e->ip_info.netmask.addr, dns_net);
        return;
    }
    if (base != WIFI_EVENT) {
        return;
    }

    switch (id) {
    case WIFI_EVENT_STA_CONNECTED: {
        if (s_wps_active) {
            break; /* WPS runs its own associate/deauth handshake */
        }
        const wifi_event_sta_connected_t *e = data;
        s_conn_channel = e->channel;
        s_associated = true;
        proto_sendf("CONN,ASSOC,%02x:%02x:%02x:%02x:%02x:%02x,%u", e->bssid[0], e->bssid[1],
                    e->bssid[2], e->bssid[3], e->bssid[4], e->bssid[5], e->channel);
        break;
    }
    case WIFI_EVENT_STA_DISCONNECTED: {
        if (s_wps_active) {
            break;
        }
        const wifi_event_sta_disconnected_t *e = data;
        s_associated = false;
        s_got_ip = false;
        s_conn_ip[0] = '\0';
        net_discover_on_down();
        if (s_suppress_down) {
            s_suppress_down = false; /* our own teardown for a reconnect/disconnect */
        } else if (s_connecting) {
            s_connecting = false;
            proto_sendf("CONN,FAIL,%d", e->reason); /* join failed before a lease */
        } else {
            proto_sendf("CONN,DOWN"); /* an established link dropped unexpectedly */
        }
        break;
    }
    case WIFI_EVENT_STA_WPS_ER_SUCCESS:
        wps_success(data);
        break;
    case WIFI_EVENT_STA_WPS_ER_FAILED: {
        esp_wifi_wps_disable();
        s_wps_active = false;
        const wifi_event_sta_wps_fail_reason_t *r = data;
        proto_sendf("WPS,FAIL,%d", r ? (int)*r : -1);
        break;
    }
    case WIFI_EVENT_STA_WPS_ER_TIMEOUT:
        esp_wifi_wps_disable();
        s_wps_active = false;
        proto_sendf("WPS,FAIL,timeout");
        break;
    default:
        break;
    }
}

static void set_channel(uint8_t ch)
{
    if (s_associated) {
        /* Associated: the connection owns the channel (the AP's). Calling
         * esp_wifi_set_channel() now would fail, so just track the real channel
         * for the H,<ch> reports and stay put -- a connected hunt never hops. */
        s_channel = s_conn_channel;
        return;
    }
    s_channel = ch;
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}

static uint8_t next_channel(uint8_t ch)
{
    return ch >= MAX_CHANNEL ? 1 : ch + 1;
}

static void set_mode(radio_mode_t mode)
{
    if (s_wps_active) {
        /* Promiscuous and WPS can't coexist; a new scan/connect ends WPS. The
         * explicit cancel path (wps_stop) is what reports WPS,FAIL. */
        esp_wifi_wps_disable();
        s_wps_active = false;
    }

    portENTER_CRITICAL(&s_mux);
    s_mode = mode;
    memset(&s_hunt, 0, sizeof(s_hunt));
    memset(&s_spec, 0, sizeof(s_spec));
    memset(s_unique_table, 0, sizeof(s_unique_table));
    s_unique_count = 0;
    portEXIT_CRITICAL(&s_mux);

    esp_wifi_set_promiscuous(mode != MODE_IDLE);
}

static void hunt_start(const radio_cmd_t *cmd)
{
    set_mode(MODE_IDLE);
    memcpy(s_target, cmd->mac, 6);
    s_hunt_locked = cmd->channel != 0;
    s_hunt_pinned = cmd->pinned && cmd->channel != 0;
    s_hunt_quiet_periods = 0;
    s_hunt_hop_periods = 0;
    s_hunt_hop_limit = cmd->hop_periods ? cmd->hop_periods : HUNT_HOP_PERIODS;
    s_hunt_lost_limit = cmd->lost_periods ? cmd->lost_periods : HUNT_LOST_PERIODS;
    set_channel(cmd->channel ? cmd->channel : 1);
    set_mode(MODE_HUNT);
}

static void hunt_tick(void)
{
    portENTER_CRITICAL(&s_mux);
    const uint32_t frames = s_hunt.frames;
    const int32_t sum = s_hunt.rssi_sum;
    const int32_t noise_sum = s_hunt.noise_sum;
    const int last = s_hunt.rssi_last;
    const int max = s_hunt.rssi_max;
    memset(&s_hunt, 0, sizeof(s_hunt));
    portEXIT_CRITICAL(&s_mux);

    const int avg = frames ? (int)(sum / (int32_t)frames) : 0;
    const int nf = frames ? (int)(noise_sum / (int32_t)frames) : 0;
    proto_sendf("H,%u,%lu,%d,%d,%d,%d,%d", s_channel, (unsigned long)frames, frames ? last : 0, avg,
                frames ? max : 0, s_hunt_locked ? 1 : 0, nf);
    led_show_heat(avg);

    if (frames) {
        s_hunt_locked = true;
        s_hunt_quiet_periods = 0;
        s_hunt_hop_periods = 0;
        return;
    }
    if (s_hunt_pinned) {
        return; /* the user chose this channel: stay */
    }

    if (s_hunt_locked) {
        if (++s_hunt_quiet_periods >= s_hunt_lost_limit) {
            /* Target went silent or moved channel: search again. */
            s_hunt_locked = false;
            s_hunt_hop_periods = 0;
        }
    } else if (++s_hunt_hop_periods >= s_hunt_hop_limit) {
        s_hunt_hop_periods = 0;
        set_channel(next_channel(s_channel));
    }
}

static void spectrum_start(void)
{
    set_mode(MODE_IDLE);
    set_channel(1);
    set_mode(MODE_SPECTRUM);
}

static void spectrum_tick(void)
{
    portENTER_CRITICAL(&s_mux);
    const uint32_t frames = s_spec.frames;
    const uint32_t bytes = s_spec.bytes;
    const int max = s_spec.rssi_max;
    memset(&s_spec, 0, sizeof(s_spec));
    portEXIT_CRITICAL(&s_mux);

    proto_sendf("S,%u,%lu,%lu,%d", s_channel, (unsigned long)frames, (unsigned long)bytes,
                frames ? max : 0);

    /* Green flicker proportional to traffic on the channel. */
    const uint32_t level = frames > 63 ? 255 : frames * 4;
    led_set(0, (uint8_t)level, 4);

    const uint8_t next = next_channel(s_channel);
    if (next == 1) {
        portENTER_CRITICAL(&s_mux);
        const uint16_t unique = s_unique_count;
        memset(s_unique_table, 0, sizeof(s_unique_table));
        s_unique_count = 0;
        portEXIT_CRITICAL(&s_mux);
        proto_sendf("U,%u", unique);
    }
    set_channel(next);
}

static bool discover_wanted(const discover_dev_t *d)
{
    switch (s_disc_filter) {
    case DISCOVER_FILTER_AP: return d->flags & DEV_FLAG_AP;
    case DISCOVER_FILTER_STA: return !(d->flags & DEV_FLAG_AP);
    /* CLIENTS already filtered on insert: everything recorded is wanted. */
    default: return true;
    }
}

static void discover_start(radio_discover_filter_t filter, uint8_t channel, const uint8_t *bssid,
                           uint16_t dwell_ms)
{
    set_mode(MODE_IDLE);
    s_dev_count = 0;
    s_disc_dwell_ms = dwell_ms ? dwell_ms : DISCOVER_DWELL_MS;
    s_disc_filter = filter;
    s_disc_channel = channel;
    memcpy(s_disc_bssid, bssid ? bssid : (const uint8_t[6]){0}, 6);
    s_disc_cursor = 0;
    s_disc_streamed = 0;
    s_disc_prev_count = 0;
    static const char *const names[] = {"ALL", "AP", "STA", "CLIENTS"};
    proto_sendf("DISCOVER_BEGIN,%s,%u", names[filter], channel);
    led_set(40, 0, 60);
    set_channel(channel ? channel : 1);
    set_mode(MODE_DISCOVER);
    if (s_associated) {
        net_discover_rescan(); /* refresh host IPs for the list being opened */
    }
}

static void discover_emit_one(const discover_dev_t *d)
{
    const uint8_t *m = d->mac;
    /* A/S, optional R, then (APs with a parsed beacon) one security and one PHY
     * code. Older firmware omits the trailing codes; readers must not assume
     * fixed positions beyond A/S/R. */
    char flags[6];
    int f = 0;
    flags[f++] = (d->flags & DEV_FLAG_AP) ? 'A' : 'S';
    if (m[0] & 0x02) {
        flags[f++] = 'R'; /* locally administered = randomized MAC */
    }
    switch (d->security) {
    case SEC_OPEN: flags[f++] = 'o'; break;
    case SEC_WEP:  flags[f++] = 'w'; break;
    case SEC_WPA:  flags[f++] = '1'; break;
    case SEC_WPA2: flags[f++] = '2'; break;
    case SEC_WPA3: flags[f++] = '3'; break;
    default: break; /* SEC_UNKNOWN: no code */
    }
    switch (d->phy) {
    case PHY_LEGACY: flags[f++] = 'g'; break;
    case PHY_HT:     flags[f++] = 'n'; break;
    case PHY_VHT:    flags[f++] = 'c'; break;
    case PHY_HE:     flags[f++] = 'x'; break;
    default: break; /* PHY_UNKNOWN: no code */
    }
    flags[f] = '\0';
    proto_sendf("DEV,%02x:%02x:%02x:%02x:%02x:%02x,%u,%d,%u,%s,%s", m[0], m[1], m[2], m[3], m[4],
                m[5], d->channel, d->rssi_max, d->frames, flags, d->ssid);
}

/* Emit DEV lines for wanted devices first seen a tick ago, so the promiscuous
 * callback (which writes the table lock-free) has had time to fill their
 * flags/frames/ssid. Streams in discovery order and never reorders, so the
 * Flipper's live list can append without reshuffling. */
static void discover_stream(void)
{
    const uint16_t ready = s_disc_prev_count;
    for (; s_disc_cursor < ready && s_disc_streamed < DISCOVER_REPORT_MAX; s_disc_cursor++) {
        const discover_dev_t *d = &s_devs[s_disc_cursor];
        if (discover_wanted(d)) {
            discover_emit_one(d);
            s_disc_streamed++;
        }
    }
    s_disc_prev_count = s_dev_count;
}

static void discover_tick(void)
{
    discover_stream();
    if (s_disc_channel == 0) {
        set_channel(next_channel(s_channel));
    }
}

static void connect_start(const radio_cmd_t *cmd)
{
    /* Associating and channel-controlled sniffing don't mix, and the app always
     * connects from idle; drop any running scan first. */
    set_mode(MODE_IDLE);
    if (s_associated) {
        /* Reconnecting from an active link: the disconnect it takes is ours, so
         * suppress the STA_DISCONNECTED it raises (guaranteed while associated). */
        s_suppress_down = true;
        esp_wifi_disconnect();
        s_associated = false;
    }
    conn_ssid_sanitized(s_conn_ssid, sizeof(s_conn_ssid), cmd->ssid);
    s_conn_ip[0] = '\0';
    s_got_ip = false;
    s_connecting = true;

    wifi_config_t wc = {0};
    /* wc is zeroed; copy the bytes (a 32-byte SSID fills the field with no NUL,
     * which is valid 802.11). */
    memcpy(wc.sta.ssid, cmd->ssid, strnlen(cmd->ssid, sizeof(wc.sta.ssid)));
    memcpy(wc.sta.password, cmd->psk, strnlen(cmd->psk, sizeof(wc.sta.password)));
    if (cmd->has_bssid) {
        memcpy(wc.sta.bssid, cmd->mac, 6);
        wc.sta.bssid_set = true;
    }
    if (cmd->channel) {
        wc.sta.channel = cmd->channel;
    }
    /* Accept any authmode including open: the default threshold would otherwise
     * refuse an open AP. */
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err == ESP_OK) {
        err = esp_wifi_connect();
    }
    if (err != ESP_OK) {
        s_connecting = false;
        proto_sendf("CONN,FAIL,%d", err);
    }
}

static void do_disconnect(void)
{
    /* Report exactly one CONN,DOWN here and swallow the forced event (only when
     * we were associated, so a disconnect event is guaranteed and the flag can't
     * leak to a later real drop). */
    const bool had_link = s_associated;
    s_connecting = false;
    s_associated = false;
    s_got_ip = false;
    s_conn_ip[0] = '\0';
    net_discover_on_down();
    if (had_link) {
        s_suppress_down = true;
        esp_wifi_disconnect();
    }
    proto_sendf("CONN,DOWN");
}

static void wps_start(void)
{
    /* WPS refuses to start with sniffer mode on, and we connect from idle. */
    set_mode(MODE_IDLE);
    if (s_associated) {
        s_suppress_down = true;
        esp_wifi_disconnect();
        s_associated = false;
    }
    s_connecting = false;

    esp_wps_config_t cfg = WPS_CONFIG_INIT_DEFAULT(WPS_TYPE_PBC);
    esp_err_t err = esp_wifi_wps_enable(&cfg);
    if (err == ESP_OK) {
        err = esp_wifi_wps_start(); /* uses the default ~120 s enrolment window */
    }
    if (err != ESP_OK) {
        esp_wifi_wps_disable();
        s_wps_active = false;
        proto_sendf("WPS,FAIL,%d", err);
        return;
    }
    s_wps_active = true;
    proto_sendf("WPS,ACTIVE"); /* now press WPS on the router */
}

static void wps_stop(void)
{
    if (s_wps_active) {
        esp_wifi_wps_disable();
        s_wps_active = false;
        proto_sendf("WPS,FAIL,cancelled");
    }
}

static void radio_task(void *arg)
{
    (void)arg;
    radio_cmd_t cmd;

    for (;;) {
        TickType_t wait = portMAX_DELAY;
        if (s_mode == MODE_HUNT) {
            wait = pdMS_TO_TICKS(HUNT_PERIOD_MS);
        } else if (s_mode == MODE_SPECTRUM) {
            wait = pdMS_TO_TICKS(SPECTRUM_DWELL_MS);
        } else if (s_mode == MODE_DISCOVER) {
            wait = pdMS_TO_TICKS(s_disc_dwell_ms);
        }

        if (xQueueReceive(s_cmd_queue, &cmd, wait) == pdTRUE) {
            switch (cmd.type) {
            case CMD_STOP:
                set_mode(MODE_IDLE);
                net_discover_elicit_stop(); /* no hunt to feed anymore */
                led_set(0, 0, 8);
                break;
            case CMD_SLEEP:
                set_mode(MODE_IDLE);
                do_disconnect(); /* drop the link on app exit (also stops elicitation) */
                led_set(0, 0, 0);
                break;
            case CMD_HUNT:
                hunt_start(&cmd);
                break;
            case CMD_SPECTRUM:
                spectrum_start();
                break;
            case CMD_DISCOVER:
                discover_start(cmd.filter, cmd.channel, cmd.mac, cmd.dwell_ms);
                break;
            case CMD_CONNECT:
                connect_start(&cmd);
                break;
            case CMD_DISCONNECT:
                do_disconnect();
                break;
            case CMD_WPS_START:
                wps_start();
                break;
            case CMD_WPS_STOP:
                wps_stop();
                break;
            }
            continue;
        }

        if (s_mode == MODE_HUNT) {
            hunt_tick();
        } else if (s_mode == MODE_SPECTRUM) {
            spectrum_tick();
        } else if (s_mode == MODE_DISCOVER) {
            discover_tick();
        }
    }
}

static void post(const radio_cmd_t *cmd)
{
    /* Commands replace each other; drop stale ones if the Flipper spams us. */
    if (xQueueSend(s_cmd_queue, cmd, 0) != pdTRUE) {
        xQueueReset(s_cmd_queue);
        xQueueSend(s_cmd_queue, cmd, 0);
    }
}

void radio_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &conn_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &conn_event_handler, NULL, NULL));

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    const wifi_country_t country = {
        .cc = "01",
        .schan = 1,
        .nchan = MAX_CHANNEL,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
    };
    ESP_ERROR_CHECK(esp_wifi_set_country(&country));
    ESP_ERROR_CHECK(esp_wifi_start());

    const wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA,
    };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(promisc_cb));

    net_discover_init();

    s_cmd_queue = xQueueCreate(4, sizeof(radio_cmd_t));
    xTaskCreate(radio_task, "radio", 4096, NULL, 5, NULL);
}

void radio_hunt(const uint8_t mac[6], uint8_t channel, bool pinned, uint16_t hop_periods,
                uint16_t lost_periods)
{
    radio_cmd_t cmd = {.type = CMD_HUNT,
                       .channel = channel,
                       .pinned = pinned,
                       .hop_periods = hop_periods,
                       .lost_periods = lost_periods};
    memcpy(cmd.mac, mac, 6);
    post(&cmd);
}

void radio_spectrum(void)
{
    const radio_cmd_t cmd = {.type = CMD_SPECTRUM};
    post(&cmd);
}

void radio_discover(radio_discover_filter_t filter, uint8_t channel, const uint8_t bssid[6],
                    uint16_t dwell_ms)
{
    radio_cmd_t cmd = {
        .type = CMD_DISCOVER, .filter = filter, .channel = channel, .dwell_ms = dwell_ms};
    if (bssid) {
        memcpy(cmd.mac, bssid, 6); /* reused by discover_start as the CLIENTS target */
    }
    post(&cmd);
}

void radio_connect(const char *ssid, const char *psk, const uint8_t bssid[6], uint8_t channel)
{
    radio_cmd_t cmd = {.type = CMD_CONNECT, .channel = channel, .has_bssid = bssid != NULL};
    snprintf(cmd.ssid, sizeof(cmd.ssid), "%s", ssid ? ssid : "");
    snprintf(cmd.psk, sizeof(cmd.psk), "%s", psk ? psk : "");
    if (bssid) {
        memcpy(cmd.mac, bssid, 6);
    }
    post(&cmd);
}

void radio_disconnect(void)
{
    const radio_cmd_t cmd = {.type = CMD_DISCONNECT};
    post(&cmd);
}

void radio_wps_start(void)
{
    const radio_cmd_t cmd = {.type = CMD_WPS_START};
    post(&cmd);
}

void radio_wps_stop(void)
{
    const radio_cmd_t cmd = {.type = CMD_WPS_STOP};
    post(&cmd);
}

bool radio_elicit_start(const uint8_t mac[6], uint16_t interval_ms)
{
    if (!s_got_ip) {
        proto_sendf("ERR,ELICIT needs a connection");
        return false;
    }
    net_discover_elicit_start(mac, interval_ms);
    return true;
}

void radio_elicit_stop(void)
{
    net_discover_elicit_stop();
}

void radio_conn_summary(char *ssid, size_t ssid_sz, char *ip, size_t ip_sz)
{
    if (ssid && ssid_sz) {
        snprintf(ssid, ssid_sz, "%s", (s_associated && s_conn_ssid[0]) ? s_conn_ssid : "-");
    }
    if (ip && ip_sz) {
        snprintf(ip, ip_sz, "%s", (s_got_ip && s_conn_ip[0]) ? s_conn_ip : "-");
    }
}

void radio_stop(void)
{
    const radio_cmd_t cmd = {.type = CMD_STOP};
    post(&cmd);
}

void radio_sleep(void)
{
    const radio_cmd_t cmd = {.type = CMD_SLEEP};
    post(&cmd);
}
