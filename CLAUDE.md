# CLAUDE.md

Guidance for AI agents working in this repo. See [README.md](README.md) for the
user-facing overview and [docs/PROTOCOL.md](docs/PROTOCOL.md) for the wire format.

## What this is

**Wavehound** is WiFi fox-hunting for the Flipper Zero WiFi dev board. It locates
a 2.4 GHz transmitter by its per-frame RSSI. It is **receive-only by default**
(promiscuous RX). The exception is the opt-in AP login and what runs over it:
host/name lookups (ARP, mDNS/SSDP), the elicitation ping and fingerprinting
(port scan + HTTP banner) all transmit. Keep any new transmitting feature
opt-in and say so in the README.

Two independent programs talk over UART:

- **`firmware/`** — ESP-IDF project for the ESP32-S2 on the dev board. Runs the
  real-time hunt/discover/spectrum state machine. C, ESP-IDF 5.x/6.x, built with
  PlatformIO, target `esp32s2`.
- **`flipper_app/`** — the Flipper external app (`.fap`), a thin UART client that
  renders what the board reports. C, Flipper firmware SDK (API 88.2, FW 1.5.1),
  built with `ufbt`.

The split is deliberate: the tight per-frame RSSI loop with channel parking/hopping
must run on the ESP32 in real time, so it lives in the board firmware, not the app.

## Layout

The file-by-file map is [docs/LAYOUT.md](docs/LAYOUT.md). Keep it current when
you add, move or remove a source file.

## Scene pattern (flipper_app)

Scenes are registered in `scenes/wh_scene_config.h` via the `ADD_SCENE(prefix,
name, id)` X-macro. Adding a scene means: add one `ADD_SCENE` line there, create
`scenes/wh_scene_<name>.c` with `wavehound_scene_<name>_{on_enter,on_event,on_exit}`,
and (usually) a new `WhCustomEvent` / `WhViewId` in `wavehound_i.h`. Shared app
state lives in `WavehoundApp` (`wavehound_i.h`); the UART worker writes results
under `results_mutex`.

## Building

```bash
python3 build_app.py          # ufbt build with the committed oui.db (needs ufbt on PATH)
python3 build_app.py --update-db --no-build  # refresh tools/manuf.gz + oui.db only
python3 build_app.py --launch # build, upload and run on a connected Flipper
cd flipper_app && ufbt        # app only (bundles the committed oui.db too)
cd firmware && pio run        # board firmware (PlatformIO)
```

The vendor database is committed data, not a build product: `flipper_app/assets/oui.db`
must be exactly what `tools/gen_oui.py tools/manuf.gz` produces (CI checks it
with `--check`). Refresh them together (`build_app.py --update-db`) and commit
both. Neither is MIT: they're GPL-2.0-or-later Wireshark data, see
`THIRD_PARTY_NOTICES.md`. `oui.db` starts with a `WHOUI` + record-size header,
so the record size can change without touching `wh_oui.c`; files without the
header are read as the older 32-byte-record format.

Don't assume `ufbt` / `pio` / ESP-IDF are installed in this environment — they
usually are not. Prefer reasoning about the code and keeping changes buildable;
only invoke the toolchains when the user asks or they're clearly available.

## Protocol is contract

`firmware/main/main.c` (parser) + `firmware/main/radio.c` (emitter) and the
`flipper_app` parsers (`wavehound.c`, `wh_uart.c`) are two ends of the plain-text,
line-based protocol in [docs/PROTOCOL.md](docs/PROTOCOL.md). Any change to a
command or message shape must stay consistent across **all three**: both firmware
and app code, and the PROTOCOL.md doc. Update the doc in the same change.

## Conventions

- Both sides ignore lines they don't understand (ESP32 ROM boot chatter is
  harmless), and fields are comma-separated ASCII, one message per `\n` line.
- App symbols: `wh_`/`Wh` prefix; firmware functions are plain C in their module.
- Radio config (pins, baud, LED GPIOs, max channel) is menuconfig/Kconfig, not
  hardcoded — e.g. `CONFIG_WH_MAX_CHANNEL`.
- Match the existing comment density and naming of the file you're editing.

## Versioning (agents may bump)

An agent may, **when it judges the change justifies it**, increase the version:

- **App:** `fap_version` in [flipper_app/application.fam](flipper_app/application.fam).
- **Board firmware:** `WH_FW_VERSION` in [firmware/main/proto.h](firmware/main/proto.h)
  (reported in `READY`/`PONG` and the `INFO` block).

Bump whichever side actually changed (app, firmware, or both). Use judgment:
a user-visible or protocol-affecting change warrants a bump; a pure refactor or
doc tweak usually does not.

### Tagging a release

Releases carry a **single git tag `v<fap_version>`** (e.g. `v1.3`) — there is
**no separate firmware tag**; a `WH_FW_VERSION` bump just rides along in the
release that happens to include it. One tag per app version; don't tag
intermediate commits.

Work normally lands on `main` as a **squash-merged PR**, so the release commit's
SHA only exists once the PR is merged. The flow after a merge is:

1. Set the catalog `commit_sha` (see below) and commit that to `main`.
2. Tag the **squash-merge commit** (the one carrying the new `fap_version`, e.g.
   `a2a3f32` for `v1.3`) as `v<fap_version>` and push the tag. The existing
   `v1.0`/`v1.1`/`v1.2` tags all point at their PR's squash-merge commit — not
   at the follow-up `commit_sha` commit — so match that.

If the version bump lands as a direct commit on `main` rather than via a PR
(as `v1.10` → `b11bf11`), tag that bump commit and point `commit_sha` at it.

Pushing the tag runs `.github/workflows/release.yml`, which builds both sides
and publishes the GitHub release (it fails if the tag isn't `v<fap_version>`
or `changelog.md` has no `## v<fap_version>` entry; the entry becomes the
release notes). The same workflow builds every branch push as a CI check,
except pushes that only touch docs, Markdown, catalog metadata (bar
`changelog.md`) or the other workflow (`paths` filter in `release.yml`; keep it
in step if the build starts reading a new file there). Tag pushes always run.
The toolchain caches (ufbt and PlatformIO, keyed separately) are saved on
`main` only, because tag runs can only restore caches saved on `main`. CI
doesn't download `manuf`; it builds and ships the committed `tools/manuf.gz` /
`oui.db`. If an ESP-IDF/platform bump moves or adds linked components, update
`LICENSES` in `tools/package_release.py` and the table in
`THIRD_PARTY_NOTICES.md`.

## Catalog metadata

The [Flipper Apps Catalog](https://github.com/flipperdevices/flipper-application-catalog)
submission files live in `flipper_app/catalog/`: `manifest.yml`, `changelog.md`
and `screenshots/`.

**Keep `manifest.yml` and `changelog.md` in sync** — with each other and with
[flipper_app/application.fam](flipper_app/application.fam):

- Every app version bump (`fap_version`) gets a new top entry in `changelog.md`
  (newest first), and the catalog version is driven by `fap_version`.
- `manifest.yml` leans on `application.fam` for the shared fields (name, id,
  category, version, author, icon, short_description) — don't duplicate them.
  If one of those changes in the `.fam`, there's nothing to change in the
  manifest, but do check they still agree.
- The manifest's `changelog` and `screenshots` paths must point at files that
  exist.
- `commit_sha` must point at the commit on `main` that actually carries the
  submitted code: the **squash-merge commit** of the release PR, the same commit
  the `v<version>` tag is on (flow in [Tagging a release](#tagging-a-release)).
  The follow-up commit that sets it (message like
  `catalog: set manifest commit_sha to the merged commit`) is **not** itself the
  referenced/tagged commit. See `v1.2` → `046038f` and `v1.3` → `a2a3f32`.

Path gotcha: the catalog bundler resolves the manifest's `@file` and screenshot
paths relative to `sourcecode.subdir` (`flipper_app`), **not** to the manifest's
own folder — hence the `catalog/` prefix on those paths.
