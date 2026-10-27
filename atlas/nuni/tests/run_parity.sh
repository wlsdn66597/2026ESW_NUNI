#!/bin/bash
# Python 원본과 C++ 이식이 같은 판단을 내리는지 한 번에 확인한다(개발 PC, 일반 리눅스).
#
#   ./run_parity.sh [빌드 폴더]                 # 기본: ../build (없으면 만든다)
#   MODEL=yamnet_emb.tflite CLASSMAP=yamnet_class_map.csv WAV=cry.wav ./run_parity.sh   # 울음 감지까지
#   E2E=1 ./run_parity.sh                         # mosquitto로 MQTT 통합 시험까지(mosquitto 필요)
#
# 필요: python3 + numpy(리샘플 비교), paho-mqtt(E2E), ai-edge-litert(울음 비교). 저장소 루트의 Python 코드를 그대로 쓴다.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="${1:-$HERE/../build}"
mkdir -p "$BUILD"
cmake -S "$HERE/.." -B "$BUILD" >/dev/null
cmake --build "$BUILD" -j"$(nproc)" >/dev/null
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0
for seed in 1 2 3; do
  python3 "$HERE/gen_scenarios.py" --seed "$seed" --scale 2 > "$TMP/sc.jsonl"
  python3 "$HERE/py_replay.py" < "$TMP/sc.jsonl" > "$TMP/py.jsonl"
  "$BUILD/nuni_replay" < "$TMP/sc.jsonl" > "$TMP/cpp.jsonl"
  echo "== 시드 $seed"
  python3 "$HERE/compare.py" "$TMP/sc.jsonl" "$TMP/py.jsonl" "$TMP/cpp.jsonl" || fail=1
done

if [ -n "${MODEL:-}" ] && [ -n "${WAV:-}" ] && ! grep -q "NUNI_TFLITE_LIBS:STRING=.\+" "$BUILD/CMakeCache.txt" \
   && ! nm -C "$BUILD/nuni_replay" 2>/dev/null | grep -q make_yamnet_tflite; then
  echo "== 울음 감지: 이 빌드에는 TFLite가 없어 건너뜀(NUNI_TFLITE_LIBS 또는 pkg-config tensorflowlite 필요)"
elif [ -n "${MODEL:-}" ] && [ -n "${WAV:-}" ]; then
  echo "== 울음 감지(같은 WAV를 1초마다 1.5초 창으로)"
  echo "{\"op\":\"model_wav\",\"wav\":\"$WAV\",\"model\":\"$MODEL\"}" > "$TMP/m.jsonl"
  echo "{\"op\":\"cry_wav\",\"wav\":\"$WAV\",\"model\":\"$MODEL\",\"classmap\":\"${CLASSMAP:-}\"}" >> "$TMP/m.jsonl"
  python3 "$HERE/py_replay.py" < "$TMP/m.jsonl" > "$TMP/mpy.jsonl" 2>"$TMP/mpy.err" || { cat "$TMP/mpy.err"; fail=1; }
  "$BUILD/nuni_replay" < "$TMP/m.jsonl" > "$TMP/mcpp.jsonl" 2>/dev/null || fail=1
  # TFLite 버전이 다르면 실수 계산 끝자리가 달라질 수 있어 작은 오차는 허용한다.
  # 정수 양자화 모델(TF Hub 분류용 yamnet.tflite 등)은 버전에 따라 일부 창의 점수가 크게 달라질 수 있다 → 실수형 yamnet_emb.tflite로 비교
  python3 "$HERE/compare.py" "$TMP/m.jsonl" "$TMP/mpy.jsonl" "$TMP/mcpp.jsonl" --tol 1e-4 || {
    echo "  (정수 양자화 모델이면 TFLite 버전 차이일 수 있음 — 실수형 yamnet_emb.tflite로 다시 비교하세요)"; fail=1; }
fi

if [ -n "${E2E:-}" ]; then
  echo "== MQTT 통합 시험"
  python3 "$HERE/e2e_mqtt.py" --bin "$BUILD/nunifusion" --seconds 40 | tail -n 6 || fail=1
fi

[ "$fail" = 0 ] && echo "전부 일치" || { echo "불일치 있음"; exit 1; }
