#!/usr/bin/env python3
"""Build the Wavehound Flipper app with the vendor database bundled in.

Steps:
  1. Generate the OUI vendor database (via tools/gen_oui.py) into
     flipper_app/assets/oui.db, which application.fam bundles as a file asset
     (fap_file_assets="assets") and the Flipper unpacks to
     /ext/apps_assets/wavehound/oui.db.
  2. Build the .fap with ufbt.

Usage:
  python3 build.py                     # download manuf, build db, then ufbt
  python3 build.py --manuf path/to/manuf   # use a local manuf file instead
  python3 build.py --skip-db           # just build (keep existing assets/oui.db)
  python3 build.py --no-build          # only (re)generate the db
  python3 build.py --launch            # build, then upload+run on a connected Flipper
  python3 build.py -- --target f7 ...  # args after -- are passed to ufbt

Needs ufbt on PATH (https://github.com/flipperdevices/flipperzero-ufbt) and,
for the db download, network access.
"""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
APP_DIR = ROOT / "flipper_app"
ASSETS_DIR = APP_DIR / "assets"
DB_PATH = ASSETS_DIR / "oui.db"
GEN = ROOT / "tools" / "gen_oui.py"


def run(cmd, cwd=None):
    print("+ " + " ".join(str(c) for c in cmd), file=sys.stderr)
    subprocess.run(cmd, cwd=cwd, check=True)


def build_db(manuf):
    ASSETS_DIR.mkdir(parents=True, exist_ok=True)
    cmd = [sys.executable, str(GEN), "-o", str(DB_PATH)]
    if manuf:
        cmd.append(str(manuf))
    else:
        cmd.append("--download")
    run(cmd)


def build_fap(ufbt, launch, extra):
    exe = shutil.which(ufbt)
    if not exe:
        sys.exit(
            f"'{ufbt}' not found on PATH. Install it with "
            "'pip install ufbt' (see flipperzero-ufbt), or pass --no-build."
        )
    # `ufbt launch` builds, uploads the .fap to a connected Flipper and runs it;
    # plain `ufbt` just builds.
    cmd = [exe] + (["launch"] if launch else []) + list(extra)
    run(cmd, cwd=APP_DIR)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--manuf", help="local Wireshark manuf file (default: download)")
    ap.add_argument("--skip-db", action="store_true", help="do not regenerate oui.db")
    ap.add_argument("--no-build", action="store_true", help="only build the db, skip ufbt")
    ap.add_argument(
        "--launch",
        action="store_true",
        help="after building, upload the .fap to a connected Flipper and run it (ufbt launch)",
    )
    ap.add_argument("--ufbt", default="ufbt", help="ufbt command (default: ufbt)")
    ap.add_argument("ufbt_args", nargs="*", help="args passed through to ufbt (after --)")
    args = ap.parse_args()

    if not args.skip_db:
        build_db(args.manuf)
    elif not DB_PATH.exists():
        print(f"warning: --skip-db but {DB_PATH} does not exist", file=sys.stderr)

    if not args.no_build:
        build_fap(args.ufbt, args.launch, args.ufbt_args)

    print("Done.", file=sys.stderr)


if __name__ == "__main__":
    main()
