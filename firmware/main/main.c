/*
 * Wavehound - WiFi fox-hunting firmware for the Flipper Zero WiFi dev board (ESP32-S2).
 *
 * Passive receive-only: the board scans, sniffs frame headers for RSSI, and
 * reports to the Flipper over UART. See docs/PROTOCOL.md.
 */
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_err.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "info.h"
#include "led.h"
#include "proto.h"
#include "radio.h"

/* Long enough for the biggest command: CONNECT carries a hex-encoded SSID and
 * PSK (up to 32 + 63 bytes -> ~220 chars). See docs/PROTOCOL.md. */
#define LINE_MAX 256

/* Bounds for the optional timing tokens (DWELL=<ms>, LOST=<s>). */
#define DWELL_MIN_MS 100
#define DWELL_MAX_MS 2000
#define LOST_MIN_S 2
#define LOST_MAX_S 120

static bool parse_mac(const char *s, uint8_t mac[6])
{
    unsigned int b[6];
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t)b[i];
    }
    return true;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode an even-length hex string into out (NUL-terminated, at most out_sz-1
 * bytes). Returns the byte count, or -1 on non-hex, odd-length or oversized
 * input. An empty string decodes to "" (0 bytes). Free-text fields (SSID, PSK)
 * travel hex-encoded so a comma or space in them can't break the line protocol. */
static int hex_decode(const char *s, char *out, size_t out_sz)
{
    size_t n = 0;
    while (s[0]) {
        const int hi = hex_nibble(s[0]);
        const int lo = hex_nibble(s[1]); /* s[1] may be the NUL terminator */
        if (hi < 0 || lo < 0) {
            return -1;
        }
        if (n + 1 >= out_sz) {
            return -1;
        }
        out[n++] = (char)((hi << 4) | lo);
        s += 2;
    }
    out[n] = '\0';
    return (int)n;
}

/* Split comma-separated args in place. Returns the number of fields found. */
static int split_args(char *args, char **fields, int max)
{
    int n = 0;
    while (args && n < max) {
        fields[n++] = args;
        args = strchr(args, ',');
        if (args) {
            *args++ = '\0';
        }
    }
    return n;
}

static bool parse_channel(const char *s, int *ch)
{
    *ch = s ? atoi(s) : 0;
    return *ch >= 0 && *ch <= CONFIG_WH_MAX_CHANNEL;
}

/* Optional trailing tokens shared by HUNT and DISCOVER. Unknown tokens are
 * ignored, so newer apps can add more without breaking this firmware. */
typedef struct {
    bool pin;
    unsigned dwell_ms; /* 0 = not given */
    unsigned lost_s;   /* 0 = not given */
} tokens_t;

static unsigned clamp_u(int v, unsigned lo, unsigned hi)
{
    return v < (int)lo ? lo : v > (int)hi ? hi : (unsigned)v;
}

static void parse_tokens(char **f, int from, int n, tokens_t *t)
{
    memset(t, 0, sizeof(*t));
    for (int i = from; i < n; i++) {
        if (strcasecmp(f[i], "PIN") == 0) {
            t->pin = true;
        } else if (strncasecmp(f[i], "DWELL=", 6) == 0) {
            t->dwell_ms = clamp_u(atoi(f[i] + 6), DWELL_MIN_MS, DWELL_MAX_MS);
        } else if (strncasecmp(f[i], "LOST=", 5) == 0) {
            t->lost_s = clamp_u(atoi(f[i] + 5), LOST_MIN_S, LOST_MAX_S);
        }
    }
}

static void handle_hunt(char *args)
{
    /* args: "<mac>,<ch>[,PIN][,DWELL=<ms>][,LOST=<s>]" */
    char *f[5] = {0};
    int n = split_args(args, f, 5);
    uint8_t mac[6];
    int ch;
    if (n < 2) {
        proto_sendf("ERR,usage HUNT,<mac>,<ch>[,PIN][,DWELL=<ms>][,LOST=<s>]");
        return;
    }
    if (!parse_mac(f[0], mac)) {
        proto_sendf("ERR,bad mac");
        return;
    }
    if (!parse_channel(f[1], &ch)) {
        proto_sendf("ERR,bad channel");
        return;
    }
    tokens_t t;
    parse_tokens(f, 2, n, &t);
    if (t.pin && ch == 0) {
        proto_sendf("ERR,PIN needs a channel");
        return;
    }
    /* Round the dwell to whole hunt periods. */
    const uint16_t hop = (t.dwell_ms + RADIO_HUNT_PERIOD_MS / 2) / RADIO_HUNT_PERIOD_MS;
    const uint16_t lost = t.lost_s * 1000 / RADIO_HUNT_PERIOD_MS;
    radio_hunt(mac, (uint8_t)ch, t.pin, hop, lost);
    proto_sendf("OK,HUNT");
}

/* ALSO=<bssid>/<bssid>/...: more access points for a CLIENTS listen (the other
 * nodes of a mesh), appended to bssids[] after the positional one. */
static void parse_also(const char *s, uint8_t (*bssids)[6], uint8_t *count)
{
    while (s && *s && *count < RADIO_CLIENTS_BSSID_MAX) {
        if (parse_mac(s, bssids[*count])) {
            (*count)++;
        }
        s = strchr(s, '/');
        if (s) {
            s++;
        }
    }
}

/* HOP=<ch>/<ch>/...: hop only these channels. Returns a mask, bit n = channel n. */
static uint16_t parse_hop(const char *s)
{
    uint16_t mask = 0;
    while (s && *s) {
        const int ch = atoi(s);
        if (ch >= 1 && ch <= CONFIG_WH_MAX_CHANNEL) {
            mask |= (uint16_t)(1u << ch);
        }
        s = strchr(s, '/');
        if (s) {
            s++;
        }
    }
    return mask;
}

static void handle_discover(char *args)
{
    /* args: "[ALL|AP|STA|CLIENTS[,<ch>[,<bssid>]]][,DWELL=<ms>][,ALSO=<bssid>/...][,HOP=<ch>/...]" */
    char *f[8] = {0};
    int n = split_args(args, f, 8);
    radio_discover_filter_t filter = DISCOVER_FILTER_ALL;
    int ch = 0;

    if (n >= 1) {
        if (strcasecmp(f[0], "AP") == 0) {
            filter = DISCOVER_FILTER_AP;
        } else if (strcasecmp(f[0], "STA") == 0) {
            filter = DISCOVER_FILTER_STA;
        } else if (strcasecmp(f[0], "CLIENTS") == 0) {
            filter = DISCOVER_FILTER_CLIENTS;
        } else if (strcasecmp(f[0], "ALL") != 0) {
            proto_sendf("ERR,filter must be ALL, AP, STA or CLIENTS");
            return;
        }
    }
    if (n >= 2 && !parse_channel(f[1], &ch)) {
        proto_sendf("ERR,bad channel");
        return;
    }
    const bool clients = filter == DISCOVER_FILTER_CLIENTS;
    uint8_t bssids[RADIO_CLIENTS_BSSID_MAX][6];
    uint8_t bssid_count = 0;
    if (clients && (n < 3 || !parse_mac(f[2], bssids[0]))) {
        proto_sendf("ERR,CLIENTS needs a BSSID: DISCOVER,CLIENTS,<ch>,<bssid>");
        return;
    }
    if (clients) {
        bssid_count = 1;
    }
    /* Tokens follow the positional fields: after <bssid> for CLIENTS, else <ch>. */
    const int from = clients ? 3 : 2;
    tokens_t t;
    parse_tokens(f, from, n, &t);
    uint16_t hop_mask = 0;
    for (int i = from; i < n; i++) {
        if (clients && strncasecmp(f[i], "ALSO=", 5) == 0) {
            parse_also(f[i] + 5, bssids, &bssid_count);
        } else if (strncasecmp(f[i], "HOP=", 4) == 0) {
            hop_mask = parse_hop(f[i] + 4);
        }
    }
    proto_sendf("OK,DISCOVER");
    radio_discover(filter, (uint8_t)ch, clients ? bssids : NULL, bssid_count, hop_mask,
                   (uint16_t)t.dwell_ms);
}

static void handle_connect(char *args)
{
    /* args: "<ssid_hex>,<psk_hex>[,<bssid>][,<ch>]". The two optional fields are
     * told apart by the ':' in a BSSID, so their order does not matter. */
    char *f[4] = {0};
    int n = split_args(args, f, 4);
    if (n < 2) {
        proto_sendf("ERR,usage CONNECT,<ssid_hex>,<psk_hex>[,<bssid>][,<ch>]");
        return;
    }
    char ssid[33];
    char psk[65]; /* 64-char PSK + NUL */
    if (hex_decode(f[0], ssid, sizeof(ssid)) <= 0) {
        proto_sendf("ERR,bad ssid");
        return;
    }
    if (hex_decode(f[1], psk, sizeof(psk)) < 0) {
        proto_sendf("ERR,bad psk");
        return;
    }
    uint8_t bssid[6];
    bool has_bssid = false;
    int ch = 0;
    for (int i = 2; i < n; i++) {
        if (strchr(f[i], ':')) {
            if (!parse_mac(f[i], bssid)) {
                proto_sendf("ERR,bad bssid");
                return;
            }
            has_bssid = true;
        } else if (!parse_channel(f[i], &ch) || ch == 0) {
            proto_sendf("ERR,bad channel");
            return;
        }
    }
    proto_sendf("OK,CONNECT");
    radio_connect(ssid, psk, has_bssid ? bssid : NULL, (uint8_t)ch);
}

static void handle_wps(char *args)
{
    /* "WPS" starts PBC enrolment; "WPS,STOP" cancels it. */
    if (args && strcasecmp(args, "STOP") == 0) {
        radio_wps_stop();
    } else {
        radio_wps_start();
    }
    proto_sendf("OK,WPS");
}

static void handle_elicit(char *args)
{
    /* "ELICIT,<mac>,<ms>" starts; "ELICIT,STOP" stops. */
    char *f[2] = {0};
    int n = split_args(args, f, 2);
    if (n >= 1 && strcasecmp(f[0], "STOP") == 0) {
        radio_elicit_stop();
        proto_sendf("OK,ELICIT");
        return;
    }
    uint8_t mac[6];
    if (n < 2 || !parse_mac(f[0], mac)) {
        proto_sendf("ERR,usage ELICIT,<mac>,<ms> | ELICIT,STOP");
        return;
    }
    if (radio_elicit_start(mac, (uint16_t)atoi(f[1]))) {
        proto_sendf("OK,ELICIT");
    }
}

static void handle_fp(char *args)
{
    /* "FP,<mac>" starts a fingerprint; "FP,STOP" cancels it. */
    char *f[1] = {0};
    int n = split_args(args, f, 1);
    if (n >= 1 && strcasecmp(f[0], "STOP") == 0) {
        radio_fingerprint_stop();
        proto_sendf("OK,FP");
        return;
    }
    uint8_t mac[6];
    if (n < 1 || !parse_mac(f[0], mac)) {
        proto_sendf("ERR,usage FP,<mac> | FP,STOP");
        return;
    }
    if (radio_fingerprint_start(mac)) {
        proto_sendf("OK,FP");
    }
}

static void handle_line(char *line)
{
    char *args = strchr(line, ',');
    if (args) {
        *args++ = '\0';
    }
    for (char *p = line; *p; p++) {
        *p = (char)toupper((unsigned char)*p);
    }

    if (strcmp(line, "PING") == 0) {
        proto_sendf("PONG,%s", WH_FW_VERSION);
    } else if (strcmp(line, "HUNT") == 0) {
        handle_hunt(args);
    } else if (strcmp(line, "SPECTRUM") == 0) {
        proto_sendf("OK,SPECTRUM");
        radio_spectrum();
    } else if (strcmp(line, "DISCOVER") == 0) {
        handle_discover(args);
    } else if (strcmp(line, "CONNECT") == 0) {
        handle_connect(args);
    } else if (strcmp(line, "DISCONNECT") == 0) {
        radio_disconnect();
        proto_sendf("OK,DISCONNECT");
    } else if (strcmp(line, "WPS") == 0) {
        handle_wps(args);
    } else if (strcmp(line, "ELICIT") == 0) {
        handle_elicit(args);
    } else if (strcmp(line, "FP") == 0) {
        handle_fp(args);
    } else if (strcmp(line, "RESOLVE") == 0) {
        /* Re-run host discovery; any resolved IP/host streams back as IP lines.
         * An optional MAC arg (RESOLVE,<mac>) is ignored -- the sweep is
         * subnet-wide -- but accepted for forward compatibility. */
        radio_resolve();
    } else if (strcmp(line, "INFO") == 0) {
        info_report();
    } else if (strcmp(line, "STOP") == 0) {
        radio_stop();
        proto_sendf("OK,STOP");
    } else if (strcmp(line, "SLEEP") == 0) {
        radio_sleep();
        proto_sendf("OK,SLEEP");
    } else {
        proto_sendf("ERR,unknown %s", line);
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    led_init();
    led_set(0, 0, 8);
    proto_init();
    radio_init();

    proto_sendf("READY,%s", WH_FW_VERSION);

    char line[LINE_MAX];
    for (;;) {
        proto_read_line(line, sizeof(line));
        handle_line(line);
    }
}
