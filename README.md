# Wavehound

**WiFi fox hunting for the Flipper Zero WiFi dev board.**

Wavehound uses the Flipper and its ESP32-S2 WiFi dev board to locate a 2.4 GHz
WiFi transmitter by its signal strength. Pick an access point or other device
heard during a short listen, or type in a MAC by hand, then walk around: the
Flipper clicks faster, the meter fills and the board's RGB LED shifts from blue
to red as the signal gets stronger.

Typical uses:

- Find that forgotten access point, repeater or IoT plug hidden in a ceiling or closet.
- Track down a rogue AP on a network you administer.
- Locate your own lost phone, smart plug or camera by its MAC.
- See which 2.4 GHz channel is least congested before you configure your router.

Wavehound is **receive-only**: it reads frame headers to measure signal strength
and never transmits. It isn't built on Marauder because the hunt — tracking the
per-frame RSSI of one specific target in promiscuous mode, with channel parking
and hopping — has to run on the ESP32 in real time. That loop can't live in the
Flipper app (the `.fap`), which is only a thin UART client that renders what the
board reports, so the hunting logic runs in the board's own firmware. Marauder is
its own board firmware with a fixed command set that doesn't expose this, which
is why Wavehound ships custom firmware rather than building on it.

```
┌──────────── Flipper Zero ─────────────┐        ┌──── WiFi dev board (ESP32-S2) ─────┐
│  Wavehound.fap                        │  UART  │  Wavehound firmware                │
│   ├ Targets: AP search, device search,│ 115200 │   ├ timed listen, AP/STA filter    │
│   │  manual MAC entry                 │◄──────►│   │  → target list                 │
│   ├ Hunt view: dBm, meter, graph,     │  pins  │   ├ promiscuous RX → RSSI of target│
│   │  Geiger clicks (speaker)          │ 13/14  │   ├ channel sweep → activity       │
│   ├ Spectrum view: ch 1-13 bars       │        │   └ RGB LED heat indicator         │
│   └ 5V OTG on, expansion svc off      │        │                                    │
└───────────────────────────────────────┘        └────────────────────────────────────┘
```

## Features

| Mode | What you see |
|------|--------------|
| **Hunt access point** | Pick how long to listen: 5, 10 or 30 seconds, or 1, 2 or 5 minutes. Press **OK** during the listen to stop early and jump straight to the results. The board listens on every channel and lists only access points, strongest first, by SSID (hidden networks show their BSSID). Pick one to open its details (vendor, type, signal), then hunt. |
| **Search devices** | First pick the channel: **All channels (hop)**, **Channel 1–13**, or **Channel of nearby AP…**, which runs an AP listen so you can pick a network and borrow its channel. Then the same timed listen (5 s to 5 min, OK to stop early), with access points left out. You get the other transmitters: phones, laptops, smart plugs, cameras. `*` marks a randomized (privacy) MAC that may change over time. Pick one to open its details (vendor, type, signal), then hunt. If you fixed the channel, the hunt stays on it. |
| **Enter MAC** | Type a MAC on the hex keypad (pre-filled with your last target), then pick the channel the same way: **Auto (hop until heard)**, a channel number, or a nearby AP's channel. A chosen channel is pinned: the board stays there even while the target is quiet. Clients always use their AP's channel, so the AP option is the quick way to hunt a known client. |
| **Device details** | After picking a scan result you get a details screen: SSID/name, MAC, **vendor** (resolved from the MAC's OUI), AP or station, channel and signal. **Hunt** continues to the hunt; **Save** stores it as a favorite. Randomized MACs show `randomized` instead of a vendor. Vendor resolution needs the `oui.db` database; without it, other MACs show "Unknown" (see [Vendor database](#vendor-database)). |
| **Hunt screen** | The board parks on the target's channel and reports the signal strength of every frame that MAC sends (beacons, data, probes) every 100 ms. The screen shows a big dBm readout, a COLD/COOL/WARM/HOT! label, a meter with peak marker and a 12-second signal graph. Geiger clicks play through the speaker, faster as the signal gets stronger. `?ch` in the corner means the board is still searching channels. After 8 s of silence it hops again to re-find the target. Press **Right** to save the current target as a named favorite. |
| **Saved MACs** | Favorites you saved from the hunt screen, kept on the SD card (`/ext/apps_data/wavehound/saved.txt`). Pick one for **Hunt / Edit / Delete**. Hunt opens the channel picker with the saved channel pre-selected, so you can accept it, choose another, or switch to Auto. Edit fixes the name, MAC or channel (each field pre-filled). Delete removes it. Saving a randomized (`*`) MAC is of little use since it rotates. |
| **Channel spectrum** | The board sweeps channels 1–13 (120 ms dwell each) in promiscuous mode. The Flipper draws one bar per channel with peak hold, a sweep cursor, the busiest channel, and the number of distinct transmitters heard per sweep. You can switch the metric between packets, max dBm and bytes. |
| **Board info** | Ask the board about itself: firmware build (date + ELF hash), the ESP-IDF version actually running, chip model/revision/cores, radio features, factory MAC, flash size, the app partition and how much of it the current firmware uses, free/min heap, uptime, chip temperature and the last reset reason (handy for spotting brownouts from the 5 V boost). |

Controls:

- **Hunt:** OK = mute/unmute, Left = reset peak, Right = save target, Back = return to the list
- **Searching:** OK = stop now and show the results, Back = cancel
- **Target list:** the first entry, "~ Search again", reruns the same listen
- **Spectrum:** OK/Left/Right = change metric, Back = return to menu

## Layout

```
firmware/     ESP-IDF project for the ESP32-S2 on the WiFi dev board
  main/main.c     command parser
  main/radio.c    discover / hunt / spectrum state machine + promiscuous callback
  main/info.c     board version / identity / health report (INFO)
  main/proto.c    UART line I/O
  main/led.c      RGB LED (LEDC PWM)
flipper_app/  Flipper external app (uFBT)
  wavehound.c             entry, board power/expansion handling, board line parser
  wh_uart.c               USART worker, line splitting, keep-alive ping
  scenes/                 one file per screen (start, listen time, searching,
                          target list, details, MAC input, hunt, spectrum,
                          board info, name input, saved MACs, saved entry,
                          edit saved, about)
  wh_store.c              saved MACs: load/add/update/delete on the SD card
  wh_oui.c                vendor lookup from the SD oui.db
  views/wh_hunt_view.c    hunt screen + Geiger click thread
  views/wh_progress_view.c search progress screen
  views/wh_spectrum_view.c spectrum screen
  assets/                 bundled file assets (oui.db, generated by build.py)
tools/gen_oui.py  build oui.db (vendor database) from Wireshark's manuf
build.py          generate+bundle oui.db, then build the app (ufbt)
docs/PROTOCOL.md  the UART protocol
```

## Vendor database

The device-details screen shows the manufacturer behind a MAC, resolved from
its OUI (the first 3 bytes). Vendor names come from `oui.db`, a sorted
fixed-width binary (3-byte OUI + name) the app binary-searches by seeking, so
it stays fast and never loads into RAM.

Build the app with the database bundled in:

```
python3 build.py                        # download manuf, build oui.db, run ufbt
python3 build.py --manuf path/to/manuf  # use a local manuf file instead
python3 build.py --no-build             # only (re)generate the db
python3 build.py --launch               # build, then upload + run on the Flipper
```

`build.py` writes the db to `flipper_app/assets/oui.db`, which the manifest
bundles (`fap_file_assets`) so the Flipper unpacks it to
`/ext/apps_assets/wavehound/oui.db` on install. (`tools/gen_oui.py` is the
underlying generator if you want to run it directly.)

The app looks for the database in two places, in order:

1. `/ext/apps_data/wavehound/oui.db` — your own drop-in (overrides, newer data)
2. `/ext/apps_assets/wavehound/oui.db` — the bundled copy

So it works out of the box, and you can refresh the data without rebuilding by
dropping a newer `oui.db` in `apps_data`. Without any database, non-randomized
MACs show "Unknown"; randomized (`*`) MACs show
`randomized`. The data comes from Wireshark's `manuf` (derived from the IEEE
registry); redistribute per its terms with attribution.

## 1. Flash the dev board firmware

You can build with **PlatformIO** (simplest: one global install that pulls its
own ESP-IDF toolchain) or with a full **ESP-IDF** install. PlatformIO is shown
first and recommended; verified building with PlatformIO + ESP-IDF 6.1.

### Option A: PlatformIO (recommended)

Install PlatformIO Core once, globally (needs Python 3 with `pip`):

```bash
pip install -U platformio
```

Check it runs:

```bash
pio --version
```

> If `pio` fails with `No module named 'requirements'`, an unrelated PyPI
> package named `pio` is shadowing PlatformIO's command. Remove it and restore
> PlatformIO's launcher:
> `pip uninstall -y pio && pip install --force-reinstall --no-deps platformio`.
> In any case, `python -m platformio <args>` always calls the real PlatformIO.

Put the board in **download mode**: hold **BOOT**, tap **RESET**, release
**BOOT**, then connect the board to your PC with its own USB-C port (directly,
not through the Flipper).

Find the serial port:

```bash
pio device list
```

The dev board appears as an Espressif / USB serial device (e.g. `COM5` on
Windows, `/dev/ttyACM0` or `/dev/ttyUSB0` on Linux, `/dev/cu.usbmodem*` on
macOS). Note it. The **Flipper also shows up as a serial port**, so be sure to
pick the ESP32-S2, not the Flipper.

Build, then flash (replace `COM5` with your port):

```bash
cd firmware
pio run                                  # build only
pio run -t upload --upload-port COM5     # build and flash
```

Omitting `--upload-port` lets PlatformIO auto-detect, but naming it explicitly
makes sure you never write to the Flipper's port. If the upload can't enter the
bootloader by itself, redo the BOOT+RESET step right before uploading.

Change pins, baud, LED or max channel (e.g. 11 for US-style regions):

```bash
cd firmware
pio run -t menuconfig
```

### Option B: ESP-IDF

With a full ESP-IDF 5.x/6.x install, from its `idf.py`/export shell and after
the same BOOT+RESET download-mode step:

```bash
cd firmware
idf.py set-target esp32s2
idf.py build
idf.py -p COM5 flash        # use your port
idf.py menuconfig           # pins, baud, LED, max channel -> Wavehound
```

---

Tap **RESET** after flashing to boot the new firmware.

> The defaults assume the official board: UART0 on GPIO43/44 to Flipper
> pins 13/14, and an active-low RGB LED on GPIO6/5/4 (R/G/B). If your clone
> differs, change them in menuconfig.

## 2. Build and install the Flipper app

```bash
pip install ufbt
cd flipper_app
ufbt            # build -> dist/wavehound.fap
ufbt launch     # build, upload over USB and run
```

Or copy `dist/wavehound.fap` to `SD:/apps/GPIO/ESP/` with qFlipper. The app shows up
under **Apps → GPIO → ESP → Wavehound**. Built against official firmware SDK 1.5.1
(API 88.2).

## 3. Use it

1. Plug the dev board onto the Flipper and open Wavehound. The app turns on
   5 V on the GPIO header and takes over the USART from the expansion service.
2. The menu header changes to **"Wavehound - board OK"** once the board answers.
3. Pick a target: **Hunt access point**, **Search devices** or **Enter MAC**.
   Then start walking. Hold the Flipper away from your body and turn slowly.
   Your body blocks the signal, so the strongest reading points toward the
   source.

Hunting phones and other client devices works best while they are in use. An
idle phone may only transmit every few seconds, so the readout updates in
bursts. Phones also rotate randomized MACs (`*`), so a MAC you found yesterday
may not exist today.

## Protocol

The link is a plain-text, line-based protocol, so you can test the board from
any serial terminal. See [docs/PROTOCOL.md](docs/PROTOCOL.md). Example:

```
> HUNT,a4:2b:b0:11:22:33,6
< OK,HUNT
< H,6,9,-47,-48,-45,1
> DISCOVER,10,AP
< OK,DISCOVER
< DISCOVER_BEGIN,10,AP
< DP,1,4
< ...
< DEV,a4:2b:b0:11:22:33,6,-41,93,A,HomeNet
< DISCOVER_END,1,1
> DISCOVER,10,STA,6
< ...
< DEV,3a:4f:12:ab:cd:ef,6,-58,12,SR,
< DISCOVER_END,1,1
```

## Disclaimer

This is a hobby project, provided as-is and without any warranty. Flashing the
dev board and running this firmware is at your own risk; I don't guarantee it
won't damage, brick or otherwise break your hardware. You are responsible for
anything that happens to your Flipper, dev board or other equipment.

This code is AI-assisted: much of it was written with the help of AI tools.
Review it yourself before trusting it or flashing it to your hardware.

Only locate networks and devices you own or are explicitly authorized to locate,
and only on your own network. Wavehound is receive-only, and passive reception is
legal in most places, but radio and privacy rules vary by jurisdiction. Checking
what you are allowed to do, and getting permission first, is up to you.

## License

MIT. See [LICENSE](LICENSE).
