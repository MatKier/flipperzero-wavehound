#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    DISCOVER_FILTER_ALL,
    DISCOVER_FILTER_AP,      /* access points only */
    DISCOVER_FILTER_STA,     /* everything that is not an access point */
    DISCOVER_FILTER_CLIENTS, /* only stations associated with a given BSSID */
} radio_discover_filter_t;

/** Bring up WiFi (station mode, not connected) and start the radio task. */
void radio_init(void);

/**
 * Track frames sent by `mac`. channel 0 = hop until found.
 * `pinned` keeps the radio on `channel` even when the target goes quiet.
 */
void radio_hunt(const uint8_t mac[6], uint8_t channel, bool pinned);

/** Sweep all channels and stream per-channel activity. */
void radio_spectrum(void);

/**
 * Listen for `seconds` (hopping all channels, or only on `channel` if non-zero),
 * then report the transmitters matching `filter`. For DISCOVER_FILTER_CLIENTS,
 * `bssid` is the access point whose associated stations to report; it is ignored
 * (and may be NULL) for every other filter.
 */
void radio_discover(uint16_t seconds, radio_discover_filter_t filter, uint8_t channel,
                    const uint8_t bssid[6]);

/** End a running listen now and report what was heard so far. */
void radio_finish(void);

/** Go idle. */
void radio_stop(void);
