#!/bin/sh
# Build DisplayMOD for the CardputerZero (arm64) and package the .deb.
# Needs: aarch64-linux-gnu-gcc, make, git, dpkg-deb
# (python3-pil and fonts-dejavu only if you regenerate src/common/font_dm.h)
set -e
cd "$(dirname "$0")"

CC="aarch64-linux-gnu-gcc -O2 -Wall -static -s"
[ -d src/ui/lvgl ] || git clone --depth 1 -b v9.2.2 https://github.com/lvgl/lvgl.git src/ui/lvgl

mkdir -p pkg/usr/bin pkg/usr/lib/displaymod
$CC -o pkg/usr/lib/displaymod/displaymod-mirror src/mirror/displaymod-mirror.c
$CC -o pkg/usr/lib/displaymod/displaymod-probe  src/probe/displaymod-probe.c
$CC -o pkg/usr/bin/displaymod-term              src/term/displaymod-term.c
$CC -o pkg/usr/lib/displaymod/displaymod-daemon src/daemon/displaymod-daemon.c
$CC -o pkg/usr/lib/displaymod/displaymod-send   src/daemon/displaymod-send.c
make -C src/ui -j"$(nproc)"
install -m 0755 src/ui/build/arm64/displaymod-ui pkg/usr/bin/displaymod-ui

V=$(sed -n 's/^Version: //p' pkg/DEBIAN/control)
dpkg-deb --root-owner-group --build pkg "displaymod_${V}_arm64.deb"
