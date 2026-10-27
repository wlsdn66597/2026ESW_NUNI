"""nuni_replay(C++)와 같은 명령을 Python 원본 모듈로 실행한다 — 이식 결과 비교용.

    python3 py_replay.py < scenario.jsonl > py_out.jsonl

저장소 루트의 fusion.py · sleep_state.py · env_control.py · personalization.py · radar_bridge.py ·
cry_model/yamnet_cry.py를 그대로 불러 쓴다(수정하지 않음). 시간은 가짜 시계로 바꿔 끼운다.
"""
import importlib
import json
import math
import os
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
sys.path[:0] = [ROOT, os.path.join(ROOT, "cry_model")]

_fake = [0.0]
time.time = lambda: _fake[0]          # 모든 모듈의 time.time()을 가짜 시계로

import env_control  # noqa: E402
import fusion as fusion_mod  # noqa: E402
import sleep_state  # noqa: E402

_pubs = []
fusion_mod.bus.publish = lambda topic, payload: _pubs.append({"topic": topic, "payload": payload})


def clean(o):
    """NaN·무한대 → None (C++은 표준 JSON이라 null로 낸다), 튜플 → 리스트."""
    if isinstance(o, float) and not math.isfinite(o):
        return None
    if isinstance(o, dict):
        return {k: clean(v) for k, v in o.items()}
    if isinstance(o, (list, tuple)):
        return [clean(v) for v in o]
    return o


def main():
    fusion = None
    rb = None
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        c = json.loads(line)
        op = c.get("op", "")
        if "t" in c:
            _fake[0] = float(c["t"])
        if op == "fusion_new":
            fusion = fusion_mod.Fusion(debounce_n=c.get("debounce_n", 2), cooldown_s=c.get("cooldown_s", 10),
                                       now_fn=lambda: _fake[0])
            out = {"ok": True}
        elif op in ("fusion", "fusion_update"):
            _pubs.clear()
            if op == "fusion":
                level, reasons, emitted = fusion.on_message(c.get("topic", ""), c.get("payload"))
            else:
                level, reasons, emitted = fusion.update(c.get("radar"), c.get("cry"), c.get("env"))
            out = {"level": level, "reasons": reasons, "emitted": emitted, "pubs": list(_pubs)}
        elif op == "decide":
            level, reasons = fusion_mod.decide(c.get("radar"), c.get("cry"), c.get("env"))
            out = {"level": level, "reasons": reasons}
        elif op == "single":
            level, reasons = fusion_mod.single_sensor_decide(c.get("radar"), c.get("cry"), c.get("env"))
            out = {"level": level, "reasons": reasons}
        elif op == "sleep":
            s, r = sleep_state.classify(c.get("radar"), c.get("cry"))
            out = {"state": s, "reason": r}
        elif op == "env":
            out = {"actions": [[a, r] for a, r in env_control.recommend(c.get("env"), c.get("ss", ""))]}
        elif op == "radar_new":
            import radar_bridge
            rb = importlib.reload(radar_bridge)        # 모듈 전역 상태 초기화
            names = {"apnea_hold_s": "APNEA_HOLD_S", "apnea_require_prior_s": "APNEA_REQUIRE_PRIOR_S",
                     "stale_s": "STALE_S", "move_window_s": "MOVE_WINDOW_S", "move_scale_cm": "MOVE_SCALE_CM",
                     "br_min": "BR_MIN", "br_max": "BR_MAX", "dist_min_cm": "DIST_MIN_CM",
                     "dist_max_cm": "DIST_MAX_CM", "move_reject": "MOVE_REJECT", "smooth_window_s": "SMOOTH_WINDOW_S"}
            for k, v in c.get("cfg", {}).items():
                setattr(rb, names[k], float(v))
            out = {"ok": True}
        elif op == "esphome":
            rb.on_esphome(c["topic"], c["raw"])
            out = {"ok": True}
        elif op == "radar_step":
            out = {"radar": rb.step(_fake[0])}
        elif op == "resample":
            import numpy as np
            from yamnet_cry import resample_linear
            y = resample_linear(np.asarray(c["x"], dtype=np.float32), c["sr"], c["target"])
            out = {"y": [float(v) for v in y]}
        elif op == "pyround":
            out = {"v": round(float(c["x"]), int(c["n"]))}
        elif op == "cry_wav":
            out = cry_wav(c)
        elif op == "model_wav":
            out = model_wav(c)
        else:
            out = {"error": "unknown op " + op}
        sys.stdout.write(json.dumps(clean(out), ensure_ascii=False) + "\n")


def cry_wav(c):
    """cry_classifier.classify_yamnet과 같은 계산: 1초마다 최근 1.5초 → 16kHz 선형 리샘플 → YAMNet."""
    import wave

    import numpy as np
    import yamnet_cry
    import topics
    from yamnet_cry import YamnetCry, resample_linear

    yamnet_cry.MODEL_PATH = c["model"]
    if c.get("classmap"):
        yamnet_cry.CLASS_MAP_PATH = c["classmap"]
    m = YamnetCry.load()
    with wave.open(c["wav"]) as w:
        sr, ch = w.getframerate(), w.getnchannels()
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").reshape(-1, ch)[:, 0] / 32768.0
    x = x.astype(np.float32)
    win = int(sr * 1.5)
    rows = []
    for end in range(sr, len(x) + 1, sr):
        buf = np.zeros(win, dtype=np.float32)
        take = min(win, end)
        buf[win - take:] = x[end - take:end]
        score, label = m.predict(resample_linear(buf, sr))
        msg = topics.cry_msg(score > 0.3, "none", round(score, 2))
        rows.append({"score": score, "label": label, "msg": msg})
    return {"rows": rows}


def _wav_windows(path):
    import wave

    import numpy as np
    from yamnet_cry import resample_linear
    with wave.open(path) as w:
        sr, ch = w.getframerate(), w.getnchannels()
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").reshape(-1, ch)[:, 0] / 32768.0
    x = x.astype(np.float32)
    win = int(sr * 1.5)
    for end in range(sr, len(x) + 1, sr):
        buf = np.zeros(win, dtype=np.float32)
        take = min(win, end)
        buf[win - take:] = x[end - take:end]
        y = resample_linear(buf, sr)
        y = np.pad(y, (0, 15600 - len(y))) if len(y) < 15600 else y[-15600:]
        yield y.astype(np.float32)


def model_wav(c):
    """모델 출력만 비교(같은 창 → 점수 중 울음 최대값, 임베딩 앞 8개·노름)."""
    import numpy as np
    from ai_edge_litert.interpreter import Interpreter
    it = Interpreter(model_path=c["model"])
    it.allocate_tensors()
    inp = it.get_input_details()[0]
    rows = []
    for y in _wav_windows(c["wav"]):
        it.set_tensor(inp["index"], y.reshape(inp["shape"]))
        it.invoke()
        outs = {int(np.prod(d["shape"])): it.get_tensor(d["index"]).reshape(-1) for d in it.get_output_details()}
        row = {}
        if 521 in outs:
            row["cry"] = float(max(outs[521][19], outs[521][20]))
        if 1024 in outs:
            e = outs[1024]
            row["emb8"] = [float(v) for v in e[:8]]
            row["norm"] = float(np.sqrt(np.sum(e.astype(np.float64) ** 2)))
        rows.append(row)
    return {"rows": rows}


if __name__ == "__main__":
    main()
