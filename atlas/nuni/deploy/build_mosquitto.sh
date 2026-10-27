#!/bin/bash
# atlas 기기용 MQTT 브로커(mosquitto) 빌드 — 기기에 브로커가 없어서(2026-09-28 확인) 우리가 같이 넣는다.
#
# 개발 컨테이너(atlas SDK 환경이 잡힌 셸)에서 실행한다. nunifusion을 빌드할 때와 같은 cmake 크로스 설정을 쓴다.
#   ./build_mosquitto.sh [출력 폴더]      # 기본: ./mosquitto-out
# 결과: <출력>/mosquitto (브로커 실행 파일 하나, TLS·웹소켓·cJSON 없음, libc·pthread만 씀) + mosquitto.conf
set -euo pipefail
VER=v2.0.18
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$(mkdir -p "${1:-mosquitto-out}" && cd "${1:-mosquitto-out}" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

git clone -q --depth 1 --branch "$VER" https://github.com/eclipse/mosquitto.git "$WORK/src"
cmake -S "$WORK/src" -B "$WORK/build" -DCMAKE_BUILD_TYPE=Release \
  -DWITH_TLS=OFF -DWITH_TLS_PSK=OFF -DWITH_CJSON=OFF -DWITH_WEBSOCKETS=OFF -DWITH_SRV=OFF \
  -DWITH_CLIENTS=OFF -DWITH_APPS=OFF -DWITH_PLUGINS=OFF -DWITH_LIB_CPP=OFF -DDOCUMENTATION=OFF \
  -DWITH_SYSTEMD=OFF -DWITH_DLT=OFF -DWITH_THREADING=ON >/dev/null
cmake --build "$WORK/build" --target mosquitto -j"$(nproc)" >/dev/null
cp "$WORK/build/src/mosquitto" "$OUT/mosquitto"
cp "$HERE/mosquitto.conf" "$OUT/mosquitto.conf"
file "$OUT/mosquitto" 2>/dev/null || true
echo "완료: $OUT/mosquitto ($VER)"
