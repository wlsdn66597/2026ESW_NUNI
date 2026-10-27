"""울음-맥락 상관 분석.

울음 '이유'를 분류하는 대신, 울음이 발생한 순간에 자주 동반되는 맥락(환경 조건·시간대)을
찾아 선제 대응의 근거로 삼는다. 이유 분류(불확실)에 의존하지 않고도 유용성을 제공한다.
"""
import numpy as np


def analyze(records):
    """records: [{minute, cry(0/1), humidity, co2, temp}, ...] → 상관 요약/인사이트."""
    cry = [r for r in records if r.get("cry")]
    non = [r for r in records if not r.get("cry")]
    out = {"cry_minutes": len(cry), "env": {}, "insights": []}
    if not cry or not non:
        out["insights"].append("표본 부족으로 상관 분석 불가")
        return out

    specs = [("humidity", "습도", "%"), ("co2", "CO₂", "ppm"), ("temp", "온도", "℃")]
    for key, label, unit in specs:
        mc = float(np.mean([r[key] for r in cry]))
        mn = float(np.mean([r[key] for r in non]))
        out["env"][key] = {"cry": round(mc, 1), "baseline": round(mn, 1), "diff": round(mc - mn, 1)}
        if key == "humidity" and mc < mn - 5:
            out["insights"].append(f"울음 시 평균 습도 {mc:.0f}% (평상시 {mn:.0f}%) → 건조와 연관 가능")
        if key == "co2" and mc > mn + 80:
            out["insights"].append(f"울음 시 평균 CO₂ {mc:.0f}ppm (평상시 {mn:.0f}) → 환기 필요와 연관 가능")
        if key == "temp" and abs(mc - mn) > 1.0:
            direction = "높음" if mc > mn else "낮음"
            out["insights"].append(f"울음 시 평균 온도 {mc:.1f}℃ (평상시 {mn:.1f}) → 온도 {direction}과 연관 가능")

    by_hour = {}
    for r in cry:
        h = r["minute"] // 60
        by_hour[h] = by_hour.get(h, 0) + 1
    out["cry_by_hour"] = by_hour
    if by_hour:
        peak_h = max(by_hour, key=by_hour.get)
        out["insights"].append(f"울음이 {peak_h}시간대에 가장 잦음({by_hour[peak_h]}회)")
    if not out["insights"]:
        out["insights"].append("뚜렷한 환경·시간 상관은 관측되지 않음")
    return out
