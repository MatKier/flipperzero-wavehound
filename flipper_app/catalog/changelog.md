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
