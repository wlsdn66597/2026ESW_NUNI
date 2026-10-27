"""수면/각성 상태 분류 (일상 모니터링 헤드라인).

레이더 움직임 + 울음 '감지'(is_crying)로 일상 상태를 도출한다.
울음 '이유' 분류에는 의존하지 않는다(감지만 사용).

state: calm_sleep(안정 수면) / restless(뒤척임) / awake(각성)
       / unknown(재실 없음) / hold(판정 보류 — 사람은 있으나 호흡 판단 불가)

'재실 없음'과 '판정 보류'는 다르다. 둘을 같은 unknown으로 묶으면 사람이 앞에
있는데도 화면에 "재실 없음"이 떠 모순으로 보인다(실측에서 확인됨).
"""
import os

MOVE_RESTLESS = 0.4   # 이 이상이면 뒤척임
MOVE_AWAKE = 0.7      # 이 이상이면 각성
CRY_CONFIRM_THRESHOLD = float(os.getenv("NUNI_FUSION_CRY_THRESHOLD", "0.7"))

KO_LABEL = {
    "calm_sleep": "낮은 움직임",
    "restless": "뒤척임",
    "awake": "각성",
    "unknown": "관측 불가",
    "hold": "판정 보류",
}


def classify(radar, cry):
    """→ (state, reason)"""
    if not radar or not radar.get("valid", True) or not radar.get("presence"):
        return "unknown", "레이더에서 대상을 확인하지 못함"

    breathing_rate = radar.get("breathing_rate", radar.get("breathing_bpm"))
    # 실물 레이더가 호흡을 못 재는 구간(breath_valid=False)의 0은 '무호흡'이 아니라
    # '미측정'이다. 이때 낮은 호흡수를 이상으로 보면 오탐이 된다.
    # (breath_valid 없는 합성/기존 데이터는 True로 간주해 기존 동작 유지)
    br_valid = radar.get("breath_valid", False)
    if radar.get("apnea") or (
        br_valid and isinstance(breathing_rate, (int, float)) and breathing_rate < 8
    ):
        return "hold", "호흡 이상 감지로 수면 판정 보류"
    if not br_valid:
        return "hold", "호흡 신호 미확보로 판정 보류"

    crying = bool(
        cry
        and cry.get("is_crying")
        and cry.get("confidence", 1.0) >= CRY_CONFIRM_THRESHOLD
    )
    movement = radar.get("movement", 0.0)

    if crying or movement >= MOVE_AWAKE:
        return "awake", "울음 또는 큰 움직임"
    if movement >= MOVE_RESTLESS:
        return "restless", "움직임 증가"
    if not radar.get("movement_valid",False): return "hold", "움직임 신호 미확보"
    return "calm_sleep", "호흡 신호 측정·낮은 움직임(수면 여부 미확인)"
