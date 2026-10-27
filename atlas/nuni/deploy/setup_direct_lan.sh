#!/bin/sh
set -eu
role=${1:?pi4 or pi5 required}
iface=${2:-eth0}
case "$role" in
  pi4) address=10.77.0.4;peer=10.77.0.5;;
  pi5) address=10.77.0.5;peer=10.77.0.4;;
  *) echo 'role must be pi4 or pi5' >&2;exit 2;;
esac
test -r "/sys/class/net/$iface/address"
mac=$(tr -d ':' < "/sys/class/net/$iface/address")
service="ethernet_${mac}_cable"
backup=/data/nuni/backups/wired-split-20261024
mkdir -p "$backup"
ip addr show dev "$iface" > "$backup/ethernet-before.txt"
if [ -e "/var/lib/connman/$service/settings" ] && [ ! -e "$backup/connman-ethernet-settings" ]; then
  cp -p "/var/lib/connman/$service/settings" "$backup/connman-ethernet-settings"
fi
connmanctl config "$service" --ipv4 manual "$address" 255.255.255.0 0.0.0.0
ip addr show dev "$iface"
ip route get "$peer"
