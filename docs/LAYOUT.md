# Repository layout

```
firmware/                 ESP-IDF project for the ESP32-S2 on the WiFi dev board (PlatformIO)
  main/main.c             UART command parser (HUNT/DISCOVER/SPECTRUM/INFO/...)
  main/radio.c            discover / hunt / spectrum state machine + promiscuous RX callback
  main/info.c             board version / identity / health (INFO reply)
  main/net_discover.c     ARP sweep + mDNS/SSDP/PTR/NBNS name & type discovery (IP lines),
                          elicitation ping, fingerprint scan
  main/proto.c            UART line I/O            main/proto.h  has WH_FW_VERSION
  main/led.c              RGB LED heat indicator (LEDC PWM)
  main/Kconfig.projbuild  pins, baud, LED, max channel (menuconfig -> Wavehound)
  git_version.py          PlatformIO pre-script: reconfigure when `git describe` changes (INFO Commit)
flipper_app/              Flipper external app (uFBT)
  application.fam         app manifest (appid, fap_version, assets)
  wavehound.c             entry, board power/expansion handling, board line parser
  wavehound_i.h           WavehoundApp struct, WhTarget/WhSavedMac, view & event enums
  wh_uart.c               USART worker, line splitting, keep-alive ping
  wh_store.c              saved MACs: load/add/update/delete on SD (saved.txt)
  wh_ap_store.c           saved AP passwords: load/add/delete on SD (aps.txt)
  wh_settings.c           app settings: load/save on SD (settings.txt)
  wh_oui.c                vendor lookup from SD oui.db (binary search, no RAM load)
  scenes/                 one file per screen (scene-manager pattern): start, channel select,
                          target list, details, MAC input, hunt, fingerprint, spectrum,
                          board info, name input, saved MACs, saved entry, edit saved,
                          AP login, AP password, AP WPS, AP connect, saved APs,
                          saved AP entry, settings, about
  views/wh_hunt_view.c    hunt screen + Geiger click thread
  views/wh_spectrum_view.c  spectrum screen
  assets/oui.db           bundled vendor db, committed (generated from tools/manuf.gz)
  catalog/                Flipper Apps Catalog submission: manifest.yml, changelog.md, screenshots/
tools/gen_oui.py          build oui.db from Wireshark's manuf
tools/manuf.gz            the Wireshark manuf oui.db is built from (committed, GPL-2.0+)
tools/package_release.py  collect release files (fap, board bins, merged bin, notices)
build_app.py              run ufbt (--update-db: refresh oui.db + manuf.gz first)
licenses/GPL-2.0.txt      license of manuf.gz / oui.db
THIRD_PARTY_NOTICES.md    licenses of what the release binaries and the repo's Wireshark data contain
docs/PROTOCOL.md          the UART protocol (source of truth for the wire format)
docs/DISCOVERY.md         logged-in host discovery flow (passes, listen windows, name precedence)
docs/BUILDING.md          building and flashing from source (PlatformIO, ufbt)
docs/VENDOR_DB.md         oui.db format, lookup order, refresh, license
docs/LAYOUT.md            this file
docs/SCREENSHOTS.md       screenshot gallery; docs/screenshots/ holds the censored shots
.github/workflows/release.yml  build app + firmware on every branch push (CI); on a v* tag also publish the release
.github/workflows/toolchain-check.yml  weekly: open an issue when a newer Flipper SDK or espressif32 platform is released
```
