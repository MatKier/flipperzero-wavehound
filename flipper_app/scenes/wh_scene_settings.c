#include "../wavehound_i.h"
#include "../wh_settings.h"

#include <stdio.h>

/* App settings, edited with a VariableItemList and saved on exit. */

#define VOLUME_COUNT 10 /* 0.1 .. 1.0 in 0.1 steps */
#define WH_SETTINGS_RESET_INDEX 11 /* position of "Reset to defaults" in the list */

static const char* const sound_names[] = {"On", "Muted"};
static const char* const saved_hunt_names[] = {"Saved", "Auto-hop", "Nearby AP"};
static const char* const off_on_names[] = {"Off", "On"};
static const char* const range_names[] = {"Near", "Normal", "Far"};
static const char* const response_names[] = {"Fast", "Normal", "Smooth"};

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

static void range_changed(VariableItem* item) {
    WavehoundApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->settings.range_mode = idx;
    variable_item_set_current_value_text(item, range_names[idx]);
}

static void response_changed(VariableItem* item) {
    WavehoundApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->settings.meter_response = idx;
    variable_item_set_current_value_text(item, response_names[idx]);
}

static void wh_settings_ms_text(VariableItem* item, uint16_t ms) {
    char buf[8];
    if(ms >= 1000 && ms % 1000 == 0) {
        snprintf(buf, sizeof(buf), "%us", ms / 1000);
    } else {
        snprintf(buf, sizeof(buf), "%ums", ms);
    }
    variable_item_set_current_value_text(item, buf);
}

static void wh_settings_s_text(VariableItem* item, uint16_t s) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%us", s);
    variable_item_set_current_value_text(item, buf);
}

static void hop_dwell_changed(VariableItem* item) {
    WavehoundApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->settings.hop_dwell = idx;
    wh_settings_ms_text(item, wh_settings_hop_dwell_ms(idx));
}

static void research_changed(VariableItem* item) {
    WavehoundApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->settings.research = idx;
    wh_settings_s_text(item, wh_settings_research_s(idx));
}

static void scan_dwell_changed(VariableItem* item) {
    WavehoundApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->settings.scan_dwell = idx;
    wh_settings_ms_text(item, wh_settings_scan_dwell_ms(idx));
}

static void ping_interval_changed(VariableItem* item) {
    WavehoundApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->settings.ping_interval = idx;
    wh_settings_ms_text(item, wh_settings_ping_interval_ms(idx));
}

static void haptic_changed(VariableItem* item) {
    WavehoundApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->settings.haptic = idx == 1;
    variable_item_set_current_value_text(item, off_on_names[idx]);
}

static void keep_backlight_changed(VariableItem* item) {
    WavehoundApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->settings.keep_backlight = idx == 1;
    variable_item_set_current_value_text(item, off_on_names[idx]);
}

/* OK on a row: only the last one ("Reset to defaults") reacts. Rebuilding the
 * list happens in on_event, not inside the list's own callback. */
static void wh_settings_enter_callback(void* context, uint32_t index) {
    WavehoundApp* app = context;
    if(index == WH_SETTINGS_RESET_INDEX) {
        view_dispatcher_send_custom_event(app->view_dispatcher, WhEventSettingsReset);
    }
}

static void wh_settings_build_list(WavehoundApp* app) {
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

    /* How long a hunt lingers on each channel while searching for the target */
    uint8_t didx = app->settings.hop_dwell;
    if(didx >= WH_HOP_DWELL_COUNT) didx = 2;
    item = variable_item_list_add(list, "Hop dwell", WH_HOP_DWELL_COUNT, hop_dwell_changed, app);
    variable_item_set_current_value_index(item, didx);
    wh_settings_ms_text(item, wh_settings_hop_dwell_ms(didx));

    /* Silence before a locked hunt gives up the channel and searches again */
    uint8_t qidx = app->settings.research;
    if(qidx >= WH_RESEARCH_COUNT) qidx = 1;
    item = variable_item_list_add(list, "Re-search", WH_RESEARCH_COUNT, research_changed, app);
    variable_item_set_current_value_index(item, qidx);
    wh_settings_s_text(item, wh_settings_research_s(qidx));

    /* Meter/click range preset (shifts the dBm window the meter spans) */
    uint8_t ridx = app->settings.range_mode;
    if(ridx >= WhRangeModeCount) ridx = WhRangeNormal;
    item = variable_item_list_add(list, "Range", WhRangeModeCount, range_changed, app);
    variable_item_set_current_value_index(item, ridx);
    variable_item_set_current_value_text(item, range_names[ridx]);

    /* Meter smoothing: how fast the readout follows new samples */
    uint8_t pidx = app->settings.meter_response;
    if(pidx >= WhResponseModeCount) pidx = WhResponseNormal;
    item = variable_item_list_add(list, "Response", WhResponseModeCount, response_changed, app);
    variable_item_set_current_value_index(item, pidx);
    variable_item_set_current_value_text(item, response_names[pidx]);

    /* How long a scan lingers on each channel */
    uint8_t cidx = app->settings.scan_dwell;
    if(cidx >= WH_SCAN_DWELL_COUNT) cidx = 0;
    item =
        variable_item_list_add(list, "Scan dwell", WH_SCAN_DWELL_COUNT, scan_dwell_changed, app);
    variable_item_set_current_value_index(item, cidx);
    wh_settings_ms_text(item, wh_settings_scan_dwell_ms(cidx));

    /* Haptic proximity cue (buzzes faster as the signal strengthens) */
    uint8_t vbidx = app->settings.haptic ? 1 : 0;
    item = variable_item_list_add(list, "Haptic", 2, haptic_changed, app);
    variable_item_set_current_value_index(item, vbidx);
    variable_item_set_current_value_text(item, off_on_names[vbidx]);

    /* Hold the backlight on while hunting */
    uint8_t blidx = app->settings.keep_backlight ? 1 : 0;
    item = variable_item_list_add(list, "Keep backlight", 2, keep_backlight_changed, app);
    variable_item_set_current_value_index(item, blidx);
    variable_item_set_current_value_text(item, off_on_names[blidx]);

    /* Elicitation ping interval (hunt screen, Down key) */
    uint8_t piidx = app->settings.ping_interval;
    if(piidx >= WH_PING_INTERVAL_COUNT) piidx = 2;
    item =
        variable_item_list_add(list, "Ping interval", WH_PING_INTERVAL_COUNT, ping_interval_changed, app);
    variable_item_set_current_value_index(item, piidx);
    wh_settings_ms_text(item, wh_settings_ping_interval_ms(piidx));

    /* Press OK to restore every setting above to its default */
    variable_item_list_add(list, "Reset to defaults", 0, NULL, app);
    variable_item_list_set_enter_callback(list, wh_settings_enter_callback, app);
}

void wh_scene_settings_on_enter(void* context) {
    WavehoundApp* app = context;
    wh_settings_build_list(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, WhViewVarItemList);
}

bool wh_scene_settings_on_event(void* context, SceneManagerEvent event) {
    WavehoundApp* app = context;
    if(event.type == SceneManagerEventTypeCustom && event.event == WhEventSettingsReset) {
        wh_settings_reset(&app->settings);
        wh_settings_build_list(app);
        variable_item_list_set_selected_item(app->var_item_list, WH_SETTINGS_RESET_INDEX);
        return true;
    }
    return false;
}

void wh_scene_settings_on_exit(void* context) {
    WavehoundApp* app = context;
    wh_settings_save(app);
    /* Apply straight away so the next hunt uses the new values. */
    wh_settings_apply(app);
    variable_item_list_reset(app->var_item_list);
}
