#include "wh_ap_store.h"

#include <furi.h>
#include <storage/storage.h>
#include <toolbox/stream/buffered_file_stream.h>
#include <toolbox/stream/stream.h>
#include <stdlib.h>
#include <string.h>

#ifndef APP_DATA_PATH
#define APP_DATA_PATH(p) EXT_PATH("apps_data/wavehound/" p)
#endif

#define WH_AP_DIR APP_DATA_PATH("")
#define WH_AP_FILE APP_DATA_PATH("aps.txt")

/* One entry per line: "<ssid>\t<security>\t<psk>". Tabs/newlines are stripped
 * from each field on save, so the split is unambiguous. */

static void sanitize_field(char* s) {
    for(; *s; s++) {
        if(*s == '\t' || *s == '\n' || *s == '\r') *s = ' ';
    }
}

void wh_ap_store_load(WavehoundApp* app) {
    app->saved_ap_count = 0;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    Stream* stream = buffered_file_stream_alloc(storage);

    if(buffered_file_stream_open(stream, WH_AP_FILE, FSAM_READ, FSOM_OPEN_EXISTING)) {
        FuriString* line = furi_string_alloc();
        while(app->saved_ap_count < WH_MAX_SAVED_AP && stream_read_line(stream, line)) {
            furi_string_trim(line, "\r\n");
            if(furi_string_empty(line)) continue;

            char buf[WH_AP_SSID_MAX + 5 + WH_AP_PSK_MAX + 4];
            strlcpy(buf, furi_string_get_cstr(line), sizeof(buf));

            char* tab1 = strchr(buf, '\t');
            if(!tab1) continue;
            *tab1 = '\0';
            char* security = tab1 + 1;
            char* tab2 = strchr(security, '\t');
            if(!tab2) continue;
            *tab2 = '\0';
            char* psk = tab2 + 1;

            if(!buf[0]) continue; /* SSID is required */

            WhSavedAp* a = &app->saved_aps[app->saved_ap_count];
            strlcpy(a->ssid, buf, sizeof(a->ssid));
            strlcpy(a->security, security, sizeof(a->security));
            strlcpy(a->psk, psk, sizeof(a->psk));
            app->saved_ap_count++;
        }
        furi_string_free(line);
    }

    buffered_file_stream_close(stream);
    stream_free(stream);
    furi_record_close(RECORD_STORAGE);
}

static void wh_ap_store_write(WavehoundApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, WH_AP_DIR); /* no-op if it already exists */
    Stream* stream = buffered_file_stream_alloc(storage);

    if(buffered_file_stream_open(stream, WH_AP_FILE, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        for(size_t i = 0; i < app->saved_ap_count; i++) {
            const WhSavedAp* a = &app->saved_aps[i];
            stream_write_format(stream, "%s\t%s\t%s\n", a->ssid, a->security, a->psk);
        }
    }

    buffered_file_stream_close(stream);
    stream_free(stream);
    furi_record_close(RECORD_STORAGE);
}

bool wh_ap_store_add(WavehoundApp* app, const char* ssid, const char* security, const char* psk) {
    if(!ssid || !ssid[0]) return false;

    /* Upsert: update the existing entry for this SSID, else append. */
    WhSavedAp* a = NULL;
    for(size_t i = 0; i < app->saved_ap_count; i++) {
        if(strcmp(app->saved_aps[i].ssid, ssid) == 0) {
            a = &app->saved_aps[i];
            break;
        }
    }
    if(!a) {
        if(app->saved_ap_count >= WH_MAX_SAVED_AP) return false;
        a = &app->saved_aps[app->saved_ap_count++];
    }

    strlcpy(a->ssid, ssid, sizeof(a->ssid));
    strlcpy(a->security, security ? security : "", sizeof(a->security));
    strlcpy(a->psk, psk ? psk : "", sizeof(a->psk));
    sanitize_field(a->ssid);
    sanitize_field(a->security);
    sanitize_field(a->psk);

    wh_ap_store_write(app);
    return true;
}

void wh_ap_store_delete(WavehoundApp* app, size_t index) {
    if(index >= app->saved_ap_count) return;
    for(size_t i = index; i + 1 < app->saved_ap_count; i++) {
        app->saved_aps[i] = app->saved_aps[i + 1];
    }
    app->saved_ap_count--;
    wh_ap_store_write(app);
}

const WhSavedAp* wh_ap_store_find(WavehoundApp* app, const char* ssid) {
    if(!ssid || !ssid[0]) return NULL;
    for(size_t i = 0; i < app->saved_ap_count; i++) {
        if(strcmp(app->saved_aps[i].ssid, ssid) == 0) return &app->saved_aps[i];
    }
    return NULL;
}
