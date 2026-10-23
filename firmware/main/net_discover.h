#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_netif.h"

/*
 * Local-network host discovery: once associated, sweep the subnet with ARP to
 * learn each host's IPv4 (matched to the MACs the hunt/discover scans report),
 * and make a best-effort attempt at a hostname (reverse DNS, then NBNS). Results
 * stream to the Flipper as IP,<mac>,<ip>[,<host_hex>] lines. All lwip calls run
 * in the tcpip thread (this build has core-locking off); the slow socket
 * resolution runs in a dedicated low-priority task so the radio loop is never
 * blocked.
 */

/** Create the background discovery task and its sync primitives. Call once. */
void net_discover_init(void);

/**
 * A DHCP lease arrived: remember the subnet / DNS server and kick a sweep.
 * `ip`, `mask` and `dns` are IPv4 addresses in network byte order
 * (esp_ip4_addr_t.addr); `dns` may be 0 if none.
 */
void net_discover_on_gotip(esp_netif_t *netif, uint32_t ip, uint32_t mask, uint32_t dns);

/** The link went down: forget every host and stop sweeping. */
void net_discover_on_down(void);

/** Re-run a sweep now (e.g. when the client list opens), if still connected. */
void net_discover_rescan(void);

/**
 * Look up a known host's IPv4 (network byte order) by MAC. Returns false if the
 * MAC has not been seen in a sweep yet.
 */
bool net_discover_lookup(const uint8_t mac[6], uint32_t *out_ip);

/**
 * Start eliciting reply frames from `mac` so a hunt can sample it: poke it every
 * `interval_ms` (clamped to 50-1000) with a unicast ARP request (primary) and an
 * ICMP echo (complement). The target's IP is read from the sweep table; the
 * caller is expected to have discovered it first (the app only enables
 * elicitation once the IP is known). If the MAC isn't mapped, nothing is sent --
 * this does not itself trigger a sweep. Emits `ELICIT,ON,<ms>`.
 */
void net_discover_elicit_start(const uint8_t mac[6], uint32_t interval_ms);

/** Stop elicitation. Emits `ELICIT,OFF` if it was running. */
void net_discover_elicit_stop(void);
