# Host discovery flow (board firmware, v16+)

How the board finds and names hosts once it holds a DHCP lease. The wire format
of the results (`IP,<mac>,<ip>[,<host_hex>[,<type>]]`) is in
[PROTOCOL.md](PROTOCOL.md#host-discovery); this page covers the timing. The code
is `sweep_once()` in `firmware/main/net_discover.c`.

## One sweep

A sweep runs on `CONN,GOTIP`, when a client scan opens, and on `RESOLVE`. It runs
up to `ND_SWEEP_PASSES` (3) passes over the /24 around the board.

```
sweep_once (one trigger: GOTIP / client list opens / RESOLVE)

PASS 0
 |-- ARP chunks over the /24 ........ ~6.5 s: 6 requests, wait 150 ms, read ARP cache
 |      -> IP,<mac>,<ip> announced as hosts appear
 |-- SEND mDNS (1 packet, 14 PTR) + 3x SSDP M-SEARCH   (sockets opened here)
 |-- LISTEN 1000 ms  [window A]
 |      answer -> match by source IP -> set name/type -> re-announce IP line
 |      (IP not in table yet -> pending cache, applied when the host appears)
 |-- PTR / NBNS for hosts still without a name (up to ~0.8 s per host)
 v
GAP
 |-- LISTEN 1500 ms  [window B]  (listen only, no query sent)
 v
PASS 1
 |-- ARP only for addresses not found yet (known ones skipped, chunks instant)
 |-- SEND mDNS + SSDP again
 |-- LISTEN 1000 ms  [window C]
 |-- PTR / NBNS for hosts still unnamed
 v
GAP
 |-- LISTEN 1500 ms  [window D]  (listen only)
 v
PASS 2
 |-- ARP only for addresses still missing
 |-- no query sent (ND_QUERY_SENDS = 2)
 |-- PTR / NBNS for hosts still unnamed
 v
CLOSE sockets (anything arriving after this is lost)
```

## Name precedence on a row

```
mDNS instance / TXT fn=  >  PTR (router DNS)  >  NBNS
      (can overwrite)        (only fills an empty name)
```

A device type (`Chromecast`, `Printer`, ...) comes from mDNS or SSDP only. Each
type has a rank and the lowest rank wins, so a device advertising several
services keeps the most specific label.

## Notes

- The mDNS question is sent from an ephemeral port with the QU bit set, so
  responders reply by unicast. The board never binds 5353 or joins the multicast
  group.
- The sockets stay open for the whole sweep. Replies that arrive during the ARP
  chunks or the PTR/NBNS phase wait in the socket queue until the next listen
  window; the lwIP UDP queue is only 6 datagrams deep, so a burst can drop some.
- A slow responder can miss window A and be caught in B, C or D. Reverse DNS may
  already have named it; the mDNS name then replaces that name.
- An answer whose IP is not in the host table yet is parked in a small pending
  cache (`ND_PEND_MAX` = 8) and applied when ARP discovers the host.
- A host gets at most `ND_NAME_MAX_TRIES` (3) reverse-DNS/NBNS attempts across
  passes and sweeps, so one with no name isn't retried forever.
- Nothing is read after pass 2, so a device that answers only then is picked up
  by the next sweep.

Timings are the `ND_*` constants at the top of `net_discover.c`
(`ND_QUERY_DRAIN_MS`, `ND_PASS_GAP_MS`, `ND_QUERY_SENDS`, `ND_SWEEP_PASSES`,
`ND_NAME_MAX_TRIES`).
