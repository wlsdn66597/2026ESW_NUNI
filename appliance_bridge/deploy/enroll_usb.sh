#!/bin/sh
# All three ESPs must be connected. Keep the same physical USB ports afterwards.
set -eu
root=/data/nuni/appliances
current=$root/current
export PYTHONPATH="$current/vendor" PYTHONDONTWRITEBYTECODE=1
ro=0
cleanup() {
  rc=$?
  systemctl daemon-reload || true
  systemctl start nunisensors nuni-appliance-usb.service || true
  if [ "$ro" -eq 1 ]; then sync; mount -o remount,ro /; fi
  exit "$rc"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
systemctl stop nuni-appliance-usb.service nunisensors
"$current/python/bin/python3" "$current/code/enroll.py" --output "$root/devices.next.json"
"$current/python/bin/python3" - "$root/devices.next.json" "$root/env-port.conf" <<'PY'
import json,sys
from pathlib import Path
j=json.loads(Path(sys.argv[1]).read_text())
p=j['environment']['port']
assert p.startswith('/dev/serial/by-path/') and all(c not in p for c in '\n\r" ')
Path(sys.argv[2]).write_text('[Service]\nEnvironment=NUNI_ENV_PORT='+p+'\n')
PY
if awk '$2=="/" && $4 ~ /(^|,)ro(,|$)/ {found=1} END {exit !found}' /proc/mounts; then
  ro=1;mount -o remount,rw /
fi
mkdir -p /etc/systemd/system/nunisensors.service.d
test ! -f "$root/devices.json" || cp -p "$root/devices.json" "$root/devices.before-enroll.json"
mv "$root/devices.next.json" "$root/devices.json"
cp "$root/env-port.conf" /etc/systemd/system/nunisensors.service.d/60-appliance-env-port.conf
systemctl daemon-reload
