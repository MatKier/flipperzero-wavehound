#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * Resolve a MAC's vendor from its OUI into `out`:
 *   - "randomized" for a locally-administered (privacy) MAC,
 *   - the vendor name from the built-in table or the SD database (oui.db),
 *   - "Unknown" if nothing matches.
 * Safe to call with no database present. Does a few SD seeks, so call it once
 * per screen, not in a hot loop.
 */
void wh_oui_lookup(const uint8_t mac[6], char* out, size_t out_size);
