"""MQTT 통합 시험 — 실제 브로커(mosquitto)에 nunifusion을 붙여 전체 흐름을 확인한다.

    python3 e2e_mqtt.py --bin ../build/nunifusion [--seconds 40]

1) 시험용 mosquitto를 따로 띄우고(포트 18830), nunifusion을 붙인다(울음·D-Bus 끔).
2) ESPHome 레이더 평문(mr60bha2/*)과 sensor/env, audio/cry를 발행하고, 브로커의 모든 메시지를 기록한다.
   중간에 브로커를 재시작해 재접속·구독 복원을 확인하고, JSON이 아닌 메시지도 섞는다.
3) 브로커 재시작 전까지 기록된 입력(sensor/*, audio/cry)을 같은 순서로 Python 원본 Fusion에 넣어,
   nunifusion이 낸 fusion/state와 같은 판단이 나오는지 비교한다(시각은 C++이 찍은 ts를 그대로 씀).
   재시작 뒤에는 기록기와 nunifusion이 다시 붙는 순서가 매번 달라 입력이 1:1로 맞지 않으므로,
   다시 접속해 구독이 복원됐는지(입력을 받고 상태를 다시 내는지)만 본다.
"""
import argparse
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

import paho.mqtt.client as mqtt

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
sys.path.insert(0, ROOT)
PORT = 18830


def start_broker(tmp):
    conf = os.path.join(tmp, "m.conf")
    with open(conf, "w") as f:
        f.write(f"listener {PORT} 127.0.0.1\nallow_anonymous true\npersistence false\n")
    return subprocess.Popen([shutil.which("mosquitto") or "/usr/sbin/mosquitto", "-c", conf],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def client(name):
    cbv = getattr(mqtt, "CallbackAPIVersion", None)
    c = mqtt.Client(cbv.VERSION1, client_id=name) if cbv else mqtt.Client(client_id=name)
    return c


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--seconds", type=int, default=40)
    a = ap.parse_args()
    tmp = tempfile.mkdtemp()
    broker = start_broker(tmp)
    time.sleep(0.5)

    env = dict(os.environ, NUNI_ENABLE_CRY="0", NUNI_ENABLE_DBUS="0", NUNI_MQTT_PORT=str(PORT))
    log = open(os.path.join(tmp, "nuni.log"), "w")
    svc = subprocess.Popen([a.bin], env=env, stdout=log, stderr=subprocess.STDOUT)

    rec = []
    lock = threading.Lock()

    def on_msg(cl, ud, m):
        with lock:
            rec.append((time.time(), m.topic, m.payload.decode("utf-8", "replace")))

    sub = client("e2e-rec")
    sub.on_message = on_msg
    sub.on_connect = lambda cl, ud, fl, rc: cl.subscribe("#")
    sub.reconnect_delay_set(1, 2)
    sub.connect("127.0.0.1", PORT, 30)
    sub.loop_start()
    pub = client("e2e-pub")
    pub.reconnect_delay_set(1, 2)
    pub.connect("127.0.0.1", PORT, 30)
    pub.loop_start()
    time.sleep(2.0)

    t0 = time.time()
    restarted = False
    k = 0
    while time.time() - t0 < a.seconds:
        el = time.time() - t0
        # 레이더: 앞 1/3 정상 호흡 → 가운데 호흡 0(무호흡 유도) → 뒤 정상 + 큰 움직임
        phase = 0 if el < a.seconds / 3 else (1 if el < 2 * a.seconds / 3 else 2)
        br = {0: 30 + (k % 5), 1: 0, 2: 28}[phase]
        dist = 60 + (0 if phase < 2 else (k % 4) * 12)
        pub.publish("mr60bha2/breath_rate", f"{br:.1f}")
        pub.publish("mr60bha2/distance", f"{dist:.2f}")
        pub.publish("mr60bha2/presence", "ON")
        pub.publish("mr60bha2/heart_rate", "120")
        if k % 8 == 0:
            pub.publish("sensor/env", json.dumps({"temp": 22 + (k % 3), "humidity": 30 if phase == 1 else 45,
                                                  "co2": 1100 if phase == 2 else 600, "lux": 50, "ts": time.time()}))
        if k % 4 == 0:
            crying = phase >= 1 and (k // 4) % 2 == 0
            pub.publish("audio/cry", json.dumps({"is_crying": crying, "cls": "none",
                                                 "confidence": 0.85 if crying else 0.05, "ts": time.time()}))
        if k == 20:
            pub.publish("sensor/env", "not json")             # 무시돼야 함
        if not restarted and el > a.seconds / 2:              # 브로커 재시작 → 재접속·구독 복원
            t_restart = time.time()
            broker.terminate()
            broker.wait()
            time.sleep(1.5)
            broker = start_broker(tmp)
            restarted = True
            time.sleep(4.0)
        k += 1
        time.sleep(0.25)
    time.sleep(2.0)
    svc.send_signal(2)
    try:
        svc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        svc.kill()
    sub.loop_stop()
    pub.loop_stop()
    broker.terminate()
    log.close()
    print(open(os.path.join(tmp, "nuni.log")).read())

    # ---- 검증 ----
    import fusion as fusion_mod

    with lock:
        msgs = list(rec)
    n_radar = sum(1 for _, t, _ in msgs if t == "sensor/radar")
    states = [json.loads(p) for _, t, p in msgs if t == "fusion/state"]
    alerts = [json.loads(p) for _, t, p in msgs if t == "fusion/alert"]
    apnea = sum(1 for _, t, p in msgs if t == "sensor/radar" and json.loads(p).get("apnea"))
    after = [t for ts, t, _ in msgs if t == "sensor/radar" and ts > t0 + a.seconds / 2 + 6]
    print(f"기록 {len(msgs)}개: sensor/radar {n_radar}(무호흡 {apnea}), fusion/state {len(states)}, 경보 {len(alerts)}")
    print(f"브로커 재시작 뒤 sensor/radar {len(after)}개")

    # 원본 Fusion으로 같은 입력을 재생
    fake = [0.0]
    pubs = []
    fusion_mod.bus.publish = lambda t, p: pubs.append((t, p))
    fusion_mod.topics.now = lambda: fake[0]
    f = fusion_mod.Fusion(now_fn=lambda: fake[0])
    ins = [(t, p) for ts, t, p in msgs if t in ("sensor/radar", "audio/cry", "sensor/env") and ts < t_restart]
    states_after = [s for s in states if s["ts"] > t_restart + 3]
    states = [s for s in states if s["ts"] < t_restart]
    py_states = []
    si = 0
    for t, p in ins:
        try:
            payload = json.loads(p)
        except json.JSONDecodeError:
            continue                                         # C++도 버린다(원본은 여기서 예외로 멈춤)
        if si >= len(states):
            break
        fake[0] = states[si]["ts"]                           # C++이 그 입력을 처리한 시각
        pubs.clear()
        f.on_message(t, payload)
        py_states += [pp for tt, pp in pubs if tt == "fusion/state"]
        si += 1

    def norm(o):
        if isinstance(o, float) and not math.isfinite(o):
            return None
        if isinstance(o, dict):
            return {k: norm(v) for k, v in o.items()}
        if isinstance(o, (list, tuple)):
            return [norm(v) for v in o]
        return o

    n = min(len(states), len(py_states))
    bad = [i for i in range(n) if norm(states[i]) != norm(py_states[i])]
    print(f"fusion/state 비교: {n}개 중 불일치 {len(bad)}개")
    for i in bad[:3]:
        print("  C++ ", states[i])
        print("  Py  ", py_states[i])
    print(f"재시작 뒤 fusion/state {len(states_after)}개")
    ok = (n_radar > 10 and apnea > 0 and len(after) > 3 and n > 10 and not bad and len(states) == len(ins) - 1
          and len(states_after) > 5 and len(alerts) > 0)
    print("결과:", "통과" if ok else "실패", f"(재시작 전 입력 {len(ins)}개 중 JSON 아님 1개 제외 → 상태 {len(states)}개)")
    shutil.rmtree(tmp, ignore_errors=True)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
