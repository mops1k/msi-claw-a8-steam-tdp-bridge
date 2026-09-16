#!/usr/bin/env python3
"""Hard verification of the suspend/resume path.

Sets a known TDP limit, snapshots the EC limits + platform profile, suspends the
machine, then after resume waits for the sleep hook (`--restore`) to bring the
state back and compares the two snapshots.

Run as root (writes sysfs, calls systemctl suspend):

    sudo python3 tools/verify-suspend.py --limit 15

Exits 0 when everything is restored, non-zero otherwise.
"""

import argparse
import os
import subprocess
import sys
import time

PROFILE = "/sys/class/platform-profile/platform-profile-1/profile"
ATTR_DIR = "/sys/class/firmware-attributes/msi-wmi-platform/attributes"
ATTRS = {
    "spl": "ppt_pl1_spl",
    "sppt": "ppt_pl2_sppt",
    "fppt": "ppt_pl3_fppt",
}
CLI = "/usr/bin/msi-claw-a8-steam-tdp-bridge"


def read(path):
    try:
        with open(path) as handle:
            return handle.read().strip()
    except OSError as exc:
        return f"<error: {exc}>"


def snapshot():
    snap = {"profile": read(PROFILE)}
    for key, name in ATTRS.items():
        snap[key] = read(f"{ATTR_DIR}/{name}/current_value")

    # Read our daemon directly: steamosctl needs the user session bus, which
    # root does not have.
    result = subprocess.run([
        "busctl", "--system", "get-property",
        "com.steampowered.TdpBridge", "/com/steampowered/TdpBridge",
        "com.steampowered.SteamOSManager1.TdpLimit1", "TdpLimit",
    ], capture_output=True, text=True)
    snap["daemon_tdp_limit"] = (result.stdout.strip() or result.stderr.strip()
                                or "<error>")
    return snap


def log_snapshot(label, snap):
    print(f"{label}: " + ", ".join(f"{key}={value}"
                                   for key, value in snap.items()))


def diff(before, after):
    return {key: (before[key], after[key])
            for key in before if before[key] != after[key]}


def set_limit(value):
    """Set the limit through the daemon so its in-memory value, the state file
    and sysfs all agree (fall back to the CLI when the daemon is not running)."""
    result = subprocess.run([
        "busctl", "--system", "set-property",
        "com.steampowered.TdpBridge", "/com/steampowered/TdpBridge",
        "com.steampowered.SteamOSManager1.TdpLimit1", "TdpLimit", "u", str(value),
    ], capture_output=True, text=True)
    if result.returncode != 0:
        subprocess.run([CLI, "--apply", str(value)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--limit", type=int, default=15,
                        help="TDP limit to apply before suspending (default: 15)")
    parser.add_argument("--timeout", type=float, default=30.0,
                        help="seconds to wait for restore after resume")
    args = parser.parse_args()

    if os.geteuid() != 0:
        parser.error("must run as root (sudo)")

    set_limit(args.limit)
    time.sleep(1)
    before = snapshot()
    log_snapshot("before", before)

    print("suspending; wake the machine to continue...", flush=True)
    subprocess.run(["systemctl", "suspend"], check=False)

    deadline = time.monotonic() + args.timeout
    after = snapshot()
    while time.monotonic() < deadline and diff(before, after):
        time.sleep(1)
        after = snapshot()

    log_snapshot("after ", after)

    changes = diff(before, after)
    if changes:
        print("FAIL")
        for key, (old, new) in changes.items():
            print(f"  {key}: {old} -> {new}")
        return 1

    print("PASS: EC limits and platform profile restored after resume")
    return 0


if __name__ == "__main__":
    sys.exit(main())
