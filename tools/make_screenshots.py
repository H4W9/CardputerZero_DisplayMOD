#!/usr/bin/env python3
"""Render the DisplayMOD app's screenshots with the host test harness.

    pip install --user ziglang pillow
    python tools/make_screenshots.py

Builds src/ui/main_test.c + backend_fake.c + LVGL for this PC with zig
(fresh every run), walks the app with scripted keys and writes:
    screenshots/app_*.png          2x, for the README
    store/screenshots/*.png        320x170, for the app store listing
The fake backend's settings and status match displaymod-ctl's output.
"""

import os
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

import ziglang
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UI = os.path.join(ROOT, "src", "ui")
BUILD = os.path.join(ROOT, "build", "shots")
ZIG = os.path.join(os.path.dirname(ziglang.__file__), "zig.exe" if os.name == "nt" else "zig")
EXE = os.path.join(BUILD, "harness.exe" if os.name == "nt" else "harness")

# name: (script, extra env). Keys: U D L R E X A, W = wait, snap:NAME.
# Home rows (status bar layout): Mirror, BigTermCLI, Layout, Built-in off,
# Built-in dark, History, EXT display settings, Status, Wiring, About.
D = lambda n: " ".join(["D"] * n)
SHOTS = {
    "home_not_detected": ("snap:home_not_detected", {"DM_FAKE_PANEL": "missing"}),
    "queued": ("D D R R snap:queued", {}),                    # layout -> 1:1, no bar (queued)
    "ext_settings": (D(6) + " E snap:ext_settings", {}),
    "history": (D(5) + " E snap:history", {}),
    "history_list": (D(5) + " E D D D E D snap:history_list", {}),
    "history_delete": (D(5) + " E D D D E D E snap:history_delete", {}),
    "history_clear": (D(5) + " E D D D D E snap:history_clear", {}),
    "status": (D(7) + " E snap:status", {}),
    "about": (D(9) + " E snap:about", {}),
    "home": ("snap:home", {"DM_EXT_SIZE": "320x240"}),         # also writes home_ext
}
README_SET = ["home", "home_ext", "home_not_detected", "queued", "ext_settings",
              "history", "history_list", "history_delete", "history_clear", "status", "about"]
STORE_SET = ["home", "queued", "ext_settings", "history", "status"]


def run(cmd, cwd=ROOT):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("FAILED: %s\n%s%s" % (" ".join(cmd), r.stdout, r.stderr))
    return r.stdout


def version():
    with open(os.path.join(ROOT, "pkg", "DEBIAN", "control"), encoding="utf-8") as fh:
        for line in fh:
            if line.startswith("Version:"):
                return line.split(":", 1)[1].strip()
    return "dev"


def build():
    shutil.rmtree(BUILD, ignore_errors=True)   # OneDrive may hold a file: rebuild over what's left
    os.makedirs(BUILD, exist_ok=True)
    os.environ["ZIG_GLOBAL_CACHE_DIR"] = os.environ["ZIG_LOCAL_CACHE_DIR"] = os.path.join(BUILD, "zig-cache")
    lvgl = os.path.join(UI, "lvgl", "src")
    if not os.path.isdir(lvgl):
        sys.exit("src/ui/lvgl is missing: run tools/build_win.py once first")
    srcs = [os.path.join(b, f) for b, _, fs in os.walk(lvgl) for f in fs if f.endswith(".c")]
    srcs += [os.path.join(UI, f) for f in ("ui.c", "backend_fake.c", "main_test.c")]
    flags = ["-O1", "-I.", "-Ilvgl", "-DLV_CONF_INCLUDE_SIMPLE", "-DLV_USE_LINUX_FBDEV=0",
             '-DDM_VERSION="%s"' % version(), "-w"]

    def obj(src):
        o = os.path.join(BUILD, "obj", os.path.splitext(os.path.relpath(src, UI))[0] + ".o")
        os.makedirs(os.path.dirname(o), exist_ok=True)
        run([ZIG, "cc"] + flags + ["-c", src, "-o", o], cwd=UI)
        return o

    t = time.time()
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        objs = list(ex.map(obj, srcs))
    rsp = os.path.join(BUILD, "objs.rsp")
    with open(rsp, "w") as fh:
        fh.write("\n".join('"%s"' % o.replace("\\", "/") for o in objs))
    run([ZIG, "cc", "-o", EXE, "@" + rsp])
    print("harness built (%d files, %.0f s)" % (len(srcs), time.time() - t))


def shoot():
    raw = os.path.join(BUILD, "raw")
    os.makedirs(raw, exist_ok=True)
    for script, env in SHOTS.values():
        e = dict(os.environ)
        e.update(env)
        r = subprocess.run([EXE, raw, script], cwd=ROOT, env=e, capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit("harness failed on %r\n%s%s" % (script, r.stdout, r.stderr))

    shots = os.path.join(ROOT, "screenshots")
    store = os.path.join(ROOT, "store", "screenshots")
    for d in (shots, store):                  # replace the old images, keep the folders
        os.makedirs(d, exist_ok=True)
        for f in os.listdir(d):
            if f.endswith(".png"):
                os.remove(os.path.join(d, f))
    for name in README_SET:
        im = Image.open(os.path.join(raw, name + ".ppm")).convert("RGB")
        im.resize((im.width * 2, im.height * 2), Image.LANCZOS).save(os.path.join(shots, "app_%s.png" % name))
    for name in STORE_SET:
        Image.open(os.path.join(raw, name + ".ppm")).convert("RGB").save(os.path.join(store, name + ".png"))
    print("wrote %d README shots to screenshots/, %d store shots to store/screenshots/"
          % (len(README_SET), len(STORE_SET)))


if __name__ == "__main__":
    build()
    shoot()
