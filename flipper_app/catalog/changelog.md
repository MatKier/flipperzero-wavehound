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
