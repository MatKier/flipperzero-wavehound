#include "../wavehound_i.h"
#include "../wh_oui.h"

/* Shown after picking a scan result, before hunting: identity + vendor (from
 * the OUI), with Hunt and Save actions. app->target and app->mac_bytes were set
 * by the target list. */

static void wh_scene_details_button_cb(GuiButtonType result, InputType type, void* context) {
    WavehoundApp* app = context;
    if(type != InputTypeShort) return;
    if(result == GuiButtonTypeLeft) {
        view_dispatcher_send_custom_event(app->view_dispatcher, WhEventDetailsSave);
    } else if(result == GuiButtonTypeRight) {
        view_dispatcher_send_custom_event(app->view_dispatcher, WhEventDetailsHunt);
    }
}

void wh_scene_details_on_enter(void* context) {
    WavehoundApp* app = context;
    const WhTarget* t = &app->target;
    Widget* w = app->widget;
    widget_reset(w);

    /* Identity line: an AP shows its SSID (its row can pick up the gateway IP,
     * so IP must not win there); a station prefers its resolved host / IP when
     * known, matching the client list and hunt header. */
    const char* title;
    if(t->is_ap) {
        title = t->name[0] ? t->name : "(hidden AP)";
    } else if(t->host[0]) {
        title = t->host;
    } else if(t->ip[0]) {
        title = t->ip;
    } else if(t->name[0]) {
        title = t->name;
    } else {
        title = "(device)";
    }
    widget_add_string_element(w, 0, 11, AlignLeft, AlignBottom, FontPrimary, title);
    widget_add_string_element(w, 0, 23, AlignLeft, AlignBottom, FontSecondary, t->mac);

    char vendor[40];
    wh_oui_lookup(app->mac_bytes, vendor, sizeof(vendor));
    char line[56];
    snprintf(line, sizeof(line), "Vendor: %s", vendor);
    widget_add_string_element(w, 0, 35, AlignLeft, AlignBottom, FontSecondary, line);

    /* "<AP|Station>[ chN][ <security>][ <phy>] <rssi>dBm". Security and PHY are
     * only known for access points whose beacon was parsed. */
    size_t n = snprintf(line, sizeof(line), "%s", t->is_ap ? "AP" : "Station");
    if(t->channel) n += snprintf(line + n, sizeof(line) - n, " ch%u", t->channel);
    if(t->security[0]) n += snprintf(line + n, sizeof(line) - n, " %s", t->security);
    if(t->phy[0]) n += snprintf(line + n, sizeof(line) - n, " %s", t->phy);
    snprintf(line + n, sizeof(line) - n, " %ddBm", t->rssi);
    widget_add_string_element(w, 0, 47, AlignLeft, AlignBottom, FontSecondary, line);

    widget_add_button_element(w, GuiButtonTypeLeft, "Save", wh_scene_details_button_cb, app);
    widget_add_button_element(w, GuiButtonTypeRight, "Hunt", wh_scene_details_button_cb, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, WhViewWidget);
}

bool wh_scene_details_on_event(void* context, SceneManagerEvent event) {
    WavehoundApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == WhEventDetailsHunt) {
        scene_manager_next_scene(app->scene_manager, WhSceneHunt);
        return true;
    }
    if(event.event == WhEventDetailsSave) {
        /* Reuse the name-input scene's add path (editing == false). */
        const WhTarget* t = &app->target;
        app->save_channel = t->channel;
        strlcpy(app->name_buf, t->name[0] ? t->name : t->mac, sizeof(app->name_buf));
        app->editing = false;
        scene_manager_next_scene(app->scene_manager, WhSceneNameInput);
        return true;
    }
    return false;
}

void wh_scene_details_on_exit(void* context) {
    WavehoundApp* app = context;
    widget_reset(app->widget);
}
