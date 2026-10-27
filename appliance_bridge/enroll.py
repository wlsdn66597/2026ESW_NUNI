"""Run on Pi4 with nunisensors stopped. Identify frames before pinning USB ports."""
import argparse
import glob
import json
import os
from pathlib import Path
import time
import serial
from rule import ROLES


def discover():
    found = {}
    used = set()
    for path in sorted(glob.glob("/dev/serial/by-path/*")):
        real = os.path.realpath(path)
        if real in used: continue
        used.add(real)
        if not ("ttyUSB" in real or "ttyACM" in real): continue
        port = serial.Serial()
        port.port, port.baudrate, port.timeout = path, 115200, .2
        port.dtr = port.rts = False; port.exclusive = True
        try:
            port.open()
            end = time.monotonic() + 10
            while time.monotonic() < end:
                line = port.readline(1024)
                if line.startswith(b"NUNIA1 "):
                    data = json.loads(line[7:])
                    role = data.get("role")
                    if role in ROLES and isinstance(data.get("device_id"), str):
                        if role in found: raise RuntimeError("two boards have the same role: " + role)
                        found[role] = {"port": path, "device_id": data["device_id"]}; break
                elif line.startswith(b"NUNI1 "):
                    data = json.loads(line[6:])
                    if ("temp" in data or data.get("kind") == "env" or
                        (data.get("v") == 1 and data.get("device") == "env" and data.get("kind") in ("tof", "relay"))):
                        if "environment" in found: raise RuntimeError("two environment boards found")
                        found["environment"] = {"port": path}; break
        except (OSError, ValueError) as exc:
            print(json.dumps({"port": path, "error": str(exc)}), flush=True)
        finally: port.close()
    return found


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output", default="/data/nuni/appliances/devices.json")
    args = p.parse_args(); found = discover()
    print(json.dumps(found, indent=2), flush=True)
    if any(role not in found for role in (*ROLES, "environment")):
        raise SystemExit("Need original environment ESP and both appliance ESPs; config unchanged.")
    target = Path(args.output); target.parent.mkdir(parents=True, exist_ok=True)
    temp = target.with_suffix(".tmp")
    temp.write_text(json.dumps({"v": 1, "devices": {r: found[r] for r in ROLES},
        "environment": found["environment"]}, indent=2), encoding="utf-8")
    temp.replace(target)
