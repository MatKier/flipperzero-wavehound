#include "radio.h"

#include <stdbool.h>
#include <string.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "led.h"
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
    CMD_SLEEP,
} radio_cmd_type_t;

typedef struct {
    radio_cmd_type_t type;
    uint8_t mac[6];
    uint8_t channel;
    radio_discover_filter_t filter;
    bool pinned;
    uint16_t hop_periods;  /* hunt: 0 = HUNT_HOP_PERIODS */
    uint16_t lost_periods; /* hunt: 0 = HUNT_LOST_PERIODS */
    uint16_t dwell_ms;     /* discover: 0 = DISCOVER_DWELL_MS */
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

static void set_channel(uint8_t ch)
{
    s_channel = ch;
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}

static uint8_t next_channel(uint8_t ch)
{
    return ch >= MAX_CHANNEL ? 1 : ch + 1;
}

static void set_mode(radio_mode_t mode)
{
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
                led_set(0, 0, 8);
                break;
            case CMD_SLEEP:
                set_mode(MODE_IDLE);
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
    esp_netif_create_default_wifi_sta();

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
