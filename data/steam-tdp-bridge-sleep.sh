#!/bin/sh
# systemd sleep hook: re-apply the saved TDP limit after resume.
# The MSI EC drops the ppt_* limits (and may reset the platform profile) on
# suspend, so restore them once the system is back.

[ "$1" = "post" ] || exit 0
exec /usr/bin/steam-tdp-bridge --restore >/dev/null 2>&1
