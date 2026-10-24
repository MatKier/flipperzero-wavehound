#pragma once

#include <gui/view.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct WhHuntView WhHuntView;

/** Called when the user asks to save the current target (Right key). */
typedef void (*WhHuntSaveCallback)(void* context);

/** Called when the user presses Down to toggle the elicitation ping. */
typedef void (*WhHuntElicitCallback)(void* context);

WhHuntView* wh_hunt_view_alloc(void);
void wh_hunt_view_free(WhHuntView* hunt);
View* wh_hunt_view_get_view(WhHuntView* hunt);

/** Register the callback fired when the user presses Right to save the target. */
void wh_hunt_view_set_save_callback(WhHuntView* hunt, WhHuntSaveCallback cb, void* context);

/** Register the callback fired when the user presses Down to toggle elicitation. */
void wh_hunt_view_set_elicit_callback(WhHuntView* hunt, WhHuntElicitCallback cb, void* context);

/** Set the target's IP / hostname / device class (the header label is the first of
 * hostname, class, IP that is set; any may be ""). */
void wh_hunt_view_set_ip(
    WhHuntView* hunt,
    const char* ip,
    const char* host,
    const char* dtype);

/** Whether elicitation is offered here (connected + target IP known). */
void wh_hunt_view_set_elicit_available(WhHuntView* hunt, bool available);

/** Reflect the current elicitation on/off state (for the indicator). */
void wh_hunt_view_set_elicit_on(WhHuntView* hunt, bool on);

/** Set the geiger click loudness (0..1). */
void wh_hunt_view_set_volume(WhHuntView* hunt, float volume);

/** Set whether sound is on (the hunt's default mute state). */
void wh_hunt_view_set_sound(WhHuntView* hunt, bool on);

/** Enable the haptic proximity cue (motor buzzes faster as the signal grows). */
void wh_hunt_view_set_haptic(WhHuntView* hunt, bool on);

/** Set the dBm window the meter and click/haptic rate span (floor..ceil). */
void wh_hunt_view_set_range(WhHuntView* hunt, int8_t floor, int8_t ceil);

/** Set how much each new sample moves the meter (0..1; higher = faster, jumpier). */
void wh_hunt_view_set_response(WhHuntView* hunt, float alpha);

/** Channel the hunt is currently locked on, or 0 if still hopping/seeking. */
uint8_t wh_hunt_view_current_channel(WhHuntView* hunt);

/** Reset the view for a new target. */
void wh_hunt_view_set_target(WhHuntView* hunt, const char* name, uint8_t channel);

/** Feed one `H` sample from the board. Safe to call from any thread. `nf` is the
 * mean noise floor in dBm (0 when the target was not heard, or from firmware too
 * old to send it). */
void wh_hunt_view_push_sample(
    WhHuntView* hunt,
    uint8_t channel,
    uint32_t frames,
    int8_t last,
    int8_t avg,
    int8_t max,
    bool locked,
    int8_t nf);
