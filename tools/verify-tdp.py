#!/usr/bin/env python3
"""Verify that the steam-tdp-bridge TDP limit is actually enforced.

Applies a list of TDP values, puts the APU under load and measures package
power two independent ways:

  * amdgpu hwmon  power1_average (label "PPT")
  * RAPL          intel-rapl:0/energy_uj

Run as root (needs to write the firmware attributes via steam-tdp-bridge).

    sudo tools/verify-tdp.py 7 20 28
"""

import multiprocessing
import os
import sys
import time

BRIDGE = "/usr/bin/steam-tdp-bridge"
SPL = "/sys/class/firmware-attributes/msi-wmi-platform/attributes/ppt_pl1_spl/current_value"
PROFILE = "/sys/class/platform-profile/platform-profile-1/profile"
LOAD_SECONDS = 15
SAMPLE_INTERVAL = 0.25


def apply_tdp(value):
    """Prefer the full steamosctl -> steamos-manager -> bridge path."""
    if os.system(f"steamosctl set-tdp-limit {value} >/dev/null 2>&1") != 0:
        os.system(f"{BRIDGE} --apply {value} >/dev/null 2>&1")


def find_hwmon_power(label):
    for entry in sorted(os.listdir("/sys/class/hwmon")):
        base = os.path.join("/sys/class/hwmon", entry)
        try:
            with open(os.path.join(base, "power1_label")) as fh:
                if fh.read().strip() == label:
                    return os.path.join(base, "power1_average")
        except OSError:
            continue
    return None


def read_int(path):
    try:
        with open(path) as fh:
            return int(fh.read().strip())
    except OSError:
        return None


def burn(stop):
    x = 1.000001
    while not stop.value:
        for _ in range(20000):
            x = x * 1.0000001 + 0.1
            if x > 1e6:
                x = 1.000001


def measure(ppt_path, rapl_path):
    stop = multiprocessing.Value("b", 0)
    workers = [multiprocessing.Process(target=burn, args=(stop,))
               for _ in range(multiprocessing.cpu_count())]
    for w in workers:
        w.start()

    samples = []
    rapl_start = read_int(rapl_path)
    t_start = time.monotonic()
    try:
        while time.monotonic() - t_start < LOAD_SECONDS:
            value = read_int(ppt_path)
            if value is not None:
                samples.append(value / 1e6)
            time.sleep(SAMPLE_INTERVAL)
    finally:
        stop.value = 1
        for w in workers:
            w.join(timeout=5)
            if w.is_alive():
                w.terminate()

    rapl_end = read_int(rapl_path)
    elapsed = time.monotonic() - t_start
    rapl_watts = None
    if rapl_start is not None and rapl_end is not None and elapsed > 0:
        rapl_watts = (rapl_end - rapl_start) / 1e6 / elapsed

    if not samples:
        return None, rapl_watts
    return (sum(samples) / len(samples), max(samples)), rapl_watts


def main():
    if os.geteuid() != 0:
        sys.exit("run as root")

    values = [int(v) for v in sys.argv[1:]] or [7, 20, 28]

    ppt_path = find_hwmon_power("PPT")
    rapl_path = "/sys/class/powercap/intel-rapl:0/energy_uj"
    if not ppt_path:
        sys.exit("no hwmon device with power1_label=PPT")
    if read_int(rapl_path) is None:
        rapl_path = None

    print(f"PPT sensor : {ppt_path}")
    print(f"RAPL sensor: {rapl_path or 'unavailable'}")
    try:
        with open(PROFILE) as fh:
            profile = fh.read().strip()
    except OSError:
        profile = "unknown"
    print(f"profile    : {profile}")
    print()
    print(f"{'TDP set':>8} | {'sysfs':>6} | {'PPT avg':>8} | {'PPT max':>8} | {'RAPL pkg':>8}")
    print("-" * 56)

    for value in values:
        apply_tdp(value)
        time.sleep(1.0)
        readback = read_int(SPL)
        (avg, peak), rapl = measure(ppt_path, rapl_path)
        avg_s = f"{avg:8.1f}" if avg is not None else "    n/a "
        peak_s = f"{peak:8.1f}" if peak is not None else "    n/a "
        rapl_s = f"{rapl:8.1f}" if rapl is not None else "    n/a "
        rb_s = f"{readback:>6}" if readback is not None else "  n/a "
        print(f"{value:>8} | {rb_s} | {avg_s} | {peak_s} | {rapl_s}")

    print()
    print("Power in W. PPT should scale with the requested TDP; hard caps mean")
    print("the limit is applied by the EC.")


if __name__ == "__main__":
    main()
