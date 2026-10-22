#pragma once

#include "wavehound_i.h"

/* Saved AP credentials (SSID + security label + passphrase), persisted to the SD
 * card. Plaintext, exactly like the saved-MAC store -- the Flipper SD is not
 * encrypted, so obfuscation would add no real protection. */

/** Load saved AP credentials into app->saved_aps. Missing file = empty list. */
void wh_ap_store_load(WavehoundApp* app);

/**
 * Store a credential for `ssid`: add it, or update the psk/security if the SSID
 * is already saved, then persist. `security` may be empty. Returns false if the
 * list is full or the SSID is empty.
 */
bool wh_ap_store_add(WavehoundApp* app, const char* ssid, const char* security, const char* psk);

/** Remove the entry at `index` and persist. */
void wh_ap_store_delete(WavehoundApp* app, size_t index);

/** Find a saved credential by exact SSID, or NULL if none. */
const WhSavedAp* wh_ap_store_find(WavehoundApp* app, const char* ssid);
