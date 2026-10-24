# Building from source

To flash a prebuilt release you only need [download mode](#download-mode); see
the [README](../README.md#install).

## Board firmware (PlatformIO)

The firmware is built with **PlatformIO**, which pulls its own ESP-IDF
toolchain (pinned to ESP-IDF 6.1), so you don't need a separate ESP-IDF install.
Install PlatformIO Core once with [pipx](https://pipx.pypa.io/) (e.g.
`apt install pipx` or `brew install pipx`, then `pipx ensurepath`):

```bash
pipx install platformio      # later: pipx upgrade platformio
pio --version                # check it runs
```

Plain `pip install` into the system Python is refused on most current distros
(`externally-managed-environment`); `pip install platformio` inside a venv works
too.

> If `pio` fails with `No module named 'requirements'`, an unrelated PyPI
> package named `pio` is shadowing PlatformIO's command. Remove it and restore
> PlatformIO's launcher: `pipx uninstall pio && pipx reinstall platformio`
> (in a venv: `pip uninstall -y pio && pip install --force-reinstall --no-deps platformio`).

### Download mode

Put the board in **download mode**: hold **BOOT**, tap **RESET**, release
**BOOT**, then connect the board's own USB-C port to your PC (directly, not
through the Flipper). Find its port with `pio device list`: it shows up as an
Espressif / USB serial device (e.g. `COM5` on Windows, `/dev/ttyACM0` or
`/dev/ttyUSB0` on Linux, `/dev/cu.usbmodem*` on macOS). The **Flipper is a
serial port too**, so be sure to pick the ESP32-S2.

```bash
cd firmware
pio run                                  # build only
pio run -t upload --upload-port COM5     # build and flash (use your port)
pio run -t menuconfig                    # optional: pins, baud, LED, max channel
```

Without `--upload-port` PlatformIO auto-detects, but naming the port makes sure
you never write to the Flipper. If the upload can't enter the bootloader by
itself, redo BOOT+RESET right before uploading. Tap **RESET** after flashing to
boot the new firmware.

> The defaults assume the official board: UART0 on GPIO43/44 to Flipper pins
> 13/14, an active-low RGB LED on GPIO6/5/4 (R/G/B) and channels up to 13 (set
> the max channel to 11 for US-style regions). If your clone differs, change
> them in menuconfig.

## Flipper app (ufbt)

From the repo root, `build_app.py` runs `ufbt` with the committed vendor
database bundled:

```bash
pipx install ufbt                  # or: pip install ufbt inside a venv
python3 build_app.py               # build -> flipper_app/dist/wavehound.fap
python3 build_app.py --launch      # build, upload over USB and run
python3 build_app.py --update-db   # download the latest manuf, regenerate oui.db, then build
python3 build_app.py --manuf path/to/manuf.gz  # regenerate oui.db from a local manuf.gz
python3 build_app.py --update-db --no-build    # only refresh oui.db
```

Running `ufbt` in `flipper_app/` directly also works and bundles the same
`oui.db`. Built against official firmware SDK 1.5.1 (API 88.2). To install
without `--launch`, copy the `.fap` as described in the
[README](../README.md#install). The vendor database itself is covered in
[VENDOR_DB.md](VENDOR_DB.md).
