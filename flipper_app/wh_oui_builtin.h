#pragma once

#include <stdint.h>

/*
 * Small built-in OUI -> vendor table: a zero-setup convenience covering the
 * device families this tool is most used with. Entries MUST stay sorted by OUI
 * ascending. Full coverage comes from the optional SD database (oui.db) built
 * with tools/gen_oui.py; this list is just a curated seed and can be extended
 * with any prefix you verify against the IEEE registry.
 */

typedef struct {
    uint8_t oui[3];
    const char* name;
} WhOuiEntry;

static const WhOuiEntry wh_oui_builtin[] = {
    {{0x24, 0x0a, 0xc4}, "Espressif"},
    {{0x2c, 0xcf, 0x67}, "Raspberry Pi"},
    {{0xb8, 0x27, 0xeb}, "Raspberry Pi"},
    {{0xdc, 0xa6, 0x32}, "Raspberry Pi"},
    {{0xe4, 0x5f, 0x01}, "Raspberry Pi"},
};
