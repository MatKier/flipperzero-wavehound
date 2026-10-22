# Wavehound UART protocol

Link: Flipper USART (GPIO 13 TX / 14 RX) <-> ESP32-S2 UART0 (GPIO 43 TX / 44 RX),
115200 baud, 8N1. Every message is one ASCII line terminated by `\n` (a
preceding `\r` is ignored). Fields are separated by `,`. Both sides ignore lines
they do not understand, so ROM boot chatter from the ESP32 is harmless.

## Flipper -> board

| Command               | Meaning                                                        |
|-----------------------|----------------------------------------------------------------|
| `PING`                | Liveness check. Board answers `PONG,<version>`.                |
| `HUNT,<mac>,<ch>[,PIN][,DWELL=<ms>][,LOST=<s>]` | Track frames transmitted by `<mac>` on channel `<ch>`. `ch=0` hops until the target is heard. `PIN` (needs `ch>0`) keeps the radio on `<ch>` even when the target goes quiet. Optional [timing tokens](#timing-tokens): `DWELL` is the per-channel dwell while searching (default 300 ms), `LOST` the silence before a locked hunt searches again (default 8 s). |
| `SPECTRUM`            | Sweep channels 1..N, report per-channel activity.              |
| `DISCOVER[,ALL\|AP\|STA\|CLIENTS[,<ch>[,<bssid>]]][,DWELL=<ms>]` | Listen **continuously**, streaming each transmitter as it is first heard, until `STOP`. `AP` keeps only access points, `STA` keeps everything else, `ALL` (default) keeps both. `CLIENTS` keeps only stations associated with `<bssid>` (required for this filter) and reports them as `DEV` lines the same way. `<ch>` 1-13 parks the radio on that channel; `0` or omitted hops all channels (pass the AP's channel with `CLIENTS`). Optional [timing token](#timing-tokens) `DWELL` (after `<bssid>` for `CLIENTS`, else after `<ch>`) sets the per-channel dwell when hopping (default 120 ms); `DEV` lines are also streamed at that cadence. |
| `CONNECT,<ssid_hex>,<psk_hex>[,<bssid>][,<ch>]` | Associate with an access point in station mode (v9+). `<ssid_hex>` and `<psk_hex>` are the SSID and passphrase [hex-encoded](#hex-encoded-fields); an empty `<psk_hex>` joins an open network. The optional `<bssid>` (a MAC) and `<ch>` (1-13) pin and speed up the join and may appear in either order. Promiscuous RX stays live, so a hunt keeps sampling on the AP's channel. Progress is reported as [`CONN,...`](#association) lines. |
| `DISCONNECT`          | Drop the association and return to pure receive-only sniffing. Answered with `CONN,DOWN`. |
| `INFO`                | Report board version, identity and health as an `INFO_BEGIN` / `BI,...` / `INFO_END` block. |
| `STOP`                | Return to idle (radio listening off). Ends whatever is running: a hunt, a spectrum sweep or a discovery. The idle LED stays lit (blue). |
| `SLEEP`               | Like `STOP`, but also turn the LED off (dark idle). The app sends this on exit, since the board stays powered on the Flipper's always-on 3V3 rail. |

`<mac>` is `aa:bb:cc:dd:ee:ff` (case-insensitive).

### Hex-encoded fields

Free-text values that may contain a comma, space or other delimiter byte — the
SSID and passphrase in `CONNECT`, and the SSID/passphrase/hostname the board
returns — travel as lowercase or uppercase **hex** (two characters per byte, no
`0x`), so they can never break the comma-separated, one-line framing. An empty
field is the empty string. For example SSID `My, AP` is `4d792c204150`, and an
open network's passphrase is the empty field.

### Timing tokens

`HUNT` and `DISCOVER` take optional trailing `KEY=value` tokens, after all
positional fields, in any order. Unknown tokens are ignored. Out-of-range values
are clamped. A missing token keeps the default.

| Token          | Range        | Applies to         | Default |
|----------------|--------------|--------------------|---------|
| `DWELL=<ms>`   | 100–2000     | `HUNT` (rounded to 100 ms), `DISCOVER` | 300 / 120 |
| `LOST=<s>`     | 2–120        | `HUNT`             | 8       |

Firmware before v8 only reads the positional fields (and `PIN` right after
`<ch>`), so it ignores the tokens and keeps the defaults. Senders must therefore
put `PIN` before any token.

## Board -> Flipper

| Message                                        | When                                  |
|------------------------------------------------|---------------------------------------|
| `READY,<version>`                              | Once after boot.                      |
| `PONG,<version>`                               | Reply to `PING`.                      |
| `OK,<command>`                                 | Command accepted.                     |
| `ERR,<reason>`                                 | Command rejected.                     |
| `H,<ch>,<frames>,<last>,<avg>,<max>,<locked>,<nf>` | Hunt sample, every 100 ms. RSSI values in dBm, `0` when no frame was heard in the window. `locked` is `1` while parked on a channel, `0` while hopping. `nf` is the mean noise floor (dBm) over the window, `0` when nothing was heard; the app marks it on the signal meter. |
| `S,<ch>,<frames>,<bytes>,<max_rssi>`           | Spectrum dwell result for one channel (120 ms dwell). |
| `U,<unique>`                                   | End of a spectrum sweep: distinct transmitter MACs heard during it. |
| `DISCOVER_BEGIN,<filter>,<ch>`                 | Search started (`ch` 0 = hopping).    |
| `DEV,<mac>,<ch>,<rssi>,<frames>,<flags>,<ssid>` | One transmitter, streamed once in discovery order as it is first heard (max 96; the board tracks up to 192). `rssi` is the strongest frame heard. `flags` is an unordered set of single-char codes: `A` access point (sent beacons) or `S` station; `R` if the MAC is locally administered (randomized); and, for an AP whose beacon was parsed, one security code (`o` open, `w` WEP, `1` WPA, `2` WPA2, `3` WPA3) and one PHY code (`g` 802.11b/g, `n` 802.11n, `c` 802.11ac, `x` 802.11ax). Security and PHY codes are absent for stations and for older firmware; readers must match codes individually, not by position. `ssid` (rest of line) is only set for access points. For APs, `ch` comes from the beacon's DS parameter set. |
| `CONN,<state>[,...]`                           | Station-association progress (v9+). See [Association](#association). |
| `INFO_BEGIN`                                   | Start of an `INFO` reply.             |
| `BI,<label>,<value>`                           | One board-info row (reply to `INFO`). `value` is the rest of the line and never contains a comma. Labels include Firmware, Commit, Built, IDF (run/build), ELF SHA, Chip, Cores, Radio, MAC, Assoc, IP, Flash, App part, App used, Heap free/min, Uptime, Temp, Reset. `Firmware` is the board firmware version (the same value carried by `READY`/`PONG`); `Commit` is the build system's version string (`git describe` for a git checkout, else `version.txt`/`PROJECT_VER`). `Assoc` is the SSID the board is associated with (`-` when not), `IP` its DHCP lease (`-` when none). `App part` is the flash slot reserved for the firmware; `App used` is the running image's actual size and its fill of that slot. |
| `INFO_END`                                     | End of an `INFO` reply.               |

A transmitter counts as an access point if it sends beacons or probe responses,
management frames where it is the BSSID (addr2 == addr3), or data frames coming
from the distribution system (FromDS=1, ToDS=0). The last two catch an AP
whose beacon was missed. With the `AP` filter, other frames are not even
recorded, so stations cannot fill up the table.

A station counts as a client of an AP (the `CLIENTS` filter) when it sends a
data frame to the distribution system (ToDS=1, FromDS=0) whose receiver
(addr1) is the AP's BSSID; the transmitter (addr2) is the client. Even an idle
client is caught this way through its power-save Null frames. Clients are
reported as ordinary station `DEV` lines (flag `S`, no SSID).

Hunting works for any transmitter MAC, not just access points: the board
matches the frame's transmitter address (addr2) of management and data frames.
With `ch=0`, or after `LOST` seconds of silence (default 8 s; never with `PIN`),
it hops channels (`DWELL` per channel, default 300 ms) until it hears the target
again.

### Association

`CONNECT` associates the board with an AP in station mode while keeping
promiscuous RX on, so a hunt keeps sampling frames on the AP's channel (the
board no longer retunes the channel while associated — the connection owns it).
Progress is reported as `CONN,<state>` lines:

| Line | Meaning |
|------|---------|
| `CONN,ASSOC,<bssid>,<ch>`       | Layer-2 associated with `<bssid>` on channel `<ch>` (no IP yet). |
| `CONN,GOTIP,<ip>,<gw>,<dns>`    | DHCP lease obtained: our IPv4, the gateway, and the main DNS server (`0.0.0.0` if none). |
| `CONN,FAIL,<reason>`            | The join failed before a lease; `<reason>` is the Wi-Fi driver's disconnect reason code (or a negative `esp_err_t` for a local setup failure). |
| `CONN,DOWN`                     | An established link dropped, or `DISCONNECT`/`SLEEP` tore it down. |

`CONNECT` is also acknowledged with `OK,CONNECT` as soon as the arguments parse;
the `CONN,...` lines follow asynchronously. `SLEEP` disconnects as well; `STOP`
does **not** (so the link survives moving from a client scan into a hunt).

SSIDs are sanitized: control characters become `?`, so every message stays on one line.
