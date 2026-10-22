#include "wh_oui.h"

#include <furi.h>
#include <storage/storage.h>
#include <string.h>

#ifndef APP_DATA_PATH
#define APP_DATA_PATH(p) EXT_PATH("apps_data/wavehound/" p)
#endif
#ifndef APP_ASSETS_PATH
#define APP_ASSETS_PATH(p) EXT_PATH("apps_assets/wavehound/" p)
#endif

/* User-supplied override first, then the database bundled with the app. */
#define WH_OUI_DB_USER APP_DATA_PATH("oui.db")
#define WH_OUI_DB_BUNDLED APP_ASSETS_PATH("oui.db")
/* oui.db is sorted fixed-width records: 3-byte OUI + NUL-padded name.
 * It starts with a header record: "WHOUI" + record size (1 byte), NUL-padded
 * to one record, so the records follow at index 1 and the size can change
 * without an app update. Files without it are the older format: 32-byte
 * records from offset 0. ('W' has the group bit set, so no real OUI record
 * can look like the header.) */
#define WH_OUI_MAGIC "WHOUI"
#define WH_OUI_MAGIC_LEN 5
#define WH_OUI_LEGACY_RECORD 32
#define WH_OUI_NAME_MAX 31 /* longer names are cut */

/* Binary search a sorted fixed-width oui.db on the SD card, if present. */
static bool lookup_file(const char* path, const uint8_t oui[3], char* out, size_t out_size) {
    bool found = false;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);

    if(storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        const uint64_t size = storage_file_size(f);
        size_t record = WH_OUI_LEGACY_RECORD;
        size_t lo = 0;
        uint8_t head[WH_OUI_MAGIC_LEN + 1];
        if(storage_file_read(f, head, sizeof(head)) == sizeof(head) &&
           memcmp(head, WH_OUI_MAGIC, WH_OUI_MAGIC_LEN) == 0) {
            record = head[WH_OUI_MAGIC_LEN];
            lo = 1;
        }
        size_t hi = record > 3 ? (size_t)(size / record) : 0;
        const uint16_t name_len =
            (uint16_t)(record - 3 < WH_OUI_NAME_MAX ? record - 3 : WH_OUI_NAME_MAX);
        while(lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            if(!storage_file_seek(f, (uint32_t)(mid * record), true)) break;
            uint8_t key[3];
            if(storage_file_read(f, key, 3) != 3) break;
            const int c = memcmp(key, oui, 3);
            if(c == 0) {
                char name[WH_OUI_NAME_MAX + 1];
                uint16_t r = storage_file_read(f, (uint8_t*)name, name_len);
                if(r > name_len) r = name_len;
                name[r] = '\0'; /* records are NUL-padded, so this trims cleanly */
                strlcpy(out, name, out_size);
                found = true;
                break;
            } else if(c < 0) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
    }

    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
    return found;
}

void wh_oui_lookup(const uint8_t mac[6], char* out, size_t out_size) {
    if(out_size == 0) return;
    out[0] = '\0';

    if(mac[0] & 0x01) {
        /* Group/multicast address: not a real transmitter OUI. */
        strlcpy(out, "—", out_size);
        return;
    }
    if(mac[0] & 0x02) {
        /* Locally administered = randomized/privacy MAC, no vendor. */
        strlcpy(out, "randomized", out_size);
        return;
    }

    const uint8_t oui[3] = {mac[0], mac[1], mac[2]};
    if(lookup_file(WH_OUI_DB_USER, oui, out, out_size)) return;
    if(lookup_file(WH_OUI_DB_BUNDLED, oui, out, out_size)) return;
    strlcpy(out, "Unknown", out_size);
}
