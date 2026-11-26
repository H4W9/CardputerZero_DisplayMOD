#!/bin/sh
# Checks which DisplayMOD hardware steps work for a non-root service user with
# capabilities (the store redesign). Run it through systemd-run as shown in
# DESIGN-STORE.md. It only reads values and writes back what is already there;
# the one change it makes is an empty configfs overlay folder that it removes.

ok()  { echo "PASS  $*"; }
bad() { echo "FAIL  $*"; }
try() { # label command...
  l=$1; shift
  if out=$("$@" 2>&1); then ok "$l"; else bad "$l: $(echo "$out" | head -n 1)"; fi
}
rewrite() { # label file: write the current value back
  f=$2
  if [ ! -e "$f" ]; then bad "$1: $f missing"; return; fi
  v=$(cat "$f" 2>/dev/null) || { bad "$1: cannot read $f"; return; }
  if (echo "$v" > "$f") 2>/dev/null; then ok "$1 ($f = $v)"; else bad "$1: cannot write $f"; fi
}

echo "== who"
id
grep -E '^Cap(Eff|Amb)' /proc/self/status

echo "== sysfs"
rewrite "5V output" /sys/class/leds/ext_5v_out/brightness
rewrite "built-in backlight" /sys/class/backlight/backlight/brightness
for c in /sys/class/pwm/pwmchip*; do
  [ -e "$c/export" ] && { [ -w "$c/export" ] && ok "pwm export writable ($c)" || bad "pwm export ($c)"; }
done
[ -w /sys/bus/spi/drivers/spidev/unbind ] && ok "spidev unbind writable" || bad "spidev unbind not writable"
[ -w /sys/class/graphics/fbcon/cursor_blink ] && ok "fbcon cursor_blink writable" || bad "fbcon cursor_blink"

echo "== device tree overlays"
try "dtoverlay -l" dtoverlay -l
out=$(dtoverlay displaymod-captest-none 2>&1); echo "      dtoverlay as this user says: $(echo "$out" | head -n 1)"
command -v dtc >/dev/null && ok "dtc present" || bad "dtc missing (device-tree-compiler)"
OV=/sys/kernel/config/device-tree/overlays
if [ -d "$OV" ]; then
  if mkdir "$OV/displaymod-captest" 2>/dev/null; then
    rmdir "$OV/displaymod-captest" && ok "configfs overlay mkdir/rmdir" || bad "configfs rmdir"
  else
    bad "configfs overlay mkdir"
  fi
else
  bad "$OV missing"
fi

echo "== console / tty"
fb_now=$(con2fbmap 1 2>/dev/null | sed -n 's/.*framebuffer \([0-9]*\).*/\1/p')
if [ -n "$fb_now" ]; then try "con2fbmap set (tty1 -> fb$fb_now, unchanged)" con2fbmap 1 "$fb_now"
else bad "con2fbmap read: $(con2fbmap 1 2>&1 | head -n 1)"; fi
try "KDSETMODE text on tty8" /usr/lib/displaymod/displaymod-probe vt 8 text
try "fgconsole" fgconsole
if [ -e /dev/tty8 ]; then
  o=$(stat -c %u:%g /dev/tty8); m=$(stat -c %a /dev/tty8)
  if chown "$(id -u)" /dev/tty8 2>/dev/null; then chown "$o" /dev/tty8; chmod "$m" /dev/tty8; ok "chown /dev/tty8 (restored $o $m)"
  else bad "chown /dev/tty8"; fi
fi

echo "== devices"
for f in /dev/fb0 /dev/fb1 /dev/spidev0.1; do
  [ -e "$f" ] || { echo "      $f not present"; continue; }
  [ -r "$f" ] && [ -w "$f" ] && ok "rw $f" || bad "rw $f ($(ls -l "$f"))"
done
for f in /dev/input/by-path/*i2c-event /dev/input/event0; do
  [ -e "$f" ] && { [ -r "$f" ] && ok "read $f" || bad "read $f"; break; }
done

echo "== files"
[ -w /usr/share/APPLaunch/applications ] && ok "APPLaunch applications writable" || bad "APPLaunch applications not writable (expected with ProtectSystem)"
[ -d /run/displaymod-captest ] && [ -w /run/displaymod-captest ] && ok "RuntimeDirectory writable" || bad "RuntimeDirectory"
echo "== done"
