"""Python 원본과 C++ 이식을 비교할 시험 시나리오를 만든다(재현 가능하도록 시드 고정).

    python3 gen_scenarios.py --seed 1 > scenario.jsonl

경계값(호흡 8·15·55, 울음 신뢰도 0.7, CO₂ 1000, 온도 18·26, 습도 35·60, 조도 40, 움직임 0.4·0.6·0.7)과
키 누락·빈 메시지·다른 키 이름(breathing_bpm, cry_score) 같은 실제로 나올 수 있는 입력을 섞는다.
"""
import argparse
import json
import random


def pick(r, edge, lo, hi, p_edge=0.25):
    return r.choice(edge) if r.random() < p_edge else round(r.uniform(lo, hi), r.choice([0, 1, 2, 3]))


def radar_payload(r):
    if r.random() < 0.03:
        return {}
    p = {}
    br = r.choice([lambda: pick(r, [8, 15, 55, 7.99, 15.01], 20, 50), lambda: pick(r, [0, 0.0, 7.9], 0, 8),
                   lambda: pick(r, [8, 15], 8, 15), lambda: pick(r, [55, 55.5], 55, 70)])()
    key = "breathing_bpm" if r.random() < 0.1 else "breathing_rate"
    if r.random() > 0.03:
        p[key] = br
    if r.random() > 0.05:
        p["movement"] = pick(r, [0.4, 0.6, 0.7, 0.5, 0.0, 1.0], 0, 1)
    p["presence"] = r.random() < 0.9
    if r.random() < 0.6:
        p["apnea"] = r.random() < 0.08
    if r.random() < 0.7:
        p["breath_valid"] = r.random() < 0.8
    p["ts"] = 0
    return p


def cry_payload(r):
    if r.random() < 0.03:
        return {}
    p = {"is_crying": r.random() < 0.4}
    k = r.random()
    conf = pick(r, [0.7, 0.69, 0.71, 0.3, 1.0], 0, 1)
    if k < 0.8:
        p["confidence"] = conf
    elif k < 0.9:
        p["cry_score"] = conf
    if r.random() < 0.9:
        p["cls"] = r.choice(["none", "hungry", "discomfort", "sleepy"])
    p["ts"] = 0
    return p


def env_payload(r):
    if r.random() < 0.03:
        return {}
    p = {}
    if r.random() > 0.05:
        p["temp"] = pick(r, [18, 26, 17.9, 26.1], 15, 30)
    if r.random() > 0.05:
        p["humidity"] = pick(r, [35, 60, 34.9, 60.1], 20, 75)
    if r.random() > 0.05:
        p["co2"] = int(pick(r, [1000, 1001, 999], 400, 1500))
    if r.random() > 0.2:
        p["lux"] = pick(r, [40, 40.1, 0], 0, 100)
    p["ts"] = 0
    return p


def fusion_session(r, out, steps):
    out.append({"op": "fusion_new", "debounce_n": r.choice([1, 2, 2, 3]), "cooldown_s": r.choice([5, 10, 10, 15])})
    t = 1_700_000_000.0
    for _ in range(steps):
        t += r.choice([0.2, 0.5, 1.0, 1.0, 1.0, 2.0, 3.0, 11.0])
        k = r.random()
        if k < 0.04:
            out.append({"op": "fusion_update", "t": t, "radar": radar_payload(r) if r.random() < .7 else None,
                        "cry": cry_payload(r) if r.random() < .7 else None,
                        "env": env_payload(r) if r.random() < .7 else None})
            continue
        topic = r.choices(["sensor/radar", "audio/cry", "sensor/env", "audio/voice"], [5, 4, 2, 0.2])[0]
        payload = {"sensor/radar": radar_payload, "audio/cry": cry_payload, "sensor/env": env_payload,
                   "audio/voice": lambda _r: {"intent": "status"}}[topic](r)
        for _ in range(r.choice([1, 1, 1, 2, 3])):   # 같은 메시지 반복(디바운스·쿨다운 시험)
            out.append({"op": "fusion", "t": t, "topic": topic, "payload": payload})
            t += 1.0


def rules(r, out, n):
    for _ in range(n):
        radar = radar_payload(r) if r.random() < 0.9 else None
        cry = cry_payload(r) if r.random() < 0.8 else None
        env = env_payload(r) if r.random() < 0.8 else None
        out.append({"op": "decide", "radar": radar, "cry": cry, "env": env})
        out.append({"op": "single", "radar": radar, "cry": cry, "env": env})
        out.append({"op": "sleep", "radar": radar, "cry": cry})
        out.append({"op": "env", "env": env, "ss": r.choice(["calm_sleep", "restless", "awake", "unknown", "hold", ""])})


def radar_session(r, out, seconds):
    cfg = {} if r.random() < 0.5 else {"apnea_hold_s": r.choice([3, 6, 10]), "smooth_window_s": r.choice([4, 8]),
                                       "move_window_s": r.choice([1.0, 2.0, 3.0])}
    out.append({"op": "radar_new", "cfg": cfg})
    t = 1_700_000_000.0
    pre = "mr60bha2"
    mode = "normal"
    dist = 60.0
    for s in range(seconds):
        if r.random() < 0.05:   # 상태 전환: 정상 / 호흡 멈춤 / 멀어짐 / 움직임 / 끊김 / 잡값
            mode = r.choice(["normal", "normal", "stop", "far", "move", "gap", "junk"])
        sub = sorted(r.uniform(0, 1) for _ in range(r.choice([0, 1, 2, 3, 4])))
        for f in sub:
            tt = t + f
            if mode == "gap":
                continue
            topic = r.choice(["breath_rate", "distance", "distance", "presence", "heart_rate", "other"])
            if topic == "breath_rate":
                raw = {"normal": lambda: f"{r.uniform(18, 40):.1f}", "stop": lambda: "0", "far": lambda: f"{r.uniform(10, 30):.1f}",
                       "move": lambda: f"{r.uniform(5, 70):.1f}", "junk": lambda: r.choice(["nan", "abc", "", " 23.5 ", "inf", "1e1"])}[mode]()
            elif topic == "distance":
                if mode == "far":
                    dist = r.uniform(150, 260)
                elif mode == "move":
                    dist = r.uniform(30, 120)
                else:
                    dist = max(20, min(150, dist + r.uniform(-3, 3)))
                raw = "nan" if mode == "junk" and r.random() < 0.5 else f"{dist:.2f}"
            elif topic == "presence":
                raw = r.choice(["ON", "ON", "ON", "OFF", "true", " on ", "0", "1", "maybe"])
            elif topic == "heart_rate":
                raw = f"{r.uniform(60, 140):.0f}" if mode != "junk" else "nan"
            else:
                raw = "x"
            out.append({"op": "esphome", "t": tt, "topic": f"{pre}/{topic}" if topic != "other" else "other/topic",
                        "raw": raw})
        t += 1.0
        out.append({"op": "radar_step", "t": t})


def resample_cases(r, out, n):
    pairs = [(48000, 16000), (44100, 16000), (8000, 16000), (22050, 16000), (16000, 16000), (32000, 16000)]
    for i in range(n):
        sr, tg = r.choice(pairs)
        ln = r.choice([1, 2, 3, 10, 101, 997, 4800]) if i else 72000   # 첫 번째는 실제 크기(48kHz 1.5초)
        x = [round(r.uniform(-1, 1), 6) for _ in range(ln)]
        out.append({"op": "resample", "x": x, "sr": sr, "target": tg})


def round_cases(r, out, n):
    fixed = [0.125, 0.375, 2.675, 0.5, 1.5, 2.5, -0.5, -2.675, 0.05, 0.15, 0.25, 0.35, 1e16, -0.04, 0.045, 12.345]
    for x in fixed:
        for k in (0, 1, 2):
            out.append({"op": "pyround", "x": x, "n": k})
    for _ in range(n):
        out.append({"op": "pyround", "x": r.uniform(-100, 100), "n": r.choice([1, 2])})
        out.append({"op": "pyround", "x": r.randint(0, 2000) / 1000 + 0.0005 * r.choice([0, 1]), "n": r.choice([1, 2, 3])})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--scale", type=int, default=1, help="시나리오 크기 배수")
    a = ap.parse_args()
    r = random.Random(a.seed)
    out = []
    round_cases(r, out, 300 * a.scale)
    rules(r, out, 1500 * a.scale)
    for _ in range(8 * a.scale):
        fusion_session(r, out, 400)
    for _ in range(8 * a.scale):
        radar_session(r, out, 600)
    resample_cases(r, out, 40 * a.scale)
    for c in out:
        print(json.dumps(c, ensure_ascii=False))


if __name__ == "__main__":
    main()
