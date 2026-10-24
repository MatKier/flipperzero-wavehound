"""PlatformIO pre-script: keep the app version (INFO's `Commit`) current.

ESP-IDF stamps `git describe --always --tags --dirty` into the app descriptor
when CMake configures the project. Under idf.py, ninja re-runs CMake when git
HEAD moves, but PlatformIO only re-runs it when a CMakeLists.txt or sdkconfig
changes, so an incremental `pio run` kept reporting the describe string of
whatever commit (and dirty state) the build dir was last configured at.

When the current describe no longer matches the one the last configure wrote
into project_description.json, drop build.ninja: PlatformIO then reconfigures,
and only esp_app_desc.c recompiles because nothing else changed.
"""

import json
import os
import subprocess

Import("env")  # noqa: F821 (provided by PlatformIO/SCons)


def git_describe(repo_dir):
    # Same arguments as ESP-IDF's git_describe(), so a clean match is stable.
    try:
        out = subprocess.run(
            ["git", "-C", repo_dir, "describe", "--always", "--tags", "--dirty"],
            capture_output=True, text=True, check=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return None
    return out.stdout.strip() or None


def configured_version(build_dir):
    try:
        with open(os.path.join(build_dir, "project_description.json")) as f:
            return json.load(f).get("project_version")
    except (OSError, ValueError):
        return None


build_dir = env.subst("$BUILD_DIR")  # noqa: F821
ninja = os.path.join(build_dir, "build.ninja")
current = git_describe(env.subst("$PROJECT_DIR"))  # noqa: F821

# No build dir yet (fresh checkout, CI) or no git: nothing to refresh.
if current and os.path.isfile(ninja):
    configured = configured_version(build_dir)
    if configured != current:
        print("Wavehound: version %s -> %s, reconfiguring" % (configured, current))
        os.remove(ninja)
