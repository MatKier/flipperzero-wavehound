# Third-party notices

Wavehound's own code is MIT licensed (see [LICENSE](LICENSE)). This
repository also contains third-party data (the MAC vendor database, see
[below](#in-this-repository)), and the release binaries contain third-party
code and data, listed below. The full license texts, taken from the exact
package versions used for the build, are in `licenses/` inside the
`wavehound-licenses.zip` release asset. The released copy of this file ends
with the build details (versions used).

## In this repository

Two files in this repository are not Wavehound's own work and are **not** under
its MIT license:

| File | What it is | License |
|---|---|---|
| `tools/manuf.gz` | Wireshark's manufacturer database (`manuf`), unmodified, as downloaded from <https://www.wireshark.org/download/automated/data/manuf.gz> | GPL-2.0-or-later |
| `flipper_app/assets/oui.db` | Derived from `tools/manuf.gz` by `tools/gen_oui.py`: its 24-bit MAC prefixes and shortened vendor names, re-encoded as fixed-width binary records | GPL-2.0-or-later (as a derivative of `manuf`) |

The GPL-2.0 text is in [`licenses/GPL-2.0.txt`](licenses/GPL-2.0.txt).
`tools/manuf.gz` is the source `oui.db` was built from; running
`python3 tools/gen_oui.py tools/manuf.gz` reproduces it byte for byte, and CI
checks that on every build. Both files are refreshed together.

`oui.db` is bundled into every `wavehound.fap`: the GitHub releases and the
Flipper Apps Catalog build, which is built from this repository as-is.

## Flipper app (`wavehound.fap`)

### OUI vendor database (`oui.db`)

The bundled `oui.db` is the committed `flipper_app/assets/oui.db`, generated
by `tools/gen_oui.py` from Wireshark's manufacturer database (`manuf`, committed
as `tools/manuf.gz`; see [In this repository](#in-this-repository)). It contains
the 24-bit MAC prefixes and Wireshark's shortened vendor names.

- Wireshark is © Gerald Combs and contributors, licensed under
  **GPL-2.0-or-later** (`licenses/GPL-2.0.txt`). The `manuf` data, and
  `oui.db` derived from it, are treated as covered by that license.
- The underlying prefix assignments come from the IEEE Registration
  Authority's public registry.
- The exact `manuf` used for each release is `tools/manuf.gz` at the release's
  commit, and is also attached to the release as `manuf.gz`.

`oui.db` is a separate data file that the app reads at runtime. It does not
change the license of the app's code.

### Flipper Zero firmware

The app is built with the Flipper Zero firmware SDK and runs against the
firmware's API (Flipper Zero firmware, GPL-3.0).

## Board firmware (`wavehound-esp32s2-fw*.bin`)

The ESP32-S2 images (bootloader, partition table, app) are built with ESP-IDF
via PlatformIO and include:

| Component | License | License text in `licenses/` |
|---|---|---|
| ESP-IDF (bootloader, drivers, HAL, system), © Espressif Systems | Apache-2.0 (a few files MIT) | `ESP-IDF-LICENSE.txt` |
| Espressif Wi-Fi, PHY and coexistence libraries | Apache-2.0 | `esp_wifi-lib-LICENSE.txt`, `esp_phy-lib-LICENSE.txt`, `esp_coex-lib-LICENSE.txt` |
| FreeRTOS kernel, © Amazon.com, Inc. or its affiliates | MIT | `FreeRTOS-Kernel-LICENSE.md` |
| lwIP, © Swedish Institute of Computer Science | BSD-3-Clause | `lwIP-COPYING.txt` |
| wpa_supplicant, © Jouni Malinen and contributors | BSD-3-Clause | `wpa_supplicant-COPYING.txt`, `wpa_supplicant-README.txt` |
| Mbed TLS / TF-PSA-Crypto, © The Mbed TLS Contributors | Apache-2.0 (dual Apache-2.0 OR GPL-2.0-or-later; used under Apache-2.0) | `MbedTLS-LICENSE.txt`, `TF-PSA-Crypto-LICENSE.txt` |
| picolibc / newlib C library | BSD-style, various | `picolibc-COPYING.picolibc.txt`, `picolibc-COPYING.NEWLIB.txt`, `esp_libc-COPYING.*.txt` |
| GCC runtime (libgcc, libstdc++) | GPL-3.0 with the GCC Runtime Library Exception | `GCC-COPYING.RUNTIME.txt`, `GCC-COPYING3.txt` |
