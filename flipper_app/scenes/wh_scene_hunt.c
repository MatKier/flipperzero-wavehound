#include "../wavehound_i.h"

#include <stdio.h>

/* Whether the elicitation ping is currently running (reset on each hunt). */
static bool s_elicit_on;

/* Ticks since the last RESOLVE nudge (reset on each hunt). */
static uint32_t s_resolve_ticks;
#define WH_HUNT_RESOLVE_PERIOD_TICKS (3000 / WH_TICK_MS) /* re-ask about every 3 s */

/* Elicitation is offered only when associated and the target's IP is known. */
static bool wh_scene_hunt_elicit_avail(WavehoundApp* app) {
    return app->conn.connected && app->target.ip[0] != '\0';
}

/* While hunting a connected target whose IP we don't have yet, nudge the board
 * to re-run host discovery so the IP (and name) can fill in and elicitation
 * becomes available. A no-op once resolved, or on boards without RESOLVE. */
static void wh_scene_hunt_maybe_resolve(WavehoundApp* app) {
    if(app->conn.connected && app->target.ip[0] == '\0' &&
       app->board_fw_version >= WH_FW_RESOLVE_MIN) {
        wh_send(app, "RESOLVE");
    }
}

void wh_scene_hunt_on_enter(void* context) {
    WavehoundApp* app = context;
    const WhTarget* t = &app->target;

    wh_hunt_view_set_target(app->hunt, t->name[0] ? t->name : t->mac, t->channel);
    wh_hunt_view_set_ip(app->hunt, t->ip, t->host);
    s_elicit_on = false;
    wh_hunt_view_set_elicit_on(app->hunt, false);
    wh_hunt_view_set_elicit_available(app->hunt, wh_scene_hunt_elicit_avail(app));
    s_resolve_ticks = 0;
    wh_scene_hunt_maybe_resolve(app); /* kick a lookup right away if IP unknown */

    if(app->settings.keep_backlight) {
        notification_message(app->notifications, &sequence_display_backlight_enforce_on);
    }

    /* PIN stays right after <ch>: older firmware only looks for it there and
     * ignores the timing tokens that follow. */
    char cmd[72];
    snprintf(
        cmd,
        sizeof(cmd),
        "HUNT,%s,%u%s,DWELL=%u,LOST=%u",
        t->mac,
        t->channel,
        t->pin_channel && t->channel ? ",PIN" : "",
        wh_settings_hop_dwell_ms(app->settings.hop_dwell),
        wh_settings_research_s(app->settings.research));
    wh_send(app, cmd);
    view_dispatcher_switch_to_view(app->view_dispatcher, WhViewHunt);
}

bool wh_scene_hunt_on_event(void* context, SceneManagerEvent event) {
    WavehoundApp* app = context;

    if(event.type == SceneManagerEventTypeTick) {
        /* The target's IP (or the connection) may change after we enter, so keep
         * the Down-key availability in step. If the link drops, the board has
         * already stopped eliciting, so clear our indicator too. */
        const bool avail = wh_scene_hunt_elicit_avail(app);
        wh_hunt_view_set_elicit_available(app->hunt, avail);
        if(!avail && s_elicit_on) {
            s_elicit_on = false;
            wh_hunt_view_set_elicit_on(app->hunt, false);
        }
        /* Keep nudging the board to resolve the target's IP until it is known. */
        if(++s_resolve_ticks >= WH_HUNT_RESOLVE_PERIOD_TICKS) {
            s_resolve_ticks = 0;
            wh_scene_hunt_maybe_resolve(app);
        }
        return false;
    }

    if(event.type == SceneManagerEventTypeCustom && event.event == WhEventElicitToggle) {
        if(!wh_scene_hunt_elicit_avail(app)) return true; /* not connected / no IP */
        s_elicit_on = !s_elicit_on;
        if(s_elicit_on) {
            char cmd[48];
            snprintf(
                cmd,
                sizeof(cmd),
                "ELICIT,%s,%u",
                app->target.mac,
                wh_settings_ping_interval_ms(app->settings.ping_interval));
            wh_send(app, cmd);
        } else {
            wh_send(app, "ELICIT,STOP");
        }
        wh_hunt_view_set_elicit_on(app->hunt, s_elicit_on);
        return true;
    }

    if(event.type == SceneManagerEventTypeCustom && event.event == WhEventSaveTarget) {
        /* Remember the channel we are actually locked on (fall back to the
         * configured one, which may be 0 = auto), then name and save it. */
        app->save_channel = wh_hunt_view_current_channel(app->hunt);
        if(app->save_channel == 0) app->save_channel = app->target.channel;
        const WhTarget* t = &app->target;
        strlcpy(app->name_buf, t->name[0] ? t->name : t->mac, sizeof(app->name_buf));
        app->editing = false; /* adding a new favorite, not editing one */
        scene_manager_next_scene(app->scene_manager, WhSceneNameInput);
        return true;
    }
    return false;
}

void wh_scene_hunt_on_exit(void* context) {
    WavehoundApp* app = context;
    /* STOP also stops any elicitation on the board. */
    s_elicit_on = false;
    wh_send(app, "STOP");
    /* Always release the enforce, regardless of the current setting: it is a
     * global display state, so toggling "Keep backlight" off between enter and
     * exit must not leave it stuck on. Harmless if it was never enforced. */
    notification_message(app->notifications, &sequence_display_backlight_enforce_auto);
}
