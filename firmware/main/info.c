#include "info.h"

#include <stdio.h>
#include <stdint.h>

#include "driver/temperature_sensor.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "proto.h"

static const char *reset_reason_str(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "SDIO";
    default: return "unknown";
    }
}

static const char *chip_model_str(esp_chip_model_t model)
{
    switch (model) {
    case CHIP_ESP32: return "ESP32";
    case CHIP_ESP32S2: return "ESP32-S2";
    case CHIP_ESP32S3: return "ESP32-S3";
    case CHIP_ESP32C3: return "ESP32-C3";
    default: return "ESP32?";
    }
}

/* Read the chip's internal temperature sensor. Returns false if unavailable. */
static bool read_temp_celsius(float *out)
{
    temperature_sensor_handle_t sensor = NULL;
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&cfg, &sensor) != ESP_OK) {
        return false;
    }
    bool ok = temperature_sensor_enable(sensor) == ESP_OK &&
              temperature_sensor_get_celsius(sensor, out) == ESP_OK;
    temperature_sensor_disable(sensor);
    temperature_sensor_uninstall(sensor);
    return ok;
}

/* Walk the running app image to find how many bytes of flash it occupies.
 * Best-effort: returns false (and the caller skips the line) on anything
 * unexpected rather than reporting a wrong number. */
static bool app_image_bytes(const esp_partition_t *part, size_t *out)
{
    esp_image_header_t hdr;
    if (esp_partition_read(part, 0, &hdr, sizeof(hdr)) != ESP_OK ||
        hdr.magic != ESP_IMAGE_HEADER_MAGIC) {
        return false;
    }
    size_t pos = sizeof(esp_image_header_t);
    for (unsigned i = 0; i < hdr.segment_count; i++) {
        esp_image_segment_header_t seg;
        if (esp_partition_read(part, pos, &seg, sizeof(seg)) != ESP_OK) {
            return false;
        }
        pos += sizeof(seg) + seg.data_len;
        if (pos > part->size) {
            return false;
        }
    }
    pos = (pos + 1 + 15) & ~(size_t)15; /* 1-byte checksum, padded to 16 bytes */
    if (hdr.hash_appended) {
        pos += 32; /* appended SHA-256 */
    }
    if (pos == 0 || pos > part->size) {
        return false;
    }
    *out = pos;
    return true;
}

void info_report(void)
{
    proto_sendf("INFO_BEGIN");

    const esp_app_desc_t *app = esp_app_get_description();
    /* Our own firmware version, the same value READY/PONG report. */
    proto_sendf("BI,Firmware,%s", WH_FW_VERSION);
    if (app) {
        /* The build system's version string -- `git describe` for a git
         * checkout (nearest tag / commit, +dirty), else version.txt or
         * PROJECT_VER. Kept to identify exactly which build is flashed. */
        proto_sendf("BI,Commit,%s", app->version);
        proto_sendf("BI,Built,%s %s", app->date, app->time);
        proto_sendf("BI,IDF (build),%s", app->idf_ver);
        char sha[17];
        for (int i = 0; i < 8; i++) {
            snprintf(sha + i * 2, sizeof(sha) - i * 2, "%02x", app->app_elf_sha256[i]);
        }
        proto_sendf("BI,ELF SHA,%s", sha);
    }
    proto_sendf("BI,IDF (run),%s", esp_get_idf_version());

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    proto_sendf("BI,Chip,%s rev %d", chip_model_str(chip.model), chip.revision);
    proto_sendf("BI,Cores,%d", chip.cores);

    /* Feature list joined with '/' so no value carries a comma. */
    char feat[48];
    size_t fl = 0;
    feat[0] = '\0';
#define ADD_FEAT(bit, name)                                                    \
    if ((chip.features & (bit)) && fl < sizeof(feat)) {                        \
        fl += snprintf(feat + fl, sizeof(feat) - fl, "%s%s", fl ? "/" : "", name); \
    }
    ADD_FEAT(CHIP_FEATURE_WIFI_BGN, "WiFi");
    ADD_FEAT(CHIP_FEATURE_BT, "BT");
    ADD_FEAT(CHIP_FEATURE_BLE, "BLE");
    ADD_FEAT(CHIP_FEATURE_EMB_FLASH, "flash");
    ADD_FEAT(CHIP_FEATURE_EMB_PSRAM, "PSRAM");
#undef ADD_FEAT
    proto_sendf("BI,Radio,%s", feat[0] ? feat : "none");

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    proto_sendf("BI,MAC,%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
                mac[5]);

    uint32_t flash = 0;
    if (esp_flash_get_size(NULL, &flash) == ESP_OK) {
        proto_sendf("BI,Flash,%lu MB", (unsigned long)(flash / (1024 * 1024)));
    }

    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run) {
        proto_sendf("BI,App part,%s %lu KB", run->label, (unsigned long)(run->size / 1024));
        size_t used;
        if (app_image_bytes(run, &used)) {
            proto_sendf("BI,App used,%lu KB (%lu%% of part)", (unsigned long)(used / 1024),
                        (unsigned long)(run->size ? (uint64_t)used * 100 / run->size : 0));
        }
    }

    proto_sendf("BI,Heap free,%lu B", (unsigned long)esp_get_free_heap_size());
    proto_sendf("BI,Heap min,%lu B", (unsigned long)esp_get_minimum_free_heap_size());

    const uint64_t uptime_s = (uint64_t)esp_timer_get_time() / 1000000ULL;
    proto_sendf("BI,Uptime,%llu s", (unsigned long long)uptime_s);

    float temp;
    if (read_temp_celsius(&temp)) {
        proto_sendf("BI,Temp,%d C", (int)(temp + 0.5f));
    }

    proto_sendf("BI,Reset,%s", reset_reason_str());

    proto_sendf("INFO_END");
}
