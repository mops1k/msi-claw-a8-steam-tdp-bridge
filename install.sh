#!/bin/bash
# Install steam-tdp-bridge. Must be run as root.
set -euo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${PREFIX:-/usr}"

if [[ $EUID -ne 0 ]]; then
    echo "error: run as root (sudo $0)" >&2
    exit 1
fi

echo ">> building"
meson setup "$SRC/build" --prefix="$PREFIX" --reconfigure >/dev/null
ninja -C "$SRC/build" >/dev/null
# Keep the build tree usable by the invoking user after a sudo install.
if [[ -n "${SUDO_USER:-}" && "$SUDO_USER" != "root" ]]; then
    chown -R "$SUDO_USER:" "$SRC/build" 2>/dev/null || true
fi

echo ">> installing files"
install -Dm755 "$SRC/build/steam-tdp-bridge" "$PREFIX/bin/steam-tdp-bridge"
install -Dm644 "$SRC/data/steam-tdp-bridge.service" "$PREFIX/lib/systemd/system/steam-tdp-bridge.service"
install -Dm644 "$SRC/data/steam-tdp-bridge-devicetoml.service" "$PREFIX/lib/systemd/system/steam-tdp-bridge-devicetoml.service"
install -Dm644 "$SRC/data/com.steampowered.TdpBridge.conf" "$PREFIX/share/dbus-1/system.d/com.steampowered.TdpBridge.conf"
install -Dm755 "$SRC/data/steam-tdp-bridge-sleep.sh" "$PREFIX/lib/systemd/system-sleep/steam-tdp-bridge"
install -Dm755 "$SRC/data/steam-set-profile.py" "$PREFIX/lib/steam-tdp-bridge/steam-set-profile.py"

install -Dm644 "$SRC/data/config.ini" /etc/steam-tdp-bridge/config.ini
install -Dm644 "$SRC/data/msi-claw-amd.toml" /etc/steam-tdp-bridge/msi-claw-amd.toml
install -Dm644 "$SRC/data/steam-tdp-bridge.remotes.toml" /etc/steamos-manager/remotes.d/steam-tdp-bridge.toml
install -Dm644 "$SRC/LICENSE" "$PREFIX/share/licenses/steam-tdp-bridge/LICENSE"

mkdir -p /var/lib/steam-tdp-bridge

echo ">> enabling services"
systemctl daemon-reload
systemctl enable steam-tdp-bridge-devicetoml.service steam-tdp-bridge.service
systemctl restart steam-tdp-bridge-devicetoml.service
systemctl restart steam-tdp-bridge.service

echo ">> restarting steamos-manager session daemons"
to_restart=0
for uid in $(loginctl list-sessions --no-legend 2>/dev/null | awk '{print $2}' | sort -u); do
    user=$(id -nu "$uid" 2>/dev/null) || continue
    [ "$user" = "root" ] && continue
    if runuser -u "$user" -- env XDG_RUNTIME_DIR="/run/user/$uid" \
        systemctl --user restart steamos-manager.service 2>/dev/null; then
        echo "   restarted for $user (uid $uid)"
        to_restart=1
    fi
done
if [[ $to_restart -eq 0 ]]; then
    echo "   no graphical session found; log out/in to pick up the new remote interface"
fi

echo ">> done"
echo "   steam-tdp-bridge --get         # show current TDP"
echo "   steam-tdp-bridge --apply 15    # set TDP to 15 W"
echo "   steamosctl get-tdp-limit       # via steamos-manager"
