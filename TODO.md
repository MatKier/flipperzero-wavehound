# TODO / ideas

Loose, non-committal notes. Nothing here is planned work — just things we
might do.

## Maybe

### Elicitation ping sends ARP + ICMP every interval (double rate)

`firmware/main/net_discover.c` (`elicit_task`) currently sends **both** a unicast
ARP request and an ICMP echo each interval, so a responsive target replies up to
twice per interval — roughly double the on-air frames of a single-method probe.
Kept deliberately for now (more reply frames = a steadier hunt RSSI signal, and
the rate is still low), but it is not the "ARP-primary, ICMP *fallback*" that was
intended. Options if we revisit: ARP only (halve the traffic — ARP alone almost
always elicits a reply), or a true fallback (ARP every interval, ICMP only once
the target stops answering ARP). See the comment at the send site in `elicit_task`.
