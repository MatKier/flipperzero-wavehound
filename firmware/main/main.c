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

static void handle_hunt(char *args)
{
    /* args: "<mac>,<ch>[,PIN]" */
    char *f[3] = {0};
    int n = split_args(args, f, 3);
    uint8_t mac[6];
    int ch;
    if (n < 2) {
        proto_sendf("ERR,usage HUNT,<mac>,<ch>[,PIN]");
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
    const bool pinned = n == 3 && strcasecmp(f[2], "PIN") == 0;
    if (pinned && ch == 0) {
        proto_sendf("ERR,PIN needs a channel");
        return;
    }
    radio_hunt(mac, (uint8_t)ch, pinned);
    proto_sendf("OK,HUNT");
}

static void handle_discover(char *args)
{
    /* args: "[ALL|AP|STA|CLIENTS[,<ch>[,<bssid>]]]" */
    char *f[3] = {0};
    int n = split_args(args, f, 3);
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
    proto_sendf("OK,DISCOVER");
    radio_discover(filter, (uint8_t)ch, filter == DISCOVER_FILTER_CLIENTS ? bssid : NULL);
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
