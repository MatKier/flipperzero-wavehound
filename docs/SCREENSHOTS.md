# Screenshots

Taken with qFlipper on board firmware v17 / app v1.17. In the shots under
[screenshots/](screenshots/) SSIDs, hostnames, IPs and MACs are partly
pixelated (the pixelated part was replaced with random noise first, so it can't
be restored). The ones without identifiers are the unmodified catalog shots in
[flipper_app/catalog/screenshots/](../flipper_app/catalog/screenshots/).

## Menu and access points

| | |
|---|---|
| <img src="../flipper_app/catalog/screenshots/main_menu.png" width="384" alt="Main menu"> | <img src="screenshots/ap_list.png" width="384" alt="Access point list"> |
| Main menu, once the board answers. | **Hunt access point**: the list fills as networks are heard (dBm, channel, SSID). |
| <img src="screenshots/ap_details.png" width="384" alt="Access point details"> | <img src="screenshots/hunt_ap.png" width="384" alt="Hunting an access point"> |
| AP details: MAC, vendor, channel, security, 802.11 mode and signal. | Hunting an AP: dBm and frame rate, trend arrow, peak, meter and noise floor. |

## Devices and login

| | |
|---|---|
| <img src="../flipper_app/catalog/screenshots/device_scope.png" width="384" alt="Device search scope"> | <img src="screenshots/ap_login.png" width="384" alt="AP login menu"> |
| **Hunt devices** scope: all channels, clients of a nearby AP, or one channel. | Login offered after picking an AP for its client list. |
| <img src="screenshots/clients_mac.png" width="384" alt="Client list by MAC"> | <img src="screenshots/clients_names.png" width="384" alt="Client list with hostnames"> |
| Client list without login (or before lookups finish): MACs only. | Logged in: rows show the resolved hostname, else the IP. |
| <img src="screenshots/device_details.png" width="384" alt="Device details"> | <img src="screenshots/fingerprint.png" width="384" alt="Fingerprint result"> |
| Device details with vendor, mDNS type and the **Scan** button. | Fingerprint result: device label, IP and open ports. |
| <img src="../flipper_app/catalog/screenshots/hunt_device_lost.png" width="384" alt="Hunt screen, target quiet"> | <img src="../flipper_app/catalog/screenshots/hunt_device_ping.png" width="384" alt="Hunt screen with ping"> |
| Hunting a quiet device: `quiet 5s`, `ping` available (Down). | The same device with the elicitation ping on: a steady stream of frames. |

## Tools

| | |
|---|---|
| <img src="../flipper_app/catalog/screenshots/spectrum.png" width="384" alt="Channel spectrum"> | <img src="../flipper_app/catalog/screenshots/board_info.png" width="384" alt="Board info"> |
| Channel spectrum: packets per channel, peak hold, busiest channel. | Board info: firmware, commit, build, ESP-IDF, ELF hash and more. |
| <img src="../flipper_app/catalog/screenshots/settings.png" width="384" alt="Settings"> | |
| Settings. | |
