#include "../wavehound_i.h"

void wh_scene_hunt_on_enter(void* context) {
    WavehoundApp* app = context;
    const WhTarget* t = &app->target;

    wh_hunt_view_set_target(app->hunt, t->name[0] ? t->name : t->mac, t->channel);

    if(app->settings.keep_backlight) {
        notification_message(app->notifications, &sequence_display_backlight_enforce_on);
    }

    char cmd[48];
    snprintf(
        cmd,
        sizeof(cmd),
        "HUNT,%s,%u%s",
        t->mac,
        t->channel,
        t->pin_channel && t->channel ? ",PIN" : "");
    wh_send(app, cmd);
    view_dispatcher_switch_to_view(app->view_dispatcher, WhViewHunt);
}

bool wh_scene_hunt_on_event(void* context, SceneManagerEvent event) {
    WavehoundApp* app = context;
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
    wh_send(app, "STOP");
    /* Always release the enforce, regardless of the current setting: it is a
     * global display state, so toggling "Keep backlight" off between enter and
     * exit must not leave it stuck on. Harmless if it was never enforced. */
    notification_message(app->notifications, &sequence_display_backlight_enforce_auto);
}
