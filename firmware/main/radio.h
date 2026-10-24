#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    DISCOVER_FILTER_ALL,
    DISCOVER_FILTER_AP,      /* access points only */
    DISCOVER_FILTER_STA,     /* everything that is not an access point */
    DISCOVER_FILTER_CLIENTS, /* only stations associated with a given BSSID */
} radio_discover_filter_t;

/** Bring up WiFi in station mode (initially not associated) and start the radio
 * task. radio_connect() may later associate with an AP without stopping the
 * promiscuous RX the hunt relies on. */
void radio_init(void);

/** Hunt timing is counted in hunt sample periods (100 ms). */
#define RADIO_HUNT_PERIOD_MS 100

/**
 * Track frames sent by `mac`. channel 0 = hop until found.
 * `pinned` keeps the radio on `channel` even when the target goes quiet.
 * `hop_periods` is the dwell per channel while searching, `lost_periods` the
 * silence before a locked hunt searches again; 0 picks the default for either.
 */
void radio_hunt(const uint8_t mac[6], uint8_t channel, bool pinned, uint16_t hop_periods,
                uint16_t lost_periods);

/** Sweep all channels and stream per-channel activity. */
void radio_spectrum(void);

/**
 * Listen continuously (hopping all channels, or only on `channel` if non-zero),
 * streaming each transmitter matching `filter` as it is first heard, until
 * radio_stop(). For DISCOVER_FILTER_CLIENTS, `bssid` is the access point whose
 * associated stations to report; it is ignored (and may be NULL) for every
 * other filter. `dwell_ms` is how long each tick (channel hop, when hopping)
 * lasts; 0 picks the default.
 */
void radio_discover(radio_discover_filter_t filter, uint8_t channel, const uint8_t bssid[6],
                    uint16_t dwell_ms);

/**
 * Associate with an access point in station mode, keeping promiscuous RX alive
 * so a hunt keeps sampling on the AP's channel. `psk` may be empty for an open
 * network; `bssid` (or NULL) and `channel` (or 0) speed up and disambiguate the
 * join. Returns immediately; progress arrives asynchronously as CONN,... lines.
 */
void radio_connect(const char *ssid, const char *psk, const uint8_t bssid[6], uint8_t channel);

/** Drop the association and return to pure receive-only sniffing. */
void radio_disconnect(void);

/**
 * Start WPS push-button (PBC) enrolment. Reports `WPS,ACTIVE` when armed (press
 * WPS on the router), then `WPS,OK,<ssid_hex>,<psk_hex>` with the negotiated
 * credentials and auto-associates (CONN,... follows), or `WPS,FAIL,<reason>`.
 */
void radio_wps_start(void);

/** Cancel an in-progress WPS enrolment. */
void radio_wps_stop(void);

/**
 * Start eliciting reply frames from `mac` (a client of the associated AP) at
 * `interval_ms`, so a silent target can be hunted. Requires a DHCP lease;
 * returns false (and reports `ERR`) if not connected. Emits `ELICIT,ON`.
 */
bool radio_elicit_start(const uint8_t mac[6], uint16_t interval_ms);

/** Stop elicitation (emits `ELICIT,OFF` if it was running). */
void radio_elicit_stop(void);

/** Re-run host discovery now (ARP sweep + re-announce) if associated. Used to
 * resolve a hunt target's IP/host that wasn't known yet. Returns false with no
 * lease. */
bool radio_resolve(void);

/**
 * Fingerprint the host behind `mac` (a client of the associated AP): a short
 * active port-scan + HTTP banner grab that streams `FP,...` lines. Requires a
 * DHCP lease; returns false (and reports `ERR`) if not connected.
 */
bool radio_fingerprint_start(const uint8_t mac[6]);

/** Cancel an in-flight / pending fingerprint. */
void radio_fingerprint_stop(void);

/**
 * Fill short association / IPv4 strings for the INFO block. Each is set to "-"
 * when not associated or without a lease yet. `ssid` should hold >= 33 bytes and
 * `ip` >= 16; either pointer may be NULL to skip it.
 */
void radio_conn_summary(char *ssid, size_t ssid_sz, char *ip, size_t ip_sz);

/** Go idle. */
void radio_stop(void);

/** Go idle and turn the LED off (dark idle; the app sends this on exit). */
void radio_sleep(void);
