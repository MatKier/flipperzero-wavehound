# Wavehound

**WiFi fox hunting for the Flipper Zero WiFi dev board.**

Wavehound uses the Flipper and its ESP32-S2 WiFi dev board to locate a 2.4 GHz
WiFi transmitter by its signal strength. Pick an access point or other device
from a live scan, or type in a MAC, then walk around: the Flipper clicks faster,
the meter fills and the board's RGB LED shifts from blue to red as the signal
gets stronger.

<p align="center">
  <img src="flipper_app/catalog/screenshots/main_screen.png" alt="Wavehound main menu on the Flipper Zero" width="400">
  <img src="flipper_app/catalog/screenshots/hunting_device.png" alt="Wavehound hunt screen showing signal strength" width="400">
</p>

Typical uses:

- Find that forgotten access point, repeater or IoT plug hidden in a ceiling or closet.
- Track down a rogue AP on a network you administer.
- Locate your own lost phone, smart plug or camera by its MAC.
- See which 2.4 GHz channel is least congested before you configure your router.

Wavehound is **receive-only by default**: it reads frame headers to measure
signal strength and never transmits. The one exception is the opt-in login for
[hunting silent devices](#hunting-silent-devices-optional).

**Why its own board firmware?** [ESP32 Marauder](https://github.com/justcallmekoko/ESP32Marauder)
can be driven over UART too, but its fox-hunt channel logic (park on the
target's channel, sweep to find it, re-acquire it when it goes quiet) lives in
its TFT code, which the screenless dev-board build compiles out. What is left
reports RSSI only on the channel the hunt started on, and its station scan hops
every channel with no way to pin one. Wavehound's firmware does that channel
work on the board, plus a strict client filter, which is what makes close-in
hunting of an occasional transmitter practical.

```
┌──────────── Flipper Zero ─────────────┐        ┌──── WiFi dev board (ESP32-S2) ─────┐
│  Wavehound.fap                        │  UART  │  Wavehound firmware                │
│   ├ Targets: AP search, device search,│ 115200 │   ├ live scan, AP/STA filter       │
│   │  manual MAC entry                 │◄──────►│   │  → target list                 │
│   ├ Hunt view: dBm, meter, graph,     │  pins  │   ├ promiscuous RX → RSSI of target│
│   │  Geiger clicks (speaker)          │ 13/14  │   ├ channel sweep → activity       │
│   ├ Spectrum view: ch 1-13 bars       │        │   └ RGB LED heat indicator         │
│   └ 5V OTG on, expansion svc off      │        │                                    │
└───────────────────────────────────────┘        └────────────────────────────────────┘
```

## Features

| Menu | What it does |
|------|--------------|
| **Hunt access point** | Lists access points as they are heard on any channel (by SSID; hidden networks by BSSID). |
| **Hunt devices** | Lists everything that is not an access point: phones, laptops, smart plugs, cameras. Pick the scope first: **All channels (hop)**, one **Channel 1–13**, or **Clients of nearby AP…**, which scans for networks and then lists only the stations associated with the one you pick, on its channel. A fixed channel carries over to the hunt. |
| **Enter MAC** | Type a MAC on the hex keypad (pre-filled with the last one), then pick **Auto (hop until heard)**, a channel, or **Channel of nearby AP…**. A chosen channel is pinned: the board stays there even while the target is quiet. Clients always use their AP's channel, so the AP option is the quick way to hunt a known client. |
| **Saved MACs** | Favorites saved from the hunt screen. **Hunt** opens the channel picker with the saved channel pre-selected, **Edit** fixes the name, MAC or channel, **Delete** removes it. |
| **Saved APs** | Wi-Fi passwords saved after a [login](#hunting-silent-devices-optional): **Edit password** or **Delete**. |
| **Channel spectrum** | Sweeps channels 1–13 (120 ms each) and draws a bar per channel with peak hold, a sweep cursor, the busiest channel and the number of distinct transmitters per sweep. OK/Left/Right switch the metric between packets, max dBm and bytes. |
| **Board info** | Firmware version, commit and build, ESP-IDF version, chip, radio features, factory MAC, association and IP, flash and app-partition use, heap, uptime, chip temperature and last reset reason (handy for spotting brownouts from the 5 V boost). |
| **Settings** | See [Settings](#settings). |

**Scan lists** fill live and keep scanning while you browse; new entries append
at the bottom so your highlight stays put, and there is no duration to choose.
`*` marks a randomized (privacy) MAC, which may change over time. The first
entry, **~ Search again**, clears the list and restarts the scan; Back stops
scanning.

**Device details** open when you pick a scan result: name, MAC, **vendor**
(from the MAC's OUI, see [Vendor database](#vendor-database); randomized MACs
show `randomized`), AP or station, channel and signal. For access points it also
shows the security (Open/WEP/WPA/WPA2/WPA3) and 802.11 mode (g/n/ac/ax), e.g.
`AP ch6 WPA2 n -52dBm`. **Hunt** starts the hunt, **Save** stores a favorite,
and **Scan** [fingerprints](#fingerprinting-a-device) the device when you are
logged in.

### Hunt screen

The board parks on the target's channel and reports the signal strength of
every frame that MAC sends (beacons, data, probes) every 100 ms. The screen
shows:

- a big dBm readout with the frame rate (`/s`), a COLD/COOL/WARM/HOT! label and
  a trend arrow for whether the signal is rising or falling as you move;
- a meter with peak (`pk`) and noise-floor markers, the noise floor value
  (`nf`) and a ~12-second signal graph;
- `?ch` in the corner while the board is still searching channels. Once a heard
  target goes quiet, `quiet Ns` counts the silence, and after **Re-search**
  (default 8 s) the board hops again to re-find it;
- when logged in to the target's AP and its IP is known, the device's hostname,
  type (e.g. Chromecast, Printer) or IP in the header.

Geiger clicks play through the speaker, faster as the signal gets stronger, with
an optional haptic buzz.

Controls: **OK** mute/unmute, **Left** reset peak, **Right** save as a named
favorite, **Down** toggle the [elicitation ping](#hunting-silent-devices-optional),
**Up** [fingerprint](#fingerprinting-a-device) the target (Down and Up only when
logged in and the IP is known), **Back** return to the list.

Hold the Flipper away from your body and turn slowly: your body blocks the
signal, so the strongest reading points toward the source. Phones and other
clients are easiest to hunt while in use; an idle phone may only transmit every
few seconds, so the readout updates in bursts. Phones also rotate randomized
MACs, so saving a `*` MAC is of little use.

### Settings

| Setting | Values (default) | Effect |
|---------|------------------|--------|
| **Click volume** | 0.1–1.0 (0.5) | Geiger click loudness. |
| **Sound** | On / Muted (On) | The hunt screen's starting mute state. |
| **Saved hunt ch** | Saved / Auto-hop / Nearby AP (Saved) | Which channel option is pre-selected when you hunt a saved MAC. |
| **Hop dwell** | 100 ms–2 s (300 ms) | How long a hunt stays on each channel while searching. Shorter finds an AP faster, longer catches quiet stations. |
| **Re-search** | 4–60 s (8 s) | How long a locked hunt tolerates silence before searching again. |
| **Range** | Near / Normal / Far (Normal) | The dBm window the meter spans (−75..−25, −95..−30, −100..−50), which also sets the click and haptic rate. |
| **Response** | Fast / Normal / Smooth (Normal) | How quickly the meter follows the signal. |
| **Scan dwell** | 120 ms–2 s (120 ms) | How long a scan stays on each channel. |
| **Haptic** | Off / On (Off) | Buzz as a proximity cue, faster as the signal strengthens (works even when muted). |
| **Keep backlight** | Off / On (On) | Hold the backlight on while a hunt is on screen. |
| **Ping interval** | 50 ms–1 s (250 ms) | How often the [elicitation ping](#hunting-silent-devices-optional) pokes a silent target. |

**Reset to defaults** at the bottom restores everything.

### Files on the SD card

Everything lives in `/ext/apps_data/wavehound/`: `saved.txt` (saved MACs),
`aps.txt` (saved AP passwords), `settings.txt`, and optionally your own
`oui.db` (see [Vendor database](#vendor-database)). The SD card is not
encrypted and saved passwords are stored in **plaintext**, so treat the card
accordingly.

## Hunting silent devices (optional)

A device that rarely transmits gives almost no RSSI samples, so it is hard to
locate. If it is a client of an access point **you are authorized to use**,
Wavehound can log the board in to that network and prompt the target to reply.

From **Hunt devices → Clients of nearby AP…**, pick the network and you are
offered a login before its client list is shown:

- **Connect (saved)**: a password saved earlier (shown only if one exists).
- **Enter password**: type the Wi-Fi password. The stock Flipper keyboard is
  alphanumeric plus `_`, so passwords with other symbols need WPS or a saved
  password.
- **WPS (push button)**: press the WPS button on the router; the board enrols
  and captures the credentials.
- **Connect (open)**: offered instead of the three above for an open network.
- **Skip (list only)**: list the clients without logging in.

After a successful join you can save the password to **Saved APs**. The client
list, details and hunt screen then show each device's **IP** and, when it can be
resolved, a **hostname** and device **type** (best-effort: mDNS/Bonjour and
UPnP/SSDP first, which is how a TV gets a name like "Living Room TV" and a type
like *Chromecast*, then reverse DNS and NBNS). The list and hunt header show the
type only when there is no name; the details screen shows both.

Discovery stays radio-first: the client list is built only from devices heard
over the air, keyed by MAC. Logging in adds a background sweep (ARP for each
host's IP, then the name lookups) that only *annotates* those rows. A host known
only from the router's relayed reply, such as a wired device, is never added; a
client still has to transmit to appear and to be hunted. The sweep's passes,
timing and name precedence are in [docs/DISCOVERY.md](docs/DISCOVERY.md).

On the hunt screen, **Down** toggles the **elicitation ping**: a unicast ARP
request plus an ICMP echo to the one target you are hunting, at the **Ping
interval** (capped to 50 ms–1 s), so it answers with a steady stream of frames
to measure. It only ever addresses that single MAC. It is **not** a flood or
denial-of-service tool and is built so it cannot be turned into one.

### Fingerprinting a device

When logged in and a device's IP is known, **Scan** on its details screen (or
**Up** while hunting it) makes the board guess what the device is: it tries a
fixed handful of TCP ports (SSH, Telnet, HTTP, HTTPS, SMB, RTSP, HTTP-alt,
JetDirect printer), reads the web page's `Server:` header and title, and shows
the open ports, the web server and a label such as *Network printer*, *RTSP
camera* or *Web service*. HTTPS is noted but not inspected (the board speaks no
TLS). Like the ping it is **active**: one connection attempt per port, once per
press, to the single device you are inspecting. Leaving the result screen
cancels a scan still in progress.

> **Authorized use only.** Logging in, the elicitation ping and fingerprinting
> transmit and join a network, unlike the rest of Wavehound. Only use them on a
> network you own or are explicitly authorized to use, against a device you are
> authorized to locate.

## Board firmware versions

Prebuilt releases ship the matching board firmware. On older board firmware the
app keeps working, but some features need at least:

| Feature | Board firmware |
|---------|----------------|
| Noise floor on the hunt meter | v6 |
| Security and 802.11 mode on the details screen | v7 |
| Hop dwell, Re-search and Scan dwell (older firmware keeps 300 ms / 8 s / 120 ms) | v8 |
| AP login (WPS: v10) | v9 |
| Elicitation ping | v12 |
| Device type from mDNS/SSDP | v16 |
| Fingerprinting | v17 |

The app hides the login below v9 and **Scan** below v17. **Board info** shows
the running version.

## Vendor database

The details screen resolves a MAC's vendor from its OUI (the first 3 bytes)
using `oui.db`, a sorted fixed-width binary file (a small header, then 16-byte
records: 3-byte OUI + name) that the app binary-searches on the SD card, so it
never loads into RAM.

<p align="center">
  <img src="flipper_app/catalog/screenshots/device_detail.png" alt="Wavehound device details screen showing the resolved vendor" width="400">
</p>

`flipper_app/assets/oui.db` is committed together with the Wireshark `manuf` it
is built from (`tools/manuf.gz`), so every build bundles it, including the
Flipper Apps Catalog build. The app looks for it in two places, in order:

1. `/ext/apps_data/wavehound/oui.db`: your own drop-in, e.g. newer data
   (`python3 tools/gen_oui.py --download -o oui.db` makes one; older header-less
   files with 32-byte records still work).
2. `/ext/apps_assets/wavehound/oui.db`: the bundled copy, unpacked when the app
   is installed.

Without a database, non-randomized MACs show "Unknown". To refresh the bundled
copy, run `python3 build_app.py --update-db` and commit `tools/manuf.gz` and
`oui.db` together; CI fails if they don't match byte for byte. Both are
GPL-2.0-or-later, see [License](#license).

## Install

### Prebuilt releases

Each [GitHub release](https://github.com/MatKier/flipperzero-wavehound/releases)
ships `wavehound.fap` (vendor database bundled) and the board firmware:
`wavehound-esp32s2-fw<N>-full.bin` to flash at `0x0`, plus the separate
bootloader / partition table / app images. `FLASHING.txt` in the release has
the esptool commands. Copy the `.fap` as in step 2 below.

### 1. Flash the dev board firmware

The firmware is built with **PlatformIO**, which pulls its own ESP-IDF
toolchain (pinned to ESP-IDF 6.1), so you don't need a separate ESP-IDF install.
Install PlatformIO Core once with [pipx](https://pipx.pypa.io/) (e.g.
`apt install pipx` or `brew install pipx`, then `pipx ensurepath`):

```bash
pipx install platformio      # later: pipx upgrade platformio
pio --version                # check it runs
```

Plain `pip install` into the system Python is refused on most current distros
(`externally-managed-environment`); `pip install platformio` inside a venv works
too.

> If `pio` fails with `No module named 'requirements'`, an unrelated PyPI
> package named `pio` is shadowing PlatformIO's command. Remove it and restore
> PlatformIO's launcher: `pipx uninstall pio && pipx reinstall platformio`
> (in a venv: `pip uninstall -y pio && pip install --force-reinstall --no-deps platformio`).

Put the board in **download mode**: hold **BOOT**, tap **RESET**, release
**BOOT**, then connect the board's own USB-C port to your PC (directly, not
through the Flipper). Find its port with `pio device list`: it shows up as an
Espressif / USB serial device (e.g. `COM5` on Windows, `/dev/ttyACM0` or
`/dev/ttyUSB0` on Linux, `/dev/cu.usbmodem*` on macOS). The **Flipper is a
serial port too**, so be sure to pick the ESP32-S2.

```bash
cd firmware
pio run                                  # build only
pio run -t upload --upload-port COM5     # build and flash (use your port)
pio run -t menuconfig                    # optional: pins, baud, LED, max channel
```

Without `--upload-port` PlatformIO auto-detects, but naming the port makes sure
you never write to the Flipper. If the upload can't enter the bootloader by
itself, redo BOOT+RESET right before uploading. Tap **RESET** after flashing to
boot the new firmware.

> The defaults assume the official board: UART0 on GPIO43/44 to Flipper pins
> 13/14, an active-low RGB LED on GPIO6/5/4 (R/G/B) and channels up to 13 (set
> the max channel to 11 for US-style regions). If your clone differs, change
> them in menuconfig.

### 2. Build and install the Flipper app

From the repo root, `build_app.py` runs `ufbt` with the committed vendor
database bundled:

```bash
pipx install ufbt                  # or: pip install ufbt inside a venv
python3 build_app.py               # build -> flipper_app/dist/wavehound.fap
python3 build_app.py --launch      # build, upload over USB and run
python3 build_app.py --update-db   # download the latest manuf, regenerate oui.db, then build
python3 build_app.py --manuf path/to/manuf.gz  # regenerate oui.db from a local manuf.gz
python3 build_app.py --update-db --no-build    # only refresh oui.db
```

Running `ufbt` in `flipper_app/` directly also works and bundles the same
`oui.db`. Or copy `wavehound.fap` to `SD:/apps/GPIO/` with qFlipper, the same
folder a Flipper Apps Catalog install uses; the app shows up under
**Apps → GPIO → Wavehound**. Built against official firmware SDK 1.5.1
(API 88.2).

### 3. Use it

1. Plug the dev board onto the Flipper and open Wavehound. The app turns on
   5 V on the GPIO header and takes over the USART from the expansion service.
2. The menu header changes to **"Wavehound - board OK"** once the board answers.
3. Pick a target from **Hunt access point**, **Hunt devices** or **Enter MAC**
   and start walking (see [Hunt screen](#hunt-screen)).

## Repository layout

`firmware/` is the board firmware, `flipper_app/` the Flipper app, `tools/` the
vendor-database and release scripts. The file-by-file map is in
[docs/LAYOUT.md](docs/LAYOUT.md).

## Protocol

The link is a plain-text, line-based protocol, so you can test the board from
any serial terminal. See [docs/PROTOCOL.md](docs/PROTOCOL.md). Example:

```
> HUNT,a4:2b:b0:11:22:33,6
< OK,HUNT
< H,6,9,-47,-48,-45,1,-95       # ch, frames, last/avg/max dBm, locked, noise floor
> DISCOVER,AP                   # scan continuously; AP filter, hop all channels
< OK,DISCOVER
< DISCOVER_BEGIN,AP,0
< DEV,a4:2b:b0:11:22:33,6,-41,93,A2n,HomeNet   # each AP as it is heard; flags: AP, WPA2, 802.11n
< ...                           # keeps streaming until you STOP
> STOP
< OK,STOP
```

## Disclaimer

This is a hobby project, provided as-is and without any warranty. Everything
here is developed and tested on my own original hardware: a genuine Flipper
Zero and the official WiFi dev board. I can't vouch for clones, other hardware
or other setups: flashing the dev board and running this firmware is at your
own risk, and I don't guarantee it won't damage, brick or otherwise break your
hardware. You are responsible for anything that happens to your Flipper, dev
board or other equipment.

This code is AI-assisted: much of it was written with the help of AI tools.
Review it yourself before trusting it or flashing it to your hardware.

Only locate networks and devices you own or are explicitly authorized to
locate. Wavehound is receive-only except for the optional login, elicitation
ping and fingerprint described above. Passive reception is legal in most places,
but radio and privacy rules vary by jurisdiction. Checking what you are allowed
to do, and getting permission first, is up to you.

## License

MIT (see [LICENSE](LICENSE)), except for the vendor data: `tools/manuf.gz`
(Wireshark's `manuf`, derived from the IEEE registry) and the
`flipper_app/assets/oui.db` built from it are GPL-2.0-or-later. The release
binaries also contain third-party code and data (ESP-IDF and its components,
that vendor data); see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
