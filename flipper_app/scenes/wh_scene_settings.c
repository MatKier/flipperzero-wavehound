#include "../wavehound_i.h"
#include "../wh_settings.h"

#include <stdio.h>

/* App settings, edited with a VariableItemList and saved on exit. */

#define VOLUME_COUNT 10 /* 0.1 .. 1.0 in 0.1 steps */

static const char* const sound_names[] = {"On", "Muted"};
static const char* const saved_hunt_names[] = {"Saved", "Auto-hop", "Nearby AP"};

static void wh_settings_volume_text(VariableItem* item, uint8_t idx) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%.1f", (double)((float)(idx + 1) / 10.0f));
    variable_item_set_current_value_text(item, buf);
}

static void volume_changed(VariableItem* item) {
    WavehoundApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->settings.click_volume = (float)(idx + 1) / 10.0f;
    wh_settings_volume_text(item, idx);
}

static void sound_changed(VariableItem* item) {
    WavehoundApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->settings.mute_default = idx == 1;
    variable_item_set_current_value_text(item, sound_names[idx]);
}

static void saved_hunt_changed(VariableItem* item) {
    WavehoundApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->settings.saved_hunt_mode = idx;
    variable_item_set_current_value_text(item, saved_hunt_names[idx]);
}

void wh_scene_settings_on_enter(void* context) {
    WavehoundApp* app = context;
    VariableItemList* list = app->var_item_list;
    variable_item_list_reset(list);
    VariableItem* item;

    /* Click volume: value index 0..9 maps to 0.1..1.0 */
    uint8_t vidx = (uint8_t)(app->settings.click_volume * 10.0f + 0.5f);
    if(vidx < 1) vidx = 1;
    if(vidx > VOLUME_COUNT) vidx = VOLUME_COUNT;
    vidx -= 1;
    item = variable_item_list_add(list, "Click volume", VOLUME_COUNT, volume_changed, app);
    variable_item_set_current_value_index(item, vidx);
    wh_settings_volume_text(item, vidx);

    /* Default sound (hunt screen's initial mute state) */
    uint8_t sidx = app->settings.mute_default ? 1 : 0;
    item = variable_item_list_add(list, "Sound", 2, sound_changed, app);
    variable_item_set_current_value_index(item, sidx);
    variable_item_set_current_value_text(item, sound_names[sidx]);

    /* Which channel the picker pre-selects when hunting a saved MAC */
    uint8_t hidx = app->settings.saved_hunt_mode;
    if(hidx >= WhSavedHuntModeCount) hidx = 0;
    item =
        variable_item_list_add(list, "Saved hunt ch", WhSavedHuntModeCount, saved_hunt_changed, app);
    variable_item_set_current_value_index(item, hidx);
    variable_item_set_current_value_text(item, saved_hunt_names[hidx]);

    view_dispatcher_switch_to_view(app->view_dispatcher, WhViewVarItemList);
}

bool wh_scene_settings_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void wh_scene_settings_on_exit(void* context) {
    WavehoundApp* app = context;
    wh_settings_save(app);
    /* Apply straight away so the next hunt uses the new values. */
    wh_hunt_view_set_volume(app->hunt, app->settings.click_volume);
    wh_hunt_view_set_sound(app->hunt, !app->settings.mute_default);
    variable_item_list_reset(app->var_item_list);
}
