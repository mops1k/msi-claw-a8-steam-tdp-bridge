# steam-tdp-bridge

[![Release](https://img.shields.io/github/v/release/mops1k/msi-claw-a8-steam-tdp-bridge)](https://github.com/mops1k/msi-claw-a8-steam-tdp-bridge/releases)
[![License](https://img.shields.io/github/license/mops1k/msi-claw-a8-steam-tdp-bridge)](LICENSE)

TDP bridge for the **MSI Claw A8 (Ryzen Z2 Extreme)** on SteamOS-compatible
distributions (tested on CachyOS `deckify`). It lets you change the device TDP
through **Steam's standard power management UI** in Game Mode
(QAM → Performance → TDP Limit) — no Decky plugins required.

> Русская версия: [README.ru.md](README.ru.md).

## Why

`steamos-manager` exposes a `com.steampowered.SteamOSManager1.TdpLimit1` D-Bus
interface to Steam. On this device the stock device TOML declares a *local*
TDP method that is only active in the `performance` platform profile, so the
interface is never published and the QAM slider does nothing. The MSI EC also
only enforces the `ppt_*` firmware limits while the platform profile is
`performance`.

This project supplies the missing piece: a small root daemon that implements
`TdpLimit1` as a *remote* interface for `steamos-manager`, writes the real
limits to sysfs, and keeps the platform profile in sync with Steam's TDP
checkbox.

## Features

- Native QAM TDP slider — no Decky, no patched Steam.
- Three package power limits mapped like `steamos-manager`
  (`ppt_pl1_spl`, `ppt_pl2_sppt`, `ppt_pl3_fppt`).
- Follows Steam's **TDP Limit** checkbox: forces `performance` only while it is
  enabled and releases the profile when it is turned off.
- Keeps Steam's profile picker in sync (via the Steam client itself).
- Restores the limit and profile after **suspend/resume**.
- Never blocks or interferes with Game Mode startup (see
  [Notes on the black-screen fix](#notes-on-the-black-screen-fix)).

## How it works

```
Steam QAM (TdpLimit)
   │ session bus
   ▼
com.steampowered.SteamOSManager1.TdpLimit1   (steamos-manager user daemon)
   │ remote interface relay (system bus)
   ▼
com.steampowered.TdpBridge  →  steam-tdp-bridge (root, C+GIO)
   │  │ sysfs write
   │  ▼
   │ /sys/class/firmware-attributes/.../ppt_*/current_value
   │
   └─ profile shown in the UI: helper steam-set-profile.py
      (webhelper debug port 127.0.0.1:8080; only when the profile actually changes)
```

- `steamos-manager` can delegate `TdpLimit1` to an external process (remote
  interface). The installer bind-mounts a copy of `msi-claw-amd.toml` with the
  local `[tdp_limit]` section removed, so `steamos-manager` switches to its
  `RemoteInterfaceLimitManager` and relays calls to this daemon.
- The daemon writes `ppt_pl1_spl` (SPL = requested value), `ppt_pl2_sppt` and
  `ppt_pl3_fppt` (never below their minimums).
- **The EC only enforces those limits in the `performance` profile.**
- The daemon reads Steam's **TDP Limit** checkbox directly from
  `config.vdf` (`SteamOS → TDPLimitEnabled`), **without talking to Steam** —
  safe during startup. A `GFileMonitor` on the file catches checkbox changes
  even when Steam sends the limit before persisting the file.
  - checkbox **on** → force and keep `profile_name` (`performance`);
  - checkbox **off** → give the previous profile back and stop touching it.
- To keep the QAM profile picker consistent, the daemon changes the profile
  **through the Steam client** (`SteamClient.Settings.SetSetting`, helper
  `steam-set-profile.py`), but only when the target profile differs from the
  current one. Limits are always written to sysfs.
- **Suspend:** a systemd sleep hook (`--restore`) reapplies the last value and
  re-asserts the profile after resume.

### Notes on the black-screen fix

Poking the Steam webhelper (scanning webpack modules + `SetSetting`) in the hot
path during Game Mode startup caused a black screen. The interaction is now kept
out of that path: the checkbox is read from `config.vdf`, `apply_profile()`
becomes a no-op when the profile already matches, the helper runs fire-and-forget
(`g_spawn_async`), and profile re-assertion on drift uses sysfs only.

## Requirements

- `steamos-manager` with remote interface support (SteamOS 3.x / `deckify`).
- `glib2` / `gio-2.0`, `dbus`.
- MSI Claw A8 (`msi-wmi-platform` firmware attributes + platform-profile).

## Installation

From a release (recommended):

```sh
sudo pacman -U steam-tdp-bridge-<version>-1-x86_64.pkg.tar.zst
```

From source:

```sh
sudo ./install.sh          # from the repository
# or
makepkg -si                # from the directory containing PKGBUILD
```

The two methods install the same files — do not stack one on top of the other.
Switch between them with `sudo ./uninstall.sh` first.

Then log out and back into Game Mode (the installer also restarts
`steamos-manager` for active sessions).

## Usage

Once installed, use the normal Steam controls:

- **QAM → Performance → TDP Limit** — drag the slider; the value is applied
  immediately and persisted.
- The **Performance Profile** picker will show `performance` while the TDP
  limit is enabled, and free again once it is disabled.

Command-line checks:

```sh
steam-tdp-bridge --get         # current value (W)
steam-tdp-bridge --apply 15    # set TDP to 15 W
steamosctl get-tdp-limit       # via steamos-manager
```

## Configuration

`/etc/steam-tdp-bridge/config.ini`:

| key                 | meaning                                                          |
|---------------------|------------------------------------------------------------------|
| `device`            | firmware-attributes device (`msi-wmi-platform`)                  |
| `attribute_spl/sppt/fppt` | PL1/PL2/PL3 attribute names                               |
| `platform_profile`  | platform-profile provider (`msi-wmi-platform`)                   |
| `profile_policy`    | `always` (default) / `auto` / `never` — keep the profile or not  |
| `profile_name`      | profile used for TDP (default `performance`)                     |
| `honor_steam_toggle`| `true` (default) — follow Steam's TDP Limit checkbox             |
| `enforce_profile`   | fallback when the checkbox cannot be read: `true` keeps `profile_name` |
| `steam_config`      | path to `config.vdf`; empty = auto-detect                       |
| `restore_last`      | restore the last value on daemon start                           |
| `default_limit`     | value used when there is no saved state and the EC reads `0`     |
| `state_path`        | state file (default `/var/lib/steam-tdp-bridge/tdp`)             |

## Verification tools

```sh
sudo tools/verify-tdp.py 7 20 28        # load all cores and measure PPT + RAPL
sudo python3 tools/verify-suspend.py --limit 18   # suspend/resume round-trip
```

`verify-tdp.py` is expected to show ~9–10 W at a 7 W limit (SPPT/FPPT boost) and
~18–20 W at 20 W; `verify-suspend.py` prints `PASS` when the profile and all
three limits are restored after resume.

## Diagnostics

```sh
systemctl status steam-tdp-bridge
busctl --system introspect com.steampowered.TdpBridge /com/steampowered/TdpBridge
busctl --user introspect com.steampowered.SteamOSManager1 /com/steampowered/SteamOSManager1 | grep Tdp
cat /sys/class/firmware-attributes/msi-wmi-platform/attributes/ppt_pl1_spl/current_value
```

If `TdpLimit1` is missing from `steamos-manager`:

- check the bind-mount: `mount | grep msi-claw-amd.toml`;
- check that `/etc/steam-tdp-bridge/msi-claw-amd.toml` has no `[tdp_limit]`;
- restart the session daemon: `systemctl --user restart steamos-manager`.

## Updating the device TOML

`/usr/share/steamos-manager/devices/msi-claw-amd.toml` can change with
`steamos-manager` updates. After a major upgrade, compare it with
`/etc/steam-tdp-bridge/msi-claw-amd.toml` and port the changes (except the
removed `[tdp_limit]`).

## Uninstall

```sh
sudo ./uninstall.sh
```

Removes the services (unmounting the override), files and `remotes.d` entry,
then restarts `steamos-manager` — the stock device TOML is restored.

## Releases

The [`Release`](.github/workflows/release.yml) workflow is triggered manually
(`Actions → Release → Run workflow`). It bumps `pkgver`/`pkgrel` in `PKGBUILD`
and the version in `meson.build`, commits and tags it, builds the Arch package
and a standalone binary in an `archlinux` container, and publishes a GitHub
release with both artifacts attached.

Requires `Settings → Actions → General → Workflow permissions` set to
**Read and write permissions** (the workflow pushes the version commit/tag and
creates the release).

## License

[MIT](LICENSE).
