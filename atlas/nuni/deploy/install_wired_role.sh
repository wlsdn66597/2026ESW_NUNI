#!/bin/sh
# Archive layout: bin/ and deploy/ next to this script's deploy directory.
set -eu
role=${1:?pi4 or pi5 required}
case "$role" in pi4|pi5);;*)exit 2;;esac
here=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
backup=/data/nuni/backups/wired-split-20261024
mkdir -p "$backup"
ro=0
if awk '$2=="/" && $4 ~ /(^|,)ro(,|$)/ {found=1} END {exit !found}' /proc/mounts; then
  ro=1;mount -o remount,rw /
fi
restore_mount(){ if [ "$ro" -eq 1 ];then sync;mount -o remount,ro /;fi; }
trap restore_mount EXIT
if [ "$role" = pi4 ]; then
  mkdir -p /data/nuni/sensors /data/nuni/vibe /etc/udev/rules.d
  if [ -f /data/nuni/vibe/nunivibe ] && [ ! -f "$backup/nunivibe.before" ];then cp -p /data/nuni/vibe/nunivibe "$backup/nunivibe.before";fi
  systemctl stop nunisensors nunivibe
  systemctl disable --now nuni-radar-wifi.service 2>/dev/null || true
  if [ -f /etc/systemd/system/nunisensors.service.d/70-radar-wifi-demo.conf ];then
    cp -p /etc/systemd/system/nunisensors.service.d/70-radar-wifi-demo.conf "$backup/radar-input.before.conf"
    rm -f /etc/systemd/system/nunisensors.service.d/70-radar-wifi-demo.conf
  fi
  cp "$here/bin/nunisensors" /data/nuni/sensors/nunisensors
  cp "$here/bin/nunivibe" /data/nuni/vibe/nunivibe
  chmod 755 /data/nuni/sensors/nunisensors /data/nuni/vibe/nunivibe
  cp "$here/deploy/nunisensors.service" /etc/systemd/system/
  cp "$here/deploy/nunivibe.service" /etc/systemd/system/
  cp "$here/deploy/99-nuni-usb.rules" /etc/udev/rules.d/
  modprobe cdc_acm || true
  udevadm control --reload-rules
  udevadm trigger --subsystem-match=tty
  systemctl daemon-reload
  systemctl enable nunisensors nunivibe
  systemctl restart nunisensors nunivibe
  systemctl is-active nunisensors nunivibe
else
  svc=/data/share/usr/atlas/services/com.atlas.NuniFusion1
  ip addr show dev eth0 | awk '/inet 10.77.0.5\/24/{found=1}END{exit !found}'
  if [ ! -f "$backup/nunifusion.before" ];then cp -p "$svc/nunifusion" "$backup/nunifusion.before";fi
  if [ ! -f "$backup/mosquitto.conf.before" ];then cp -p "$svc/mosquitto/mosquitto.conf" "$backup/mosquitto.conf.before";fi
  systemctl stop nunifusion nuni-mosquitto
  systemctl disable --now nuni-radar-wifi-ingress.service 2>/dev/null || true
  cp "$here/bin/nunifusion" "$svc/nunifusion"
  chmod 755 "$svc/nunifusion"
  cp "$here/deploy/mosquitto-wired.conf" "$svc/mosquitto/mosquitto.conf"
  mkdir -p /etc/systemd/system/nunifusion.service.d
  cp "$here/deploy/pi5-wired.conf" /etc/systemd/system/nunifusion.service.d/50-wired.conf
  if [ -f "$here/deploy/65-mic-input-gain.conf" ];then
    cp "$here/deploy/65-mic-input-gain.conf" /etc/systemd/system/nunifusion.service.d/65-mic-input-gain.conf
  fi
  systemctl daemon-reload
  if ! systemctl restart nuni-mosquitto nunifusion;then
    systemctl stop nunifusion nuni-mosquitto
    cp "$backup/nunifusion.before" "$svc/nunifusion"
    cp "$backup/mosquitto.conf.before" "$svc/mosquitto/mosquitto.conf"
    rm -f /etc/systemd/system/nunifusion.service.d/50-wired.conf
    systemctl daemon-reload
    systemctl start nuni-mosquitto nunifusion
    echo 'New services failed; restored previous binary and broker config.' >&2
    exit 1
  fi
  systemctl is-active nunifusion nuni-mosquitto nuni-dashboard-start
fi
