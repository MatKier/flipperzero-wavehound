#!/usr/bin/env python3
"""Build Wavehound's OUI vendor database (oui.db) from Wireshark's `manuf` file.

The Flipper reads oui.db as sorted, fixed-width records so it can binary-search
by seeking (no need to load megabytes into RAM):

    header = "WHOUI" + record size (1 byte), NUL-padded to one record
    record = 3-byte OUI  +  13-byte vendor name (NUL-padded)   = 16 bytes

The app takes the record size from the header, so RECORD can change without an
app update. Wireshark's short names are at most 12 characters, so 13 bytes
holds them with room to spare; longer ones would be cut (the count is
reported). Files without the header are read as the older format with 32-byte
records.

Only 24-bit (/24) prefixes are used; the longer /28 and /36 blocks are skipped,
so the owner of the parent /24 is reported for those ranges.

Usage:
    # from a local manuf or manuf.gz:
    #   https://www.wireshark.org/download/automated/data/manuf.gz
    python3 tools/gen_oui.py tools/manuf.gz -o oui.db

    # or let the script fetch it (and keep the .gz it used):
    python3 tools/gen_oui.py --download --save-manuf manuf.gz -o oui.db

    # check that an existing oui.db is exactly what this manuf produces:
    python3 tools/gen_oui.py tools/manuf.gz --check flipper_app/assets/oui.db

The repo's bundled copy is flipper_app/assets/oui.db, built from tools/manuf.gz
(refresh both with `python3 build_app.py --update-db`). Your own copy can go on
the SD card at /ext/apps_data/wavehound/oui.db.

Source data is Wireshark's manufacturer database (derived from the IEEE
registry), GPL-2.0-or-later; oui.db is derived from it and carries the same
license. See THIRD_PARTY_NOTICES.md.
"""

import argparse
import gzip
import re
import sys
import urllib.request

MANUF_URL = "https://www.wireshark.org/download/automated/data/manuf.gz"
MAGIC = b"WHOUI"
RECORD = 16
OUI_LEN = 3
NAME_LEN = RECORD - OUI_LEN  # 13

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


def build_db(table):
    """Return (oui.db bytes, record count, names truncated)."""
    out = bytearray(MAGIC + bytes([RECORD]))
    out += b"\x00" * (RECORD - len(out))
    truncated = 0
    for oui in sorted(table):
        name = table[oui].encode("ascii", "replace")
        if len(name) > NAME_LEN:
            truncated += 1
            name = name[:NAME_LEN]
        out += oui + name + b"\x00" * (NAME_LEN - len(name))
    return bytes(out), len(table), truncated


def main():
    ap = argparse.ArgumentParser(description="Build oui.db from Wireshark manuf.")
    ap.add_argument("manuf", nargs="?", help="path to a manuf text file (or manuf.gz)")
    ap.add_argument("--download", action="store_true", help="fetch manuf.gz from Wireshark")
    ap.add_argument("--save-manuf", metavar="PATH", help="with --download: also save the fetched manuf.gz here")
    ap.add_argument("-o", "--output", default="oui.db", help="output path (default oui.db)")
    ap.add_argument("--check", metavar="DB", help="don't write; exit 1 unless DB matches what manuf produces")
    args = ap.parse_args()

    if args.download:
        print(f"Downloading {MANUF_URL} ...", file=sys.stderr)
        with urllib.request.urlopen(MANUF_URL) as resp:
            raw = resp.read()
        if args.save_manuf:
            with open(args.save_manuf, "wb") as fh:
                fh.write(raw)  # verbatim, so the gzip header keeps Wireshark's date
        text = gzip.decompress(raw).decode("utf-8", "replace")
        lines = text.splitlines()
    elif args.manuf:
        opener = gzip.open if args.manuf.endswith(".gz") else open
        with opener(args.manuf, "rt", encoding="utf-8", errors="replace") as fh:
            lines = fh.readlines()
    else:
        ap.error("give a manuf path or --download")

    db, count, truncated = build_db(parse_manuf(lines))
    if truncated:
        print(f"warning: {truncated} vendor names cut to {NAME_LEN} characters", file=sys.stderr)

    if args.check:
        try:
            with open(args.check, "rb") as fh:
                ok = fh.read() == db
        except OSError:
            ok = False
        if not ok:
            sys.exit(
                f"{args.check} does not match {args.manuf or MANUF_URL}. Regenerate both "
                "together (python3 build_app.py --update-db, or --manuf PATH) and commit them."
            )
        print(f"{args.check} matches ({count} vendors).", file=sys.stderr)
        return

    with open(args.output, "wb") as f:
        f.write(db)
    print(f"Wrote {count} vendors to {args.output} ({len(db)} bytes).", file=sys.stderr)


if __name__ == "__main__":
    main()
