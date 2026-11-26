# DisplayMOD store edition: redesign plan

Goal: a package the CardputerZero AppStore accepts. The store check (2.7.2 submission) rejects
maintainer scripts that touch sudoers, create or delete users, read `/etc/passwd`, `chmod`
sensitive files or `rm` under `/` or home, and any service that runs as root.

The original edition (sudo rule, root helper) is kept in `legacy/` in this repo. It isn't for the store, but can be built and shared on GitHub.

## Design

No sudo rule, no extra user account, no root process.

```
displaymod.service  (User=displaymod, DynamicUser=yes, extra capabilities)
  └ displaymod-daemon         listens on /run/displaymod/ctl.sock (any local user may connect)
      ├ displaymod-ctl CMD    the existing helper script, run per request; inherits the capabilities
      └ displaymod-mirror     started and stopped by the daemon on the EXT tty (no runtime unit)

DisplayMOD app, displaymod menu, BigTermCLI  -> displaymod-send CMD ...   (instead of sudo -n displaymod-ctl)
```

- **Service user:** `DynamicUser=yes` makes systemd create the `displaymod` user while the service runs, so the maintainer scripts create no account.
- **Capabilities:** `CAP_SYS_ADMIN CAP_DAC_OVERRIDE CAP_SYS_TTY_CONFIG CAP_CHOWN CAP_FOWNER` cover sysfs writes (5V, PWM, spidev, backlight), overlays (configfs), `con2fbmap` and console modes.
- **Settings:** move to `/etc/displaymod/displaymod.conf` (`ConfigurationDirectory=displaymod`). On first start the daemon copies the old `/etc/displaymod.conf` if it is there.
- **Runtime files:** move from `/run/user/UID/` to `/run/displaymod/`. That covers the BigTermCLI flag and the built-in level. The daemon writes them and the apps read them.
- **Mirror:** the daemon runs it directly on a VT it opens with `O_NOCTTY`. This replaces the `/run/systemd/system/displaymod-view.service` unit and the `systemctl` calls.
- **BigTermCLI:** it sends `bigterm on`. The daemon then sets the flag, switches the VT to text and chowns `/dev/ttyN` to the caller (peer uid from `SO_PEERCRED`). BigTermCLI starts `view.sh` on that VT itself, as the user, so `tmux attach` reaches the user's own session. On `bigterm off` the daemon takes the VT back.
- **EXT console:** tty8 (no getty on it), mapped to the EXT display. The mirror doesn't need it active. The daemon makes it the active console only while BigTermCLI is open (fbcon only draws the active one) and switches back afterwards. 3.1.0 switched at boot, which stalled the login on tty1 under the boot splash until a key was pressed. The service now starts after `plymouth-quit-wait.service`. `TTY=fg` uses the current console.
- **Launcher entries:** both `.desktop` files ship in the package. `BIGTERM=0` renames the BigTermCLI entry to `.desktop.hidden`, and the launcher only reads `*.desktop`. The unit grants `ReadWritePaths=/usr/share/APPLaunch/applications` for that. `prerm` renames it back so dpkg removes or updates it. The setting stays in the app and the text menu.
- **Uninstall:** the app's uninstall page now shows `sudo apt remove displaymod`. The text menu shows the same and offers to run it, since it has a terminal for the sudo password.
- **Overlays:** use `dtoverlay` if it allows a non-root caller. If not, use `dtmerge` with parameters to make a dtbo in `/run/displaymod` and load it through configfs.
- **Security check:** every request comes from a local user over the socket, and ctl's `valid()` checks each value as it does now. Only fixed commands are accepted. Nothing is passed to a shell.

Maintainer scripts stay limited to `systemctl daemon-reload / enable / restart / stop / disable`.

Upgrading from 2.7.x leaves the old sudo rule and the `displaymod` system user in place, because the store doesn't allow the scripts to remove them. The README gives the one-time commands to remove them.

## Phases

0. **Feasibility (done 2026-11-23):** everything passed except `dtoverlay`, which refuses non-root callers. configfs and `dtmerge` work. (Since 3.1.13 both overlays are built with `dtc`, as Gh0st OS has no `dtmerge`.)
   Original step: run `tools/captest.sh` on the device as a dynamic user with these capabilities, and settle the open questions from its output.
**Phases 1-3 done 2026-11-23** (3.0.0 built, 0 store path violations, no flagged commands in the scripts). Not device-tested.

1. **Daemon and client:** `src/daemon/displaymod-daemon.c` and `displaymod-send`. Point the app, the text menu and BigTermCLI at it. Adapt ctl (no systemctl, no chown, no sudoers).
2. **Mirror and BigTermCLI hand-off:** move the runtime paths, run the mirror under the daemon, start the viewer from BigTermCLI.
3. **Clean-up and sync:** remove BIGTERM toggling and app uninstall from the app, the text menu and ctl together, and update the docs.
4. **Device test, then store submission.**

## Feasibility test

From cmd on the PC (put in your IP):

```
scp tools\captest.sh wade-cpz@IP:/tmp/
ssh -t wade-cpz@IP "sudo systemd-run --wait --pipe --collect --quiet -p DynamicUser=yes -p User=displaymod -p SupplementaryGroups='video input tty' -p AmbientCapabilities='CAP_SYS_ADMIN CAP_DAC_OVERRIDE CAP_SYS_TTY_CONFIG CAP_CHOWN CAP_FOWNER' -p RuntimeDirectory=displaymod-captest -p BindReadOnlyPaths=/tmp/captest.sh:/run/captest.sh /bin/sh /run/captest.sh"
```

`sudo` here is only for running the test. The store package itself won't use it.
