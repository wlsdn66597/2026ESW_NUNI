#!/bin/sh
# 파이 4(atlas)에서 root로 한 번 실행한다: 진동 노드 nunivibe를 부팅 때 자동으로 켜지게 등록한다.
#   준비: /data/nuni/vibe/ 에 nunivibe, nunivibe.service, 이 스크립트를 둔다(/data는 읽기·쓰기 파티션)
#   브로커(파이 5) 주소나 GPIO 번호를 바꾸려면 nunivibe.service의 Environment=를 고친 뒤 다시 실행한다.
set -e
DIR=/data/nuni/vibe
if [ ! -x "$DIR/nunivibe" ]; then
  echo "$DIR/nunivibe 가 없습니다."
  exit 1
fi

mount -o remount,rw /
cp "$DIR/nunivibe.service" /etc/systemd/system/nunivibe.service
systemctl daemon-reload
systemctl enable nunivibe.service
systemctl restart nunivibe.service
sleep 2
mount -o remount,ro / || true

systemctl --no-pager status nunivibe.service | head -n 12 || true
echo "로그 보기: journalctl -u nunivibe -f"
