#!/bin/bash
# Remove msi-claw-a8-steam-tdp-bridge. Must be run as root.
set -euo pipefail

PREFIX="${PREFIX:-/usr}"

if [[ $EUID -ne 0 ]]; then
    echo "error: run as root (sudo $0)" >&2
    exit 1
fi

echo ">> stopping services"
systemctl disable --now msi-claw-a8-steam-tdp-bridge.service 2>/dev/null || true
systemctl disable --now msi-claw-a8-steam-tdp-bridge-devicetoml.service 2>/dev/null || true
# Make sure the bind-mount is gone before the override file is deleted.
umount /usr/share/steamos-manager/devices/msi-claw-amd.toml 2>/dev/null || true

echo ">> removing files"
rm -f "$PREFIX/bin/msi-claw-a8-steam-tdp-bridge"
rm -f "$PREFIX/lib/systemd/system/msi-claw-a8-steam-tdp-bridge.service"
rm -f "$PREFIX/lib/systemd/system/msi-claw-a8-steam-tdp-bridge-devicetoml.service"
rm -f "$PREFIX/share/dbus-1/system.d/com.steampowered.TdpBridge.conf"
rm -f "$PREFIX/lib/systemd/system-sleep/msi-claw-a8-steam-tdp-bridge"
rm -rf "$PREFIX/lib/msi-claw-a8-steam-tdp-bridge"
rm -f /etc/steamos-manager/remotes.d/msi-claw-a8-steam-tdp-bridge.toml
rm -rf /etc/msi-claw-a8-steam-tdp-bridge
rm -rf /var/lib/msi-claw-a8-steam-tdp-bridge
rm -rf "$PREFIX/share/licenses/msi-claw-a8-steam-tdp-bridge"

systemctl daemon-reload

echo ">> restarting steamos-manager session daemons"
for uid in $(loginctl list-sessions --no-legend 2>/dev/null | awk '{print $2}' | sort -u); do
    user=$(id -nu "$uid" 2>/dev/null) || continue
    [ "$user" = "root" ] && continue
    runuser -u "$user" -- env XDG_RUNTIME_DIR="/run/user/$uid" \
        systemctl --user restart steamos-manager.service 2>/dev/null || true
done

echo ">> done (device TOML and steamos-manager are back to stock)"
