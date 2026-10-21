#pragma once

#include <stdbool.h>
#include <stdint.h>

/** Which channel option the picker pre-selects when hunting a saved MAC. */
typedef enum {
    WhSavedHuntSavedChannel, /* the channel stored with the entry */
    WhSavedHuntAutoHop,      /* auto / hop until heard */
    WhSavedHuntNearbyAp,     /* channel of a nearby access point */
    WhSavedHuntModeCount,
} WhSavedHuntMode;

/** Meter/click range preset: shifts the dBm window the hunt meter spans, so the
 * readout and click/haptic rate stay useful close up or at a distance. */
typedef enum {
    WhRangeNear,   /* close up: spread the strong end so the meter isn't pinned */
    WhRangeNormal, /* default window */
    WhRangeFar,    /* long range: let weak signals still move the meter */
    WhRangeModeCount,
} WhRangeMode;

/** How quickly the hunt's RSSI readout and meter follow new samples. */
typedef enum {
    WhResponseFast,   /* jumpy, reacts within a sample or two */
    WhResponseNormal, /* default */
    WhResponseSmooth, /* steadier, slower to settle */
    WhResponseModeCount,
} WhMeterResponse;

/* Preset counts for the board timing settings (see the wh_settings_*_ms/_s maps). */
#define WH_HOP_DWELL_COUNT 6
#define WH_RESEARCH_COUNT 5
#define WH_SCAN_DWELL_COUNT 5

/** User preferences, persisted to the SD card. */
typedef struct {
    float click_volume;      /* geiger click loudness, 0.1..1.0 */
    bool mute_default;       /* hunt screen starts muted */
    uint8_t saved_hunt_mode; /* WhSavedHuntMode */
    bool haptic;             /* buzz the motor as a proximity cue while hunting */
    bool keep_backlight;     /* hold the backlight on during a hunt */
    uint8_t range_mode;      /* WhRangeMode */
    uint8_t meter_response;  /* WhMeterResponse */
    uint8_t hop_dwell;       /* preset index: per-channel dwell while a hunt searches */
    uint8_t research;        /* preset index: silence before a locked hunt searches again */
    uint8_t scan_dwell;      /* preset index: per-channel dwell while scanning */
} WhSettings;

/** Fill `s` with the default settings. */
void wh_settings_reset(WhSettings* s);

/** Map a WhRangeMode to the dBm floor (meter empty) and ceiling (meter full). */
void wh_settings_range_bounds(uint8_t mode, int8_t* floor, int8_t* ceil);

/** Weight (0..1) of each new sample in the hunt meter's smoothing. */
float wh_settings_meter_alpha(uint8_t mode);

/** Hunt dwell per channel while searching, in ms, for a hop_dwell preset. */
uint16_t wh_settings_hop_dwell_ms(uint8_t idx);

/** Silence before a locked hunt searches again, in seconds, for a research preset. */
uint16_t wh_settings_research_s(uint8_t idx);

/** Scan (DISCOVER) dwell per channel, in ms, for a scan_dwell preset. */
uint16_t wh_settings_scan_dwell_ms(uint8_t idx);
