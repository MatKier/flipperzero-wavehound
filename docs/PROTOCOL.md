# Wavehound UART protocol

Link: Flipper USART (GPIO 13 TX / 14 RX) <-> ESP32-S2 UART0 (GPIO 43 TX / 44 RX),
115200 baud, 8N1. Every message is one ASCII line terminated by `\n` (a
preceding `\r` is ignored). Fields are separated by `,`. Both sides ignore lines
they do not understand, so ROM boot chatter from the ESP32 is harmless.

## Flipper -> board

| Command               | Meaning                                                        |
|-----------------------|----------------------------------------------------------------|
| `PING`                | Liveness check. Board answers `PONG,<version>`.                |
| `HUNT,<mac>,<ch>[,PIN]` | Track frames transmitted by `<mac>` on channel `<ch>`. `ch=0` hops until the target is heard. `PIN` (needs `ch>0`) keeps the radio on `<ch>` even when the target goes quiet. |
| `SPECTRUM`            | Sweep channels 1..N, report per-channel activity.              |
| `DISCOVER[,ALL\|AP\|STA\|CLIENTS[,<ch>[,<bssid>]]]` | Listen **continuously**, streaming each transmitter as it is first heard, until `STOP`. `AP` keeps only access points, `STA` keeps everything else, `ALL` (default) keeps both. `CLIENTS` keeps only stations associated with `<bssid>` (required for this filter) and reports them as `DEV` lines the same way. `<ch>` 1-13 parks the radio on that channel; `0` or omitted hops all channels (pass the AP's channel with `CLIENTS`). |
| `INFO`                | Report board version, identity and health as an `INFO_BEGIN` / `BI,...` / `INFO_END` block. |
| `STOP`                | Return to idle (radio listening off). Ends whatever is running: a hunt, a spectrum sweep or a discovery. The idle LED stays lit (blue). |
| `SLEEP`               | Like `STOP`, but also turn the LED off (dark idle). The app sends this on exit, since the board stays powered on the Flipper's always-on 3V3 rail. |

`<mac>` is `aa:bb:cc:dd:ee:ff` (case-insensitive).

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
| `DEV,<mac>,<ch>,<rssi>,<frames>,<flags>,<ssid>` | One transmitter, streamed once in discovery order as it is first heard (max 96; the board tracks up to 192). `rssi` is the strongest frame heard. `flags`: `A` access point (sent beacons) or `S` station, plus `R` if the MAC is locally administered (randomized). `ssid` (rest of line) is only set for access points. For APs, `ch` comes from the beacon's DS parameter set. |
| `INFO_BEGIN`                                   | Start of an `INFO` reply.             |
| `BI,<label>,<value>`                           | One board-info row (reply to `INFO`). `value` is the rest of the line and never contains a comma. Labels include Firmware, Commit, Built, IDF (run/build), ELF SHA, Chip, Cores, Radio, MAC, Flash, App part, App used, Heap free/min, Uptime, Temp, Reset. `Firmware` is the board firmware version (the same value carried by `READY`/`PONG`); `Commit` is the build system's version string (`git describe` for a git checkout, else `version.txt`/`PROJECT_VER`). `App part` is the flash slot reserved for the firmware; `App used` is the running image's actual size and its fill of that slot. |
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
With `ch=0`, or after 8 s of silence (unless `PIN`), it hops channels (300 ms
dwell) until it hears the target again.

SSIDs are sanitized: control characters become `?`, so every message stays on one line.
