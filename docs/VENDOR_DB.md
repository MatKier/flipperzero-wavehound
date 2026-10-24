# Vendor database

The details screen resolves a MAC's vendor from its OUI (the first 3 bytes)
using `oui.db`, a sorted fixed-width binary file (a `WHOUI` + record-size
header, then 16-byte records: 3-byte OUI + 13-byte name) that the app
binary-searches on the SD card, so it never loads into RAM. Because the record
size is in the header it can change without touching `wh_oui.c`; older
header-less files with 32-byte records still work.

`flipper_app/assets/oui.db` is committed together with the Wireshark `manuf` it
is built from (`tools/manuf.gz`), so every build bundles it, including the
Flipper Apps Catalog build (`fap_file_assets` in `application.fam`). The app
looks for it in two places, in order:

1. `/ext/apps_data/wavehound/oui.db`: your own drop-in, e.g. newer data.
   `python3 tools/gen_oui.py --download -o oui.db` makes one.
2. `/ext/apps_assets/wavehound/oui.db`: the bundled copy, unpacked when the app
   is installed.

Without a database, non-randomized MACs show "Unknown"; randomized MACs always
show `randomized`.

## Refreshing the bundled copy

Run `python3 build_app.py --update-db` (add `--no-build` to skip the app build).
It downloads the latest `manuf` into `tools/manuf.gz` and regenerates `oui.db`;
commit both files together. CI runs `tools/gen_oui.py --check` and fails if
`oui.db` doesn't match `tools/manuf.gz` byte for byte. `--manuf path/to/manuf.gz`
regenerates from a local file instead.

## License

The data comes from Wireshark's `manuf` (derived from the IEEE registry) and is
GPL-2.0-or-later, as is `oui.db`, which is derived from it. Neither file is under
Wavehound's MIT license; see [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).
