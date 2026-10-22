#include "../wavehound_i.h"

#include <stdio.h>

/* WPS push-button enrolment: tell the board to start WPS and ask the user to
 * press the button on their router. On WPS,OK the board auto-connects, so we
 * move to the connecting screen to wait for the lease. */

void wh_scene_ap_wps_on_enter(void* context) {
    WavehoundApp* app = context;
    Widget* w = app->widget;
    widget_reset(w);

    const WhTarget* ap = &app->clients_ap;
    char line[48];
    snprintf(line, sizeof(line), "%s", ap->name[0] ? ap->name : ap->mac);
    widget_add_string_element(w, 64, 10, AlignCenter, AlignTop, FontPrimary, "WPS");
    widget_add_string_element(w, 64, 26, AlignCenter, AlignTop, FontSecondary, line);
    widget_add_string_element(w, 64, 40, AlignCenter, AlignTop, FontSecondary, "Press WPS on your");
    widget_add_string_element(w, 64, 50, AlignCenter, AlignTop, FontSecondary, "router now...");

    wh_send(app, "WPS");
    view_dispatcher_switch_to_view(app->view_dispatcher, WhViewWidget);
}

bool wh_scene_ap_wps_on_event(void* context, SceneManagerEvent event) {
    WavehoundApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == WhEventWpsOk) {
        /* Credentials captured; the board auto-connects -> wait for the lease. */
        scene_manager_next_scene(app->scene_manager, WhSceneApConnect);
        return true;
    }
    if(event.event == WhEventWpsFail) {
        Widget* w = app->widget;
        widget_reset(w);
        widget_add_string_element(w, 64, 20, AlignCenter, AlignTop, FontPrimary, "WPS failed");
        widget_add_string_element(
            w, 64, 40, AlignCenter, AlignTop, FontSecondary, "Back to try again");
        return true;
    }
    return false;
}

void wh_scene_ap_wps_on_exit(void* context) {
    WavehoundApp* app = context;
    wh_send(app, "WPS,STOP"); /* cancel if still armed; a no-op once it finished */
    widget_reset(app->widget);
}
