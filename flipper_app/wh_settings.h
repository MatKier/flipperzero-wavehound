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

/** User preferences, persisted to the SD card. */
typedef struct {
    float click_volume;      /* geiger click loudness, 0.1..1.0 */
    bool mute_default;       /* hunt screen starts muted */
    uint8_t saved_hunt_mode; /* WhSavedHuntMode */
    bool haptic;             /* buzz the motor as a proximity cue while hunting */
    bool keep_backlight;     /* hold the backlight on during a hunt */
    uint8_t range_mode;      /* WhRangeMode */
} WhSettings;

/** Map a WhRangeMode to the dBm floor (meter empty) and ceiling (meter full). */
void wh_settings_range_bounds(uint8_t mode, int8_t* floor, int8_t* ceil);
