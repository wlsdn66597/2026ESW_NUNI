"""py_replay.py(원본)와 nuni_replay(C++)의 출력을 줄 단위로 비교한다.

    python3 compare.py scenario.jsonl py_out.jsonl cpp_out.jsonl [--tol 0]

숫자는 기본으로 완전히 같아야 한다(--tol로 허용 오차 지정). 참/거짓과 숫자는 구분한다.
NaN은 양쪽 모두 null로 바꿔 비교한다(Python json은 NaN을, C++은 표준 JSON null을 낸다).
"""
import argparse
import collections
import json
import math
import sys


def load(path):
    with open(path, encoding="utf-8") as f:
        return [json.loads(line) for line in f if line.strip()]


def norm(o):
    if isinstance(o, float) and not math.isfinite(o):
        return None
    if isinstance(o, dict):
        return {k: norm(v) for k, v in o.items()}
    if isinstance(o, list):
        return [norm(v) for v in o]
    return o


def diff(a, b, tol, path="$"):
    """첫 번째 차이를 문자열로, 같으면 None. 최대 숫자 차이도 함께 돌려준다."""
    worst = 0.0
    if isinstance(a, bool) or isinstance(b, bool):
        return (None if a is b or (type(a) is bool and type(b) is bool and a == b) else f"{path}: {a!r} != {b!r}"), 0.0
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        d = abs(a - b)
        ok = d <= tol * max(1.0, abs(a)) if tol else a == b
        return (None if ok else f"{path}: {a!r} != {b!r}"), d
    if type(a) is not type(b):
        return f"{path}: 형식 다름 {type(a).__name__} {a!r} / {type(b).__name__} {b!r}", 0.0
    if isinstance(a, dict):
        if list(a.keys()) != list(b.keys()) and set(a.keys()) != set(b.keys()):
            return f"{path}: 키 다름 {sorted(a)} / {sorted(b)}", 0.0
        for k in a:
            e, d = diff(a[k], b[k], tol, f"{path}.{k}")
            worst = max(worst, d)
            if e:
                return e, worst
        return None, worst
    if isinstance(a, list):
        if len(a) != len(b):
            return f"{path}: 길이 다름 {len(a)} / {len(b)}", 0.0
        for i, (x, y) in enumerate(zip(a, b)):
            e, d = diff(x, y, tol, f"{path}[{i}]")
            worst = max(worst, d)
            if e:
                return e, worst
        return None, worst
    return (None if a == b else f"{path}: {a!r} != {b!r}"), 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scenario")
    ap.add_argument("py")
    ap.add_argument("cpp")
    ap.add_argument("--tol", type=float, default=0.0)
    ap.add_argument("--tol-op", default="", help="이 op에만 허용 오차 적용(쉼표로 여러 개)")
    a = ap.parse_args()
    sc, py, cp = load(a.scenario), load(a.py), load(a.cpp)
    if not (len(sc) == len(py) == len(cp)):
        print(f"줄 수가 다름: 시나리오 {len(sc)}, Python {len(py)}, C++ {len(cp)}")
        sys.exit(1)
    tol_ops = set(filter(None, a.tol_op.split(",")))
    count = collections.Counter()
    fails = collections.Counter()
    worst = collections.defaultdict(float)
    shown = 0
    for i, (c, p, q) in enumerate(zip(sc, py, cp)):
        op = c["op"]
        count[op] += 1
        tol = a.tol if (not tol_ops or op in tol_ops) else 0.0
        e, d = diff(norm(p), norm(q), tol)
        worst[op] = max(worst[op], d)
        if e:
            fails[op] += 1
            if shown < 10:
                print(f"[차이] {i + 1}번째 줄 op={op}: {e}")
                print(f"   입력  {json.dumps(c, ensure_ascii=False)[:300]}")
                shown += 1
    print(f"{'op':<14}{'개수':>8}{'불일치':>8}{'최대 숫자 차이':>16}")
    for op in count:
        print(f"{op:<14}{count[op]:>8}{fails[op]:>8}{worst[op]:>16.3g}")
    total_fail = sum(fails.values())
    print(f"합계 {sum(count.values())}줄, 불일치 {total_fail}줄")
    sys.exit(1 if total_fail else 0)


if __name__ == "__main__":
    main()
