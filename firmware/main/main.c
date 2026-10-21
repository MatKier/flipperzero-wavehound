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

#define LINE_MAX 96

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

static void handle_discover(char *args)
{
    /* args: "[ALL|AP|STA|CLIENTS[,<ch>[,<bssid>]]][,DWELL=<ms>]" */
    char *f[4] = {0};
    int n = split_args(args, f, 4);
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
    uint8_t bssid[6];
    if (filter == DISCOVER_FILTER_CLIENTS && (n < 3 || !parse_mac(f[2], bssid))) {
        proto_sendf("ERR,CLIENTS needs a BSSID: DISCOVER,CLIENTS,<ch>,<bssid>");
        return;
    }
    /* Tokens follow the positional fields: after <bssid> for CLIENTS, else <ch>. */
    tokens_t t;
    parse_tokens(f, filter == DISCOVER_FILTER_CLIENTS ? 3 : 2, n, &t);
    proto_sendf("OK,DISCOVER");
    radio_discover(filter, (uint8_t)ch, filter == DISCOVER_FILTER_CLIENTS ? bssid : NULL,
                   (uint16_t)t.dwell_ms);
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
