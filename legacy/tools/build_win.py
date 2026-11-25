#!/usr/bin/env python3
"""Clean build of DisplayMOD on Windows (or any OS) without gcc, make or dpkg-deb.

    pip install --user ziglang
    python tools/build_win.py      -> dist/displaymod_<ver>_arm64.deb

Same steps as build.sh, using zig as the arm64 cross compiler (static,
musl). Every run starts from scratch: the build folder and zig's cache are
deleted first, so nothing from an earlier build is reused. LVGL 9.2.2 is
cloned into src/ui/lvgl the first time.
"""

import gzip
import io
import os
import shutil
import subprocess
import sys
import tarfile
import time
from concurrent.futures import ThreadPoolExecutor

import ziglang

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UI = os.path.join(ROOT, "src", "ui")
BUILD = os.path.join(ROOT, "build", "win")
PKG = os.path.join(ROOT, "pkg")
LVGL_TAG = "v9.2.2"

ZIG = os.path.join(os.path.dirname(ziglang.__file__), "zig.exe" if os.name == "nt" else "zig")
TARGET = ["-target", "aarch64-linux-musl"]
CFLAGS = ["-O2", "-Wall"]

BINARIES = {
    "usr/lib/displaymod/displaymod-mirror": "src/mirror/displaymod-mirror.c",
    "usr/lib/displaymod/displaymod-probe": "src/probe/displaymod-probe.c",
    "usr/bin/displaymod-term": "src/term/displaymod-term.c",
}
UI_BIN = "usr/bin/displaymod-ui"
UI_SRC = ["ui.c", "backend_ctl.c", "main_dev.c"]


def run(cmd, cwd=ROOT):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write("FAILED: %s\n%s%s" % (" ".join(cmd), r.stdout, r.stderr))
        raise SystemExit(1)
    if r.stderr.strip():
        sys.stderr.write(r.stderr)


def version():
    with open(os.path.join(PKG, "DEBIAN", "control"), encoding="utf-8") as fh:
        for line in fh:
            if line.startswith("Version:"):
                return line.split(":", 1)[1].strip()
    raise SystemExit("no Version in pkg/DEBIAN/control")


def _force_remove(func, path, _exc):
    os.chmod(path, 0o666)      # zig marks some cache files read-only
    func(path)


def fresh():
    if os.path.exists(BUILD):
        shutil.rmtree(BUILD, onerror=_force_remove)
    if os.path.exists(BUILD):
        raise SystemExit("could not delete %s; close anything using it and run again" % BUILD)
    os.makedirs(BUILD)
    cache = os.path.join(BUILD, "zig-cache")
    os.environ["ZIG_GLOBAL_CACHE_DIR"] = cache
    os.environ["ZIG_LOCAL_CACHE_DIR"] = cache
    for rel in list(BINARIES) + [UI_BIN]:
        p = os.path.join(PKG, rel)
        if os.path.exists(p):
            os.remove(p)


def lvgl():
    d = os.path.join(UI, "lvgl")
    if not os.path.isdir(d):
        run(["git", "clone", "--depth", "1", "-b", LVGL_TAG, "https://github.com/lvgl/lvgl.git", d])
    tag = subprocess.run(["git", "-C", d, "describe", "--tags"], capture_output=True, text=True).stdout.strip()
    if tag != LVGL_TAG:
        raise SystemExit("src/ui/lvgl is %r, expected %s; delete it and run again" % (tag, LVGL_TAG))
    srcs = []
    for base, _, files in os.walk(os.path.join(d, "src")):
        srcs += [os.path.join(base, f) for f in files if f.endswith(".c")]
    return sorted(srcs)


def build_tools():
    for rel, src in BINARIES.items():
        out = os.path.join(PKG, rel)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        run([ZIG, "cc"] + TARGET + CFLAGS + ["-static", "-s", "-o", out, src])
        print("built", rel)


def build_ui(ver):
    flags = TARGET + CFLAGS + ["-I.", "-Ilvgl", "-DLV_CONF_INCLUDE_SIMPLE", "-Wno-unused-parameter",
                               '-DDM_VERSION="%s"' % ver, "-DLV_USE_LINUX_FBDEV=1"]
    srcs = lvgl() + [os.path.join(UI, f) for f in UI_SRC]
    objdir = os.path.join(BUILD, "ui")

    def obj(src):
        rel = os.path.relpath(src, UI)
        o = os.path.join(objdir, os.path.splitext(rel)[0] + ".o")
        os.makedirs(os.path.dirname(o), exist_ok=True)
        run([ZIG, "cc"] + flags + ["-c", src, "-o", o], cwd=UI)
        return o

    t = time.time()
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        objs = list(ex.map(obj, srcs))
    out = os.path.join(PKG, UI_BIN)
    rsp = os.path.join(BUILD, "ui-objs.rsp")
    with open(rsp, "w") as fh:
        fh.write("\n".join('"%s"' % o.replace("\\", "/") for o in objs))
    run([ZIG, "cc"] + TARGET + ["-static", "-s", "-o", out, "@" + rsp, "-lm"])
    print("built %s (%d files, %.0f s)" % (UI_BIN, len(srcs), time.time() - t))


# ------------------------------------------------------------------ .deb

def _info(name, mode, size=0, kind=tarfile.REGTYPE, mtime=0):
    ti = tarfile.TarInfo(name)
    ti.type, ti.mode, ti.size, ti.mtime = kind, mode, size, mtime
    ti.uid = ti.gid = 0
    ti.uname = ti.gname = "root"
    return ti


def _targz(root, mtime, only=None):
    raw = io.BytesIO()
    total = 0
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.GNU_FORMAT) as tar:
        tar.addfile(_info("./", 0o755, kind=tarfile.DIRTYPE, mtime=mtime))
        for base, dirs, files in os.walk(root):
            dirs.sort()
            rel_base = os.path.relpath(base, root).replace("\\", "/")
            if rel_base != ".":
                tar.addfile(_info("./%s/" % rel_base, 0o755, kind=tarfile.DIRTYPE, mtime=mtime))
            for f in sorted(files):
                rel = f if rel_base == "." else rel_base + "/" + f
                if only and rel not in only:
                    continue
                with open(os.path.join(base, f), "rb") as fh:
                    data = fh.read()
                binary = data.startswith(b"\x7fELF") or f.endswith(".png")
                if not binary:
                    data = data.replace(b"\r\n", b"\n")
                exe = data.startswith((b"\x7fELF", b"#!")) or (only and rel != "control")
                tar.addfile(_info("./" + rel, 0o755 if exe else 0o644, len(data), mtime=mtime),
                            io.BytesIO(data))
                total += len(data)
    out = io.BytesIO()
    with gzip.GzipFile(fileobj=out, mode="wb", mtime=mtime) as gz:
        gz.write(raw.getvalue())
    return out.getvalue(), total


def _ar(members, mtime):
    out = io.BytesIO()
    out.write(b"!<arch>\n")
    for name, data in members:
        out.write(("%-16s%-12d%-6d%-6d%-8s%-10d`\n" % (name, mtime, 0, 0, "100644", len(data))).encode())
        out.write(data)
        if len(data) % 2:
            out.write(b"\n")
    return out.getvalue()


def pack(ver):
    mtime = int(os.environ.get("SOURCE_DATE_EPOCH") or time.time())   # set in CI: the commit time
    data_root = os.path.join(BUILD, "data")
    shutil.copytree(PKG, data_root, ignore=shutil.ignore_patterns("DEBIAN"))
    data_tgz, _ = _targz(data_root, mtime)
    ctl = os.path.join(PKG, "DEBIAN")
    control_tgz, _ = _targz(ctl, mtime, only={"control", "postinst", "prerm", "postrm", "preinst"} &
                            set(os.listdir(ctl)))
    deb = _ar([("debian-binary", b"2.0\n"), ("control.tar.gz", control_tgz),
               ("data.tar.gz", data_tgz)], mtime)
    os.makedirs(os.path.join(ROOT, "dist"), exist_ok=True)
    out = os.path.join(ROOT, "dist", "displaymod_%s_arm64.deb" % ver)
    with open(out, "wb") as fh:
        fh.write(deb)
    print("%s (%d KB)" % (out, len(deb) // 1024))


def main():
    ver = version()
    print("DisplayMOD %s, clean build with zig %s" % (ver, subprocess.run(
        [ZIG, "version"], capture_output=True, text=True).stdout.strip()))
    fresh()
    build_tools()
    build_ui(ver)
    pack(ver)
    return 0


if __name__ == "__main__":
    sys.exit(main())
