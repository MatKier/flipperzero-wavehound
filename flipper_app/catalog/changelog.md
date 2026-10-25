## v1.18

- **New searches no longer stay stuck on a joined network's channel.** After
  logging in to an access point to hunt one of its clients, going back to the
  device-search channel list and starting a new search ("All channels",
  "Clients of nearby AP..." or a single channel) now leaves that network first,
  so the scan hops/parks freely again. Backing out to the main menu still keeps
  the connection, so Board info shows it.

## v1.17

- **Fingerprint a target.** When you are logged in to a network and a device's IP
  is known, you can now ask the board what the device *is*: a quick check of a
  few common ports plus a peek at its web page labels it as, say, a *Network
  printer*, *RTSP camera* or *Web service*, and lists the open ports and the web
  server it runs. Press the center button ("Scan") on a device's details screen,
  or **Up** while hunting it. This is an **active** probe — it briefly connects to
  the device — so it is offered only on the same authorized-login path as the
  elicitation ping, and only against the single device you are inspecting. Needs
  board firmware v17 or newer; older boards simply don't offer it.

## v1.16

- **Friendlier device names and types.** While joined to a network, the board now
  also asks the network's devices who they are (mDNS/Bonjour and UPnP/SSDP — the
  same announcements phones, TVs, speakers and printers answer). Devices that
  used to show up as a bare IP can now appear as "Living Room TV", with a type
  such as *Chromecast*, *Printer* or *AirPlay* (shown in the details screen, and
  in the client list and hunt header for devices that have no name). Needs board firmware v16 or newer; older boards keep working and
  simply show no type. It sends a few multicast queries during the existing
  network scan, nothing new to configure.

## v1.15

- Fixed a bug where logging in to an access point by **WPS** could save a wrong
  Wi-Fi password: a long WPS credential line from the board overran the app's
  receive buffer and was truncated, so the stored password came out incomplete
  and later "Connect (saved)" would fail. Longer passwords now arrive intact.

## v1.14

- **Hunt silent devices by logging in to their access point.** From *Hunt
  devices > Clients of nearby AP*, you can now log the board in to the chosen
  network (saved password, typed password, WPS push-button or open), then hunt a
  client that rarely transmits.
- On the hunt screen, **Down** toggles an **elicitation ping**: a rate-limited
  unicast request to the single target you are hunting, just enough that it
  answers and you get a steady signal to measure. It is capped and aimed at one
  MAC — not a flood.
- The client list and hunt screen now show each device's **IP** and, when it can
  be resolved, a **hostname**. Discovery now probes the network a few times so
  devices that were briefly asleep still turn up, retries name lookups that
  first came back empty, and keeps trying to resolve a hunt target's IP while
  you hunt it, so elicitation becomes available once it answers.
- New **Saved APs** menu to edit or delete saved access-point passwords.
- New **Ping interval** setting (50 ms–1 s, default 250 ms) for the elicitation
  ping.
- Logging in transmits and joins a network (everything else stays
  receive-only), so use it only on a network you are authorized to use. Saved
  passwords are plaintext on the SD card. Login needs board firmware v9 or
  newer and the elicitation ping needs v12 or newer; the login options are
  hidden on older firmware.

## v1.13

- The MAC vendor database is now part of the source, so apps installed from the
  Flipper Apps Catalog also show device vendors (before, only the GitHub
  release had them).
- The bundled vendor database is half the size (16-byte records instead of
  32). Your own `oui.db` in `apps_data/wavehound/` in the old format still
  works.

## v1.12

- New settings: **Hop dwell** (how long a hunt stays on each channel while
  searching for the target, 100 ms–2 s), **Re-search** (how long a locked hunt
  waits in silence before it searches the channels again, 4–60 s), **Scan dwell**
  (how long a scan stays on each channel, 120 ms–2 s; longer catches more quiet
  devices but sweeps and fills the list more slowly) and **Response** (Fast /
  Normal / Smooth smoothing of the hunt meter).
- **Reset to defaults** at the bottom of Settings restores every setting.
- Hop dwell, Re-search and Scan dwell need board firmware v8. Older firmware
  keeps the fixed 300 ms / 8 s / 120 ms.

## v1.11

- Picking a result from a scan and then pressing Back now keeps the devices
  already found and resumes scanning, landing you back on the row you came from,
  instead of clearing the list and starting over. Use "Search again" to clear
  and restart from scratch.

## v1.10

- The device details screen (shown when you pick a scanned result) now reports
  an access point's security — Open, WEP, WPA, WPA2 or WPA3 — and its 802.11
  mode (e.g. g, n, ax), shown inline on the channel line as
  "AP ch6 WPA2 n -52dBm". The board classifies these from the beacon's RSN/WPA
  information elements (WPA3 from SAE/OWE/Suite-B AKM suites) and its HT/HE
  elements. Requires board firmware v7.

## v1.9

- The hunt screen now shows the channel noise floor: a marker tick above the
  signal meter and an "nf" reading, so the gap between the signal and the noise
  (your headroom) is visible at a glance. Requires board firmware v6.
- More at-a-glance hunt cues: a trend arrow by the heat word shows whether the
  signal is rising or falling as you move, a frame-rate reading tells a chatty
  transmitter from a near-idle one, and a "quiet Ns" timer shows how long a lost
  target has been silent.

## v1.8

- Fixed "Keep backlight" leaving the screen stuck on. The setting forces the
  backlight on through a system-wide display state, which was only released when
  the setting was still enabled on leaving the hunt — so turning it off, or
  quitting the app straight from the hunt screen, left the backlight forced on
  even outside Wavehound. The app now always releases that state when the hunt
  ends and when it exits.

## v1.7

- New Settings options for hunting. "Haptic" buzzes the Flipper's motor as a
  proximity cue, pulsing faster as the signal strengthens (works even with sound
  muted). "Keep backlight" holds the backlight on while a hunt is on screen, so
  it doesn't dim out mid-sweep. "Range" (Near / Normal / Far) shifts the dBm
  window the meter spans, so the readout and click/haptic rate stay useful
  whether you're right on top of a strong transmitter or chasing a faint one.

## v1.6

- The dev board's LED now turns off when you leave the app, instead of staying
  lit (the board keeps running on the Flipper's always-on 3V3 rail, so the app
  now tells it to go dark on exit). Requires board firmware v5.

## v1.5

- New Settings screen: adjust the Geiger click volume, choose whether sound
  starts on or muted, and pick which channel option is pre-selected when hunting
  a saved MAC (its saved channel, auto-hop, or a nearby AP). Settings persist on
  the SD card.

## v1.4

- The live device list now shows an animated scan indicator: a blip sweeps back
  and forth across the header while scanning, so it reads as active even before
  any transmitter is found.

## v1.3

- Continuous scanning: Hunt access point / Hunt devices now drop straight into a
  live list that fills as transmitters are heard, instead of a fixed-length
  listen. Pick a target the moment it appears, or keep waiting as long as you
  like. "~ Search again" clears and restarts the scan.

## v1.2

- Rename the "Search devices" menu entry to "Hunt devices" for consistency.
- Declare the storage permission in the app manifest and add the project URL.

## v1.1

- Device details screen with vendor (OUI) lookup from a bundled database.
- Saved MACs (favorites): save from the hunt screen, then Hunt / Edit / Delete.
- Scope the device search to the clients of a selected access point.
- Board Info screen: firmware build, chip, radio, heap, uptime and reset reason.

## v1.0

- Initial public release: hunt access points, devices or a manually entered MAC
  by per-frame RSSI; channel spectrum view; Geiger-style audio and RGB LED heat
  indicator. Receive-only, driven by the ESP32-S2 WiFi dev board over UART.
