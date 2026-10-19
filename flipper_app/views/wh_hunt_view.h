#pragma once

#include <gui/view.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct WhHuntView WhHuntView;

/** Called when the user asks to save the current target (Right key). */
typedef void (*WhHuntSaveCallback)(void* context);

WhHuntView* wh_hunt_view_alloc(void);
void wh_hunt_view_free(WhHuntView* hunt);
View* wh_hunt_view_get_view(WhHuntView* hunt);

/** Register the callback fired when the user presses Right to save the target. */
void wh_hunt_view_set_save_callback(WhHuntView* hunt, WhHuntSaveCallback cb, void* context);

/** Set the geiger click loudness (0..1). */
void wh_hunt_view_set_volume(WhHuntView* hunt, float volume);

/** Set whether sound is on (the hunt's default mute state). */
void wh_hunt_view_set_sound(WhHuntView* hunt, bool on);

/** Enable the haptic proximity cue (motor buzzes faster as the signal grows). */
void wh_hunt_view_set_haptic(WhHuntView* hunt, bool on);

/** Set the dBm window the meter and click/haptic rate span (floor..ceil). */
void wh_hunt_view_set_range(WhHuntView* hunt, int8_t floor, int8_t ceil);

/** Channel the hunt is currently locked on, or 0 if still hopping/seeking. */
uint8_t wh_hunt_view_current_channel(WhHuntView* hunt);

/** Reset the view for a new target. */
void wh_hunt_view_set_target(WhHuntView* hunt, const char* name, uint8_t channel);

/** Feed one `H` sample from the board. Safe to call from any thread. */
void wh_hunt_view_push_sample(
    WhHuntView* hunt,
    uint8_t channel,
    uint32_t frames,
    int8_t last,
    int8_t avg,
    int8_t max,
    bool locked);
