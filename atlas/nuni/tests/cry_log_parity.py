"""CryLog(C++, atlas 서비스) ↔ CryPersonal(Python, cry_personal.py) 비교.

같은 사건 순서(울음 시작 · 울음 1초 · 끝 · 답 · '안 그쳤어요' · 수유 · 힌트)를 두 구현에 넣고
  - 힌트(probs · top · top2 · confidence · top2_conf · show · show2 · n_feedback · hours_since_feed · parts)
  - 울음 끝에 남기는 미리 낸 예측(pred_base)과 맥락 특징(c)
가 같은지 본다. 소리 비교는 C++에 아직 없으므로 Python도 소리를 넣지 않는다(hint(embs=[]), track_from을 크게).

사용법(저장소 루트에서):
  python atlas/nuni/tests/cry_log_parity.py --replay "<cry_log_replay 실행 명령>" [--seeds 5] [--episodes 150]
  예) --replay "env TZ=KST-9 ./build/native/cry_log_replay"
  맥락 특징은 현지 시각(시간대)을 쓰므로 두 쪽의 시간대를 같게 둔다(위 예는 둘 다 한국 시각).
  basis·ctx는 cry_model/artifacts에서 읽는다(--artifacts로 바꿀 수 있다).
"""
import argparse
import json
import os
import random
import shlex
import subprocess
import sys

import numpy as np

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
sys.path.insert(0, ROOT)
from cry_personal import ACTIONS3, EXTRA, CryPersonal  # noqa: E402

HINT_KEYS = ["probs", "top", "top2", "confidence", "top2_conf", "show", "show2", "n_feedback", "hours_since_feed"]


def scenario(seed, n_ep):
    """여러 밤에 걸친 울음 n_ep개. 답 없음 · 기록만 하는 답 · '안 그쳤어요' · 다시 답하기 · 수유 기록을 섞는다."""
    rng = random.Random(seed)
    t = 1792411200.0 + rng.uniform(0, 86400)
    ops = []
    for i in range(n_ep):
        t += rng.uniform(300, 5 * 3600)
        if rng.random() < 0.3:
            ops.append({"op": "feed", "t": round(t - rng.uniform(0, 1800), 3)})
        ops.append({"op": "start", "t": round(t, 3)})
        ops.append({"op": "hint", "t": round(t, 3)})
        n = rng.randint(3, 90)
        ops += [{"op": "window"}] * n
        t1 = round(t + n + rng.uniform(0, 30), 3)
        ops.append({"op": "end", "t": t1})
        r = rng.random()
        if r < 0.12:
            pass                                                   # 답 없음
        elif r < 0.2:
            ops.append({"op": "not", "index": i, "action": rng.choice(ACTIONS3)})
        else:
            if rng.random() < 0.2:
                ops.append({"op": "not", "index": i, "action": rng.choice(ACTIONS3)})
            ops.append({"op": "feedback", "index": i, "action": rng.choice(ACTIONS3 * 4 + EXTRA)})
        if i > 3 and rng.random() < 0.05:                          # 예전 울음 답 바꾸기
            ops.append({"op": "feedback", "index": rng.randrange(i), "action": rng.choice(ACTIONS3)})
        if rng.random() < 0.15:
            ops.append({"op": "hint", "t": round(t1 + rng.uniform(0, 3600), 3)})
        t = t1
    return ops


def run_python(ops, basis, ctx):
    cp = CryPersonal(basis, ctx_path=ctx, blocks=["yamnet16"], track_from=10 ** 9)
    emb = np.zeros(1024, np.float32)
    out = []
    for o in ops:
        op = o["op"]
        if op == "start":
            cp.start(o["t"])
        elif op == "window":
            cp.add_window(emb)
        elif op == "end":
            i = cp.end(o["t"])
            ep = cp.episodes[i]
            out.append({"op": "end", "index": i, "pred_base": ep["pred_base"], "c": ep["c"]})
        elif op == "feedback":
            cp.feedback(o["index"], o["action"])
        elif op == "not":
            cp.feedback_not(o["index"], o["action"])
        elif op == "feed":
            cp.log_feed(o["t"])
        elif op == "hint":
            h = cp.hint(o["t"], embs=[], mfcc=None)
            out.append({"op": "hint", **h})
    return out


def run_cpp(ops, replay_cmd, ctx, workdir):
    cmd = shlex.split(replay_cmd) + [workdir, ctx]
    inp = "\n".join(json.dumps(o) for o in ops) + "\n"
    res = subprocess.run(cmd, input=inp.encode(), capture_output=True, check=True)
    lines = [json.loads(x) for x in res.stdout.decode("utf-8").splitlines() if x.strip()]
    return [x for x in lines if x["op"] in ("hint", "end")]


def same(a, b, tol=1e-9):
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(same(a[k], b[k], tol) for k in a)
    if isinstance(a, (list, tuple)) and isinstance(b, (list, tuple)):
        return len(a) == len(b) and all(same(x, y, tol) for x, y in zip(a, b))
    if isinstance(a, bool) or isinstance(b, bool):
        return a == b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return abs(a - b) <= tol
    return a == b


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--replay", required=True, help="cry_log_replay 실행 명령(인자 포함)")
    ap.add_argument("--replay-dir", default="/tmp/nuni_cry_parity", help="replay가 쓸 기록 폴더(replay 쪽 경로)")
    ap.add_argument("--replay-ctx", default="", help="replay 쪽에서 본 cry_personal_ctx.json 경로")
    ap.add_argument("--artifacts", default=os.path.join(ROOT, "cry_model", "artifacts"))
    ap.add_argument("--seeds", type=int, default=5)
    ap.add_argument("--episodes", type=int, default=150)
    a = ap.parse_args()
    basis = os.path.join(a.artifacts, "cry_personal_basis.npz")
    ctx = os.path.join(a.artifacts, "cry_personal_ctx.json")
    total = bad = 0
    for seed in range(a.seeds):
        ops = scenario(seed, a.episodes)
        py = run_python(ops, basis, ctx)
        cpp = run_cpp(ops, a.replay, a.replay_ctx or ctx, f"{a.replay_dir}/{seed}")
        if len(py) != len(cpp):
            print(f"seed {seed}: 결과 수가 다름 Python {len(py)} / C++ {len(cpp)}")
            bad += 1
            continue
        for p, c in zip(py, cpp):
            total += 1
            if p["op"] == "end":
                ok = p["index"] == c.get("index") and p["pred_base"] == c.get("pred_base") and \
                    same(p["c"], c.get("c"), 1e-4)
            else:
                ok = all(same(p[k], c.get(k)) for k in HINT_KEYS) and \
                    same(p["parts"]["context"], c["parts"]["context"]) and \
                    same(p["parts"]["history"], c["parts"]["history"])
            if not ok:
                bad += 1
                if bad <= 5:
                    print(f"seed {seed} 다름:\n  Python {json.dumps(p, ensure_ascii=False)[:400]}\n"
                          f"  C++    {json.dumps(c, ensure_ascii=False)[:400]}")
    print(f"비교 {total}건, 다름 {bad}건")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
