#!/bin/sh
# atlas 기기에서 root로 한 번 실행한다: arc install로 IPK를 설치한 뒤, 서비스가 실제로 뜨도록 마무리한다.
#
# 2026-09-19 샘플 서비스(lifecycleservice)를 올리며 확인한 함정 5가지를 그대로 처리한다.
#   1) opkg가 /data/share 아래(오프라인 루트)에 풀어서 D-Bus 파일이 시스템 경로에 없다 → 시스템 경로에 둔다
#   2) 루트(/)가 읽기 전용이다 → 잠깐 읽기·쓰기로 다시 마운트한다
#   3) dbus 데몬은 messagebus 권한으로 읽는다 → root:messagebus, 640
#   4) 설치본 .service/.conf에는 설치 시점 UID(u0_a500x)가 박혀 재설치하면 어긋난다 → 알려진 내용으로 새로 쓴다
#   5) D-Bus가 /data/share 실행 파일을 직접 띄우면 보안 레이블에 막힌다 → systemd 유닛을 두고 SystemdService=로 위임
#
# 기기 셸은 BusyBox다(head -n 처럼 옵션을 쓰고, timeout 명령이 없다).
set -e
ID=com.atlas.NuniFusion1
SVC=/data/share/usr/atlas/services/$ID

mkdir -p /data/nuni/voice
if [ ! -f /data/nuni/voice/parent.wav ] && [ -f "$SVC/voice/parent.wav" ]; then
  cp "$SVC/voice/parent.wav" /data/nuni/voice/parent.wav
fi

if [ ! -x "$SVC/nunifusion" ]; then
  echo "먼저 IPK를 설치하세요(arc install ...). $SVC/nunifusion 이 없습니다."
  exit 1
fi

mount -o remount,rw /

cat > /etc/dbus-1/system.d/$ID.conf <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <policy user="root">
    <allow own="$ID"/>
    <allow send_destination="$ID"/>
    <allow receive_sender="$ID"/>
  </policy>
  <policy context="default">
    <allow send_destination="$ID"/>
    <allow receive_sender="$ID"/>
  </policy>
  <!-- 내려받아 설치한 앱(대시보드 등)은 package-manager 그룹(1011)으로 실행된다(2026-10-20 Pi 5 확인).
       패키지 관리자가 설치 때 만드는 /data/share/usr/share/dbus-1/system.d/$ID.conf는 마지막에 읽히고
       기본(default)으로 모두 막는다. D-Bus는 group 정책을 default 정책보다 나중에 적용하므로, 여기서 허용해야
       앱이 메서드를 부르고 신호를 받을 수 있다(앱 UID는 설치마다 바뀌어 user로 적을 수 없다). -->
  <policy group="package-manager">
    <allow send_destination="$ID"/>
    <allow receive_sender="$ID"/>
  </policy>
</busconfig>
EOF

cat > /usr/share/dbus-1/system-services/$ID.service <<EOF
[D-BUS Service]
Name=$ID
Exec=$SVC/nunifusion
User=root
SystemdService=nunifusion.service
EOF

chown root:messagebus /etc/dbus-1/system.d/$ID.conf /usr/share/dbus-1/system-services/$ID.service
chmod 640 /etc/dbus-1/system.d/$ID.conf /usr/share/dbus-1/system-services/$ID.service

cp "$SVC/systemd/nunifusion.service" /etc/systemd/system/nunifusion.service
UNITS="nunifusion.service"
mkdir -p /etc/systemd/system/nunifusion.service.d
if [ -f "$SVC/systemd/65-mic-input-gain.conf" ];then
  cp "$SVC/systemd/65-mic-input-gain.conf" /etc/systemd/system/nunifusion.service.d/65-mic-input-gain.conf
fi
if [ -x "$SVC/mosquitto/mosquitto" ]; then
  cp "$SVC/systemd/nuni-mosquitto.service" /etc/systemd/system/nuni-mosquitto.service
  UNITS="nuni-mosquitto.service $UNITS"
else
  echo "참고: $SVC/mosquitto/mosquitto 가 없어 브로커는 설정하지 않습니다(다른 기기의 브로커를 쓰면 NUNI_MQTT_HOST를 바꾸세요)."
fi

# 대시보드 앱이 깔려 있으면 부팅 때 자동으로 띄운다(재부팅마다 손으로 켜지 않게)
if [ -d /data/share/usr/atlas/apps/com.atlas.app.nuni_dashboard ] && [ -f "$SVC/systemd/nuni-dashboard-start.service" ]; then
  cp "$SVC/systemd/nuni-dashboard-start.service" /etc/systemd/system/nuni-dashboard-start.service
  systemctl enable nuni-dashboard-start.service
fi

systemctl daemon-reload
systemctl reload dbus || true
for u in $UNITS; do
  systemctl enable "$u"
  systemctl restart "$u"
done
sleep 2
mount -o remount,ro / || true

echo "=== 상태"
systemctl --no-pager status nunifusion.service | head -n 12 || true
echo "=== D-Bus Ping"
busctl call $ID /com/atlas/NuniFusion1 $ID Ping || true
echo "로그 보기: journalctl -u nunifusion -f"
