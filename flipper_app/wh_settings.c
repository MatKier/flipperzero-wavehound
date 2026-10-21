#include "wh_settings.h"

#include "wavehound_i.h"

#include <furi.h>
#include <storage/storage.h>
#include <toolbox/stream/buffered_file_stream.h>
#include <toolbox/stream/stream.h>
#include <stdlib.h>
#include <string.h>

#ifndef APP_DATA_PATH
#define APP_DATA_PATH(p) EXT_PATH("apps_data/wavehound/" p)
#endif

#define WH_SETTINGS_DIR APP_DATA_PATH("")
#define WH_SETTINGS_FILE APP_DATA_PATH("settings.txt")

/* One "key=value" per line. Unknown keys are ignored, missing keys keep their
 * default, so the file stays forward/backward compatible. */

/* Board timing presets. Defaults match the firmware's built-in values, so older
 * board firmware (which ignores the timing tokens) behaves the same. */
static const uint16_t hop_dwell_ms[WH_HOP_DWELL_COUNT] = {100, 200, 300, 500, 1000};
static const uint16_t research_s[WH_RESEARCH_COUNT] = {4, 8, 15, 30, 60};
static const uint16_t scan_dwell_ms[WH_SCAN_DWELL_COUNT] = {120, 250, 500, 1000};

void wh_settings_reset(WhSettings* s) {
    s->click_volume = 0.5f;
    s->mute_default = false;
    s->saved_hunt_mode = WhSavedHuntSavedChannel;
    s->haptic = false;
    s->keep_backlight = true;
    s->range_mode = WhRangeNormal;
    s->meter_response = WhResponseNormal;
    s->hop_dwell = 2;  /* 300 ms */
    s->research = 1;   /* 8 s */
    s->scan_dwell = 0; /* 120 ms */
}

void wh_settings_range_bounds(uint8_t mode, int8_t* floor, int8_t* ceil) {
    switch(mode) {
    case WhRangeNear:
        *floor = -75;
        *ceil = -25;
        break;
    case WhRangeFar:
        *floor = -100;
        *ceil = -50;
        break;
    case WhRangeNormal:
    default:
        *floor = -95;
        *ceil = -30;
        break;
    }
}

float wh_settings_meter_alpha(uint8_t mode) {
    switch(mode) {
    case WhResponseFast:
        return 0.6f;
    case WhResponseSmooth:
        return 0.2f;
    case WhResponseNormal:
    default:
        return 0.4f;
    }
}

uint16_t wh_settings_hop_dwell_ms(uint8_t idx) {
    return hop_dwell_ms[idx < WH_HOP_DWELL_COUNT ? idx : 2];
}

uint16_t wh_settings_research_s(uint8_t idx) {
    return research_s[idx < WH_RESEARCH_COUNT ? idx : 1];
}

uint16_t wh_settings_scan_dwell_ms(uint8_t idx) {
    return scan_dwell_ms[idx < WH_SCAN_DWELL_COUNT ? idx : 0];
}

void wh_settings_apply(WavehoundApp* app) {
    const WhSettings* s = &app->settings;
    wh_hunt_view_set_volume(app->hunt, s->click_volume);
    wh_hunt_view_set_sound(app->hunt, !s->mute_default);
    wh_hunt_view_set_haptic(app->hunt, s->haptic);
    int8_t range_floor, range_ceil;
    wh_settings_range_bounds(s->range_mode, &range_floor, &range_ceil);
    wh_hunt_view_set_range(app->hunt, range_floor, range_ceil);
    wh_hunt_view_set_response(app->hunt, wh_settings_meter_alpha(s->meter_response));
}

static void apply_kv(WhSettings* s, const char* key, const char* value) {
    if(strcmp(key, "volume") == 0) {
        /* stored as tenths (1..10) to avoid float parsing */
        int tenths = atoi(value);
        if(tenths < 1) tenths = 1;
        if(tenths > 10) tenths = 10;
        s->click_volume = (float)tenths / 10.0f;
    } else if(strcmp(key, "mute") == 0) {
        s->mute_default = atoi(value) != 0;
    } else if(strcmp(key, "saved_hunt") == 0) {
        int m = atoi(value);
        if(m >= 0 && m < WhSavedHuntModeCount) s->saved_hunt_mode = (uint8_t)m;
    } else if(strcmp(key, "haptic") == 0) {
        s->haptic = atoi(value) != 0;
    } else if(strcmp(key, "keep_backlight") == 0) {
        s->keep_backlight = atoi(value) != 0;
    } else if(strcmp(key, "range") == 0) {
        int m = atoi(value);
        if(m >= 0 && m < WhRangeModeCount) s->range_mode = (uint8_t)m;
    } else if(strcmp(key, "response") == 0) {
        int m = atoi(value);
        if(m >= 0 && m < WhResponseModeCount) s->meter_response = (uint8_t)m;
    } else if(strcmp(key, "hop_dwell") == 0) {
        int m = atoi(value);
        if(m >= 0 && m < WH_HOP_DWELL_COUNT) s->hop_dwell = (uint8_t)m;
    } else if(strcmp(key, "research") == 0) {
        int m = atoi(value);
        if(m >= 0 && m < WH_RESEARCH_COUNT) s->research = (uint8_t)m;
    } else if(strcmp(key, "scan_dwell") == 0) {
        int m = atoi(value);
        if(m >= 0 && m < WH_SCAN_DWELL_COUNT) s->scan_dwell = (uint8_t)m;
    }
}

void wh_settings_load(WavehoundApp* app) {
    wh_settings_reset(&app->settings);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    Stream* stream = buffered_file_stream_alloc(storage);

    if(buffered_file_stream_open(stream, WH_SETTINGS_FILE, FSAM_READ, FSOM_OPEN_EXISTING)) {
        FuriString* line = furi_string_alloc();
        while(stream_read_line(stream, line)) {
            furi_string_trim(line, "\r\n");
            if(furi_string_empty(line)) continue;

            char buf[48];
            strlcpy(buf, furi_string_get_cstr(line), sizeof(buf));
            char* eq = strchr(buf, '=');
            if(!eq) continue;
            *eq = '\0';
            apply_kv(&app->settings, buf, eq + 1);
        }
        furi_string_free(line);
    }

    buffered_file_stream_close(stream);
    stream_free(stream);
    furi_record_close(RECORD_STORAGE);
}

void wh_settings_save(WavehoundApp* app) {
    const WhSettings* s = &app->settings;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, WH_SETTINGS_DIR); /* no-op if it already exists */
    Stream* stream = buffered_file_stream_alloc(storage);

    if(buffered_file_stream_open(stream, WH_SETTINGS_FILE, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        /* tenths (1..10); parsed back without float conversion */
        stream_write_format(stream, "volume=%u\n", (unsigned)(s->click_volume * 10.0f + 0.5f));
        stream_write_format(stream, "mute=%u\n", s->mute_default ? 1u : 0u);
        stream_write_format(stream, "saved_hunt=%u\n", s->saved_hunt_mode);
        stream_write_format(stream, "haptic=%u\n", s->haptic ? 1u : 0u);
        stream_write_format(stream, "keep_backlight=%u\n", s->keep_backlight ? 1u : 0u);
        stream_write_format(stream, "range=%u\n", s->range_mode);
        stream_write_format(stream, "response=%u\n", s->meter_response);
        /* preset indices, not ms/s */
        stream_write_format(stream, "hop_dwell=%u\n", s->hop_dwell);
        stream_write_format(stream, "research=%u\n", s->research);
        stream_write_format(stream, "scan_dwell=%u\n", s->scan_dwell);
    }

    buffered_file_stream_close(stream);
    stream_free(stream);
    furi_record_close(RECORD_STORAGE);
}
