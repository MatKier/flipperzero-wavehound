#include "../wavehound_i.h"
#include "../wh_store.h"

/* Actions for one saved MAC, chosen from the Saved list. app->edit_index names
 * the entry. */

#define ENTRY_HUNT 0
#define ENTRY_EDIT 1
#define ENTRY_DELETE 2

static void wh_scene_saved_entry_callback(void* context, uint32_t index) {
    WavehoundApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void wh_scene_saved_entry_on_enter(void* context) {
    WavehoundApp* app = context;
    if(app->edit_index >= app->saved_count) {
        scene_manager_previous_scene(app->scene_manager);
        return;
    }
    Submenu* submenu = app->submenu;
    submenu_reset(submenu);
    submenu_set_header(submenu, app->saved[app->edit_index].name);
    submenu_add_item(submenu, "Hunt", ENTRY_HUNT, wh_scene_saved_entry_callback, app);
    submenu_add_item(submenu, "Edit", ENTRY_EDIT, wh_scene_saved_entry_callback, app);
    submenu_add_item(submenu, "Delete", ENTRY_DELETE, wh_scene_saved_entry_callback, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, WhViewSubmenu);
}

bool wh_scene_saved_entry_on_event(void* context, SceneManagerEvent event) {
    WavehoundApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;
    if(app->edit_index >= app->saved_count) return false;
    const WhSavedMac* s = &app->saved[app->edit_index];

    switch(event.event) {
    case ENTRY_HUNT:
        /* Seed the target and open the channel picker (pre-selects the saved
         * channel), same as a scan pick. */
        memset(&app->target, 0, sizeof(WhTarget));
        strlcpy(app->target.mac, s->mac, sizeof(app->target.mac));
        strlcpy(app->target.name, s->name, sizeof(app->target.name));
        app->target.channel = s->channel;
        app->target.pin_channel = s->channel != 0;
        wh_parse_mac(app->target.mac, app->mac_bytes);
        app->target.randomized = app->mac_bytes[0] & 0x02;
        app->channel_purpose = WhChannelForMacHunt;
        app->from_saved_hunt = true; /* channel picker pre-selects per settings */
        scene_manager_next_scene(app->scene_manager, WhSceneChannelSelect);
        return true;
    case ENTRY_EDIT:
        /* Load the working copy and open the editor. */
        strlcpy(app->name_buf, s->name, sizeof(app->name_buf));
        wh_parse_mac(s->mac, app->mac_bytes);
        app->edit_channel = s->channel;
        scene_manager_next_scene(app->scene_manager, WhSceneEditSaved);
        return true;
    case ENTRY_DELETE:
        wh_store_delete(app, app->edit_index);
        scene_manager_previous_scene(app->scene_manager); /* back to the list */
        return true;
    default:
        return false;
    }
}

void wh_scene_saved_entry_on_exit(void* context) {
    WavehoundApp* app = context;
    submenu_reset(app->submenu);
}
