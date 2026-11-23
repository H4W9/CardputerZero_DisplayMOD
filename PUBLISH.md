# Application Store Publish Record

Portal: <https://dev.cardputer.cc/#/upload>

## Package

- Debian package: `dist/displaymod_3.1.3_arm64.deb` (built with `python tools/build_win.py`)
- Package: `displaymod`
- Version: `3.1.3`
- Architecture: `arm64` (static native binaries)
- Maintainer: `H4W9 <H4W9@users.noreply.github.com>` (GitHub noreply, no personal email)
- Depends: `tmux, fbset, kbd, device-tree-compiler`
- Launcher entries: `/usr/share/APPLaunch/applications/displaymod.desktop` (DisplayMOD) and `displaymod-bigterm.desktop` (BigTermCLI, added by the app when enabled)
- Executables: `/usr/bin/displaymod`, `/usr/bin/displaymod-ui`, `/usr/bin/displaymod-term`
- Service: `displaymod-daemon` runs `/usr/lib/displaymod/displaymod-ctl`; apps use `/usr/lib/displaymod/displaymod-send` (no sudo rule)
- Boot service: `/usr/lib/systemd/system/displaymod.service`, `User=displaymod` + `DynamicUser=yes`, capabilities SYS_ADMIN, DAC_OVERRIDE, SYS_TTY_CONFIG, CHOWN, FOWNER

## Source and ownership

- Source repository: https://github.com/H4W9/CardputerZero_DisplayMOD
- Uploader / author: H4W9
- License: MIT
- Hide uploader email: true

## Listing

- Name: DisplayMOD
- Summary: Use a 2.8 inch SPI display as a second screen, with a full-size terminal
- Category: Tools / Utilities (system add-on)
- Share code: DMOD (not checked against the store index yet)

Description:

```
Adds a 2.8" ILI9341 SPI display (240x320) to the EXT header and mirrors the built-in screen on it.

- 1:1 mirror with a status bar (clock, battery, IP, RAM, CPU) in the spare rows, or stretch to fill
- BigTermCLI: a full-size terminal on the EXT display, with the same keys and colors as the CLI app and saved command history
- Brightness follows the built-in screen, or set it from 10 to 100%
- Can keep the built-in screen off while mirroring, with a sleep timer
- Checks for the display at boot and leaves the 5V output off if nothing is connected

Settings are in the DisplayMOD app or the displaymod command over SSH. Wiring is shown in the app under Wiring help.

Tested with the red "2.8 TFT SPI 240x320 V1.2" board. Touch and SD on that board are not used.

A service loads the display driver. It runs as a systemd dynamic user with a few added capabilities (not root), and the apps send it settings over a local socket. No sudo rule, no extra user account.
```

## Permissions

- Camera: no
- Microphone: no
- IMU: no
- Network: no (the status bar only reads the local IP)
- Additional hardware: yes (2.8" ILI9341 SPI display on the EXT header)
- Background service: yes (`displaymod.service` at boot, dynamic non-root user `displaymod`; it also runs the mirror)
- External display: yes

## Assets

- Store icon 256 x 256: `store/icon.png` (drawn by `python tools/make_store_icon.py`, same design as the launcher icon)
- Launcher icon in the .deb: `pkg/usr/share/APPLaunch/share/images/displaymod_100.png`
- Screenshots (320 x 170, scaled from the 640 x 340 originals):
  1. `store/screenshots/home.png`
  2. `store/screenshots/queued.png`
  3. `store/screenshots/ext_settings.png`
  4. `store/screenshots/history.png`
  5. `store/screenshots/status.png`

## Preflight

- [x] `.deb` exists and parses; Package, Version, Architecture, Maintainer present
- [x] APPLaunch `.desktop`, executable and PNG icon inside the package
- [x] No credentials or private details embedded
- [x] Maintainer has an email address
- [x] 256 x 256 store icon
- [ ] Share code free in the store index
- [ ] 3.1.3 tested on a real CardputerZero
- [ ] Owner approved submission

## Submission result

- Status: not submitted
