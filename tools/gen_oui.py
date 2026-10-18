#!/usr/bin/env python3
"""Build Wavehound's OUI vendor database (oui.db) from Wireshark's `manuf` file.

The Flipper reads oui.db as sorted, fixed-width records so it can binary-search
by seeking (no need to load megabytes into RAM):

    record = 3-byte OUI  +  29-byte vendor name (NUL-padded)   = 32 bytes

Only 24-bit (/24) prefixes are used; the longer /28 and /36 blocks are skipped,
so the owner of the parent /24 is reported for those ranges.

Usage:
    # from a local manuf file (download it yourself):
    #   https://www.wireshark.org/download/automated/data/manuf.gz   (gunzip it)
    python3 tools/gen_oui.py manuf -o oui.db

    # or let the script fetch + gunzip it for you:
    python3 tools/gen_oui.py --download -o oui.db

Then copy oui.db to the SD card at /ext/apps_data/wavehound/oui.db.

Source data is Wireshark's manufacturer database (derived from the IEEE
registry); redistribute it per Wireshark's terms with attribution.
"""

import argparse
import gzip
import re
import sys
import urllib.request

MANUF_URL = "https://www.wireshark.org/download/automated/data/manuf.gz"
RECORD = 32
OUI_LEN = 3
NAME_LEN = RECORD - OUI_LEN  # 29

OUI24_RE = re.compile(r"^([0-9A-Fa-f]{2}):([0-9A-Fa-f]{2}):([0-9A-Fa-f]{2})$")


def parse_manuf(lines):
    """Return {oui_bytes: short_name} for every /24 entry (last one wins)."""
    table = {}
    for line in lines:
        line = line.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        # Collapse to [prefix, short_name, ...]; manuf uses tabs but be lenient.
        fields = [f for f in (p.strip() for p in fields) if f]
        if len(fields) < 2:
            fields = line.split(None, 1)
        if len(fields) < 2:
            continue
        m = OUI24_RE.match(fields[0])
        if not m:
            continue  # /28, /36 or malformed: skip
        oui = bytes(int(g, 16) for g in m.groups())
        name = fields[1].strip()
        if name:
            table[oui] = name
    return table


def write_db(table, path):
    count = 0
    with open(path, "wb") as f:
        for oui in sorted(table):
            name = table[oui].encode("ascii", "replace")[:NAME_LEN]
            name = name + b"\x00" * (NAME_LEN - len(name))
            f.write(oui + name)
            count += 1
    return count


def main():
    ap = argparse.ArgumentParser(description="Build oui.db from Wireshark manuf.")
    ap.add_argument("manuf", nargs="?", help="path to a manuf text file")
    ap.add_argument("--download", action="store_true", help="fetch manuf.gz from Wireshark")
    ap.add_argument("-o", "--output", default="oui.db", help="output path (default oui.db)")
    args = ap.parse_args()

    if args.download:
        print(f"Downloading {MANUF_URL} ...", file=sys.stderr)
        with urllib.request.urlopen(MANUF_URL) as resp:
            text = gzip.decompress(resp.read()).decode("utf-8", "replace")
        lines = text.splitlines()
    elif args.manuf:
        with open(args.manuf, "r", encoding="utf-8", errors="replace") as fh:
            lines = fh.readlines()
    else:
        ap.error("give a manuf path or --download")

    table = parse_manuf(lines)
    count = write_db(table, args.output)
    size = count * RECORD
    print(f"Wrote {count} vendors to {args.output} ({size} bytes).", file=sys.stderr)


if __name__ == "__main__":
    main()
