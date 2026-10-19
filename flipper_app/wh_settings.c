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

static void wh_settings_defaults(WhSettings* s) {
    s->click_volume = 0.5f;
    s->mute_default = false;
    s->saved_hunt_mode = WhSavedHuntSavedChannel;
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
    }
}

void wh_settings_load(WavehoundApp* app) {
    wh_settings_defaults(&app->settings);

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
    }

    buffered_file_stream_close(stream);
    stream_free(stream);
    furi_record_close(RECORD_STORAGE);
}
