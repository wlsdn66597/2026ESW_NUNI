#!/bin/sh
set -eu
role=${1:?pi4 or pi5}
case "$role" in pi4) unit=nuni-appliance-usb.service;; pi5) unit=nuni-appliance-policy.service;; *)exit 2;; esac
root=/data/nuni/appliances
stamp=$(date '+%Y%m%d-%H%M%S')
release=$root/releases/$stamp
backup=$root/backups/$stamp
previous=$(readlink "$root/current" 2>/dev/null || true)
mkdir -p "$release" "$backup"
tar -xzf /data/nuni/appliances-package.tar.gz -C "$release"
tar -xzf "$release/python-aarch64.tar.gz" -C "$release"
chmod +x "$release/python/bin/"*
export PYTHONPATH="$release/vendor" PYTHONDONTWRITEBYTECODE=1
"$release/python/bin/python3" -c 'import sys,serial,paho.mqtt.client,thinqconnect; print(sys.version)'
"$release/python/bin/python3" -m unittest discover -s "$release/code/tests" -v
"$release/python/bin/python3" "$release/code/probe_thinq.py"
test ! -f "/etc/systemd/system/$unit" || cp -p "/etc/systemd/system/$unit" "$backup/unit"
ro=0;success=0
cleanup() {
  rc=$?
  if [ "$success" -eq 0 ]; then
    systemctl stop "$unit" 2>/dev/null || true
    if [ -n "$previous" ]; then ln -sfn "$previous" "$root/current"; else rm -f "$root/current"; fi
    if [ -f "$backup/unit" ]; then
      cp -p "$backup/unit" "/etc/systemd/system/$unit"
    else
      systemctl disable "$unit" 2>/dev/null || true
      rm -f "/etc/systemd/system/$unit"
    fi
    systemctl daemon-reload || true
    if [ -n "$previous" ]; then systemctl start "$unit" || true; fi
  fi
  if [ "$ro" -eq 1 ]; then sync; mount -o remount,ro /; fi
  exit "$rc"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
if awk '$2=="/" && $4 ~ /(^|,)ro(,|$)/ {found=1} END {exit !found}' /proc/mounts; then
  ro=1;mount -o remount,rw /
fi
systemctl stop "$unit" 2>/dev/null || true
ln -sfn "$release" "$root/current"
cp "$release/code/deploy/$unit" "/etc/systemd/system/$unit"
if [ "$role" = pi4 ] && [ ! -f "$root/devices.json" ]; then
  printf '%s\n' '{"v":1,"devices":{}}' > "$root/devices.json"
fi
systemctl daemon-reload
systemctl enable "$unit"
systemctl restart "$unit"
sleep 2
systemctl is-active "$unit"
success=1
printf 'RELEASE=%s\nBACKUP=%s\n' "$release" "$backup"
