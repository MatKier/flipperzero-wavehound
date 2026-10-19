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

/** User preferences, persisted to the SD card. */
typedef struct {
    float click_volume;      /* geiger click loudness, 0.1..1.0 */
    bool mute_default;       /* hunt screen starts muted */
    uint8_t saved_hunt_mode; /* WhSavedHuntMode */
} WhSettings;
