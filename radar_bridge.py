"""MR60BHA2(XIAO ESP32C6, ESPHome) → sensor/radar 브리지.

레이더 키트의 ESPHome 펌웨어(deploy/mr60bha2.yaml)가 상태를 MQTT로 발행하면,
이 브리지가 구독해 숨결지기 표준 스키마(sensor/radar)로 1초 주기 재발행한다.
상위 계층(fusion·대시보드)은 무수정.

유도 처리(MR60BHA2가 직접 안 주는 값):
- movement: 컴포넌트가 움직임 값을 안 주므로 distance 변동폭으로 유도한다
  (최근 창의 max-min 이 클수록 큰 움직임 = 뒤척임).
- apnea: 원본 호흡값이 APNEA_LOW_BR 이하인 상태가 APNEA_HOLD_S 이상 이어지면 True
  (2026-11-05 결정: 5회/분 이하 20초. 카운트 중 재실이 꺼져도 계속 세고, 난간 ToF가 바뀌면 멈춘다).
- 화면 표시: 방금 값이 게이트에서 떨어져도 창에 유효값이 남아 있으면 그 중앙값을 유지한다.

안정성:
- ESPHome 수신이 STALE_S 이상 끊기거나 호흡수가 NaN이면 발행을 멈춘다
  (오래된 값으로 '정상'을 주장하지 않기 위함; 공백은 recorder 커버리지에 드러남).
- 전환 절차: 이 브리지를 켜면 nuni-edge 의 가상 레이더는 NUNI_SIM_RADAR=0 으로 끈다.

실행: python radar_bridge.py   (systemd: nuni-radar-bridge — 레이더 도착 후 enable)
"""
import collections
import json
import math
import os
import statistics
import time

import paho.mqtt.client as mqtt

import topics

PREFIX = os.getenv("NUNI_ESPHOME_PREFIX", "mr60bha2")
# ESPHome state_topic(YAML에서 명시) → 내부 키
TOPICS = {
    f"{PREFIX}/breath_rate": "breath",     # 회/분
    f"{PREFIX}/heart_rate": "heart",       # 참고 기록용
    f"{PREFIX}/presence": "presence",      # ON/OFF (has_target)
    f"{PREFIX}/distance": "distance",      # cm (움직임 유도용)
}

APNEA_HOLD_S = float(os.getenv("NUNI_APNEA_HOLD_S", "20"))     # 낮은 호흡이 이만큼 이어지면 무호흡
APNEA_LOW_BR = float(os.getenv("NUNI_APNEA_LOW_BR", "5"))      # 원본이 이 값 이하이면 '호흡 약함'으로 센다
# 이 시간 안에 유효 호흡을 관측한 적이 있어야 카운트를 시작한다(미측정과 구분).
APNEA_REQUIRE_PRIOR_S = float(os.getenv("NUNI_APNEA_PRIOR_S", "120"))
TOF_LEAVE_MM = float(os.getenv("NUNI_TOF_LEAVE_MM", "150"))    # 카운트 중 난간 ToF가 이만큼 바뀌면 이탈
TOF_STALE_S = float(os.getenv("NUNI_TOF_STALE_S", "3"))        # 이보다 오래된 ToF는 '모름'
STALE_S = float(os.getenv("NUNI_RADAR_STALE_S", "10"))
MOVE_WINDOW_S = float(os.getenv("NUNI_MOVE_WINDOW_S", "2.0"))
MOVE_SCALE_CM = float(os.getenv("NUNI_MOVE_SCALE_CM", "30"))   # 이만큼 변동 = 움직임 1.0
HOST = os.getenv("NUNI_MQTT_HOST", "localhost")

# --- 호흡값 신뢰 게이팅 ---
# 레이더 칩은 조건이 나쁘면(멀거나·움직이거나) 0이나 튄 값을 그대로 뱉는다. 그대로
# 흘리면 호흡수가 요동치고 "가만히 있는데 무호흡 경보"가 뜬다. 아래 조건을 모두
# 만족하는 값만 유효로 보고, 유효값들의 중앙값을 발행한다.
BR_MIN = float(os.getenv("NUNI_BR_MIN", "6"))          # 이 미만은 미검출(5 이하는 무호흡 카운트가 다룸)
BR_MAX = float(os.getenv("NUNI_BR_MAX", "60"))         # 영아 상한(성인 대상이면 더 좁혀도 됨)
DIST_MIN_CM = float(os.getenv("NUNI_DIST_MIN_CM", "0"))     # 이 값 '초과'만 유효(대상이 없으면 0.00)
DIST_MAX_CM = float(os.getenv("NUNI_DIST_MAX_CM", "100"))   # 호흡 유효거리
MOVE_REJECT = float(os.getenv("NUNI_MOVE_REJECT", "0.5"))   # 이보다 크게 움직이면 호흡값 신뢰 불가
SMOOTH_WINDOW_S = float(os.getenv("NUNI_BR_SMOOTH_S", "8"))  # 이 창의 중앙값을 발행
                                                             # (창이 유지되는 동안은 잠깐 결측이어도 값이 유지됨)

_state = {"breath": None, "heart": None, "presence": None,
          "distance": None, "last_rx": 0.0}
_dist_hist = collections.deque()            # (ts, distance_cm)
_br_hist = collections.deque()              # (ts, bpm) — 게이팅 통과한 유효값만(최근 창)
_low_since = [None]                         # 원본이 APNEA_LOW_BR 이하로 떨어진 시각(카운트 중일 때만)
_low_baseline = [None]                      # 카운트를 시작할 때의 마지막 유효 호흡 시각(대상이 사라져도 유지)
_last_valid = [None]                        # 마지막 유효 호흡 시각(창 밖까지 기억 — 무호흡/미측정 구분용)
_tof = {"mm": None, "rx": -1, "ref": None}  # 난간 ToF: 최근 유효 거리·수신 시각·카운트 시작 때 거리


_field_rx = {k: -1 for k in ("breath", "heart", "presence", "distance")}
_used_breath = [-1]
_breath_seq = [0]

def on_esphome(topic, raw, now=None):
    """ESPHome 상태값(평문) 수신 → 내부 상태 갱신."""
    key = TOPICS.get(topic)
    if key is None:
        return
    now = time.monotonic() if now is None else now
    if key == "presence":
        _state[key] = raw.strip().upper() in ("ON", "TRUE", "1")
    else:
        try:
            val = float(raw)
        except ValueError:
            return
        _state[key] = val
        # 0.00은 '대상 없음'이지 위치가 아니므로 움직임 계산에 넣지 않는다.
        if key == "distance" and not math.isnan(val) and val > 0:
            _dist_hist.append((now, val))
    _state["last_rx"] = now
    _field_rx[key] = now
    if key == "breath": _breath_seq[0] += 1


def _movement(now):
    """최근 창의 거리 변동폭으로 움직임(0~1)을 유도."""
    while _dist_hist and now - _dist_hist[0][0] > MOVE_WINDOW_S:
        _dist_hist.popleft()
    if len(_dist_hist) < 2:
        return 0.1
    ds = [d for _, d in _dist_hist]
    return max(0.0, min(1.0, (max(ds) - min(ds)) / MOVE_SCALE_CM))


def in_range(dist):
    return dist is not None and not math.isnan(dist) and DIST_MIN_CM < dist <= DIST_MAX_CM


def breath_is_trustworthy(bpm, dist, presence, move):
    """이 순간의 호흡값을 믿을 수 있는가(측정 조건이 갖춰졌는가)."""
    if not presence or bpm is None or math.isnan(bpm):
        return False
    if not (BR_MIN <= bpm <= BR_MAX):        # 0·튄 값 배제
        return False
    if not in_range(dist):
        return False                          # 유효 거리 밖이면 호흡값 무의미
    return move <= MOVE_REJECT                # 크게 움직이면 호흡 신호가 묻힘


def on_tof(distance_mm, now=None):
    """난간 ToF 측정 하나. 무효면 None(→ 변화 여부를 '모름'으로 둔다). 무호흡 카운트의 이탈 판단에만 쓴다."""
    now = time.monotonic() if now is None else now
    _tof["rx"] = now
    ok = isinstance(distance_mm, (int, float)) and math.isfinite(distance_mm) and distance_mm > 0
    _tof["mm"] = float(distance_mm) if ok else None


def smoothed_breath(now):
    """최근 창의 유효 호흡값 중앙값. 유효값이 없으면 None.
    중앙값이라 한두 개 튄 값에 흔들리지 않는다."""
    while _br_hist and now - _br_hist[0][0] > SMOOTH_WINDOW_S:
        _br_hist.popleft()
    if not _br_hist:
        return None
    return statistics.median(b for _, b in _br_hist)


def _stop_low_breath():
    _low_since[0] = None
    _low_baseline[0] = None
    _tof["ref"] = None


def update_low_breath(raw, presence, dist, mv, move, now):
    """원본 호흡값이 APNEA_LOW_BR 이하인 시간을 잰다.

    MR60은 호흡이 정상이어도 값이 1씩 미끄러져 1~4까지 내려갔다가 6~7초 안에 돌아오는
    구간이 있다(2026-11-04 실측). 그래서 순간값이 아니라 지속 시간(APNEA_HOLD_S)으로 판정한다.
      - 시작: 재실·유효 거리·정지, 그리고 직전에 유효 호흡을 본 적이 있음(미측정과 구분)
      - 카운트 중 재실이 꺼져도 계속(숨이 멈추면 레이더가 대상을 놓칠 수 있다)
      - 난간 ToF가 크게 바뀌면(안아 올림·일어남) 이탈로 보고 멈춤. ToF를 모르면 계속 센다
      - 레이더 수신이 끊기면(raw 없음) 셀 수 없으므로 멈춘다
      - 마지막 유효 호흡 후 APNEA_REQUIRE_PRIOR_S가 지나면 판정을 끝낸다(예전 동작, 정지 물체 오경보 방지)
    """
    low = raw is not None and not math.isnan(raw) and raw <= APNEA_LOW_BR
    if not low:
        _stop_low_breath(); return
    tof_known = _tof["mm"] is not None and _tof["rx"] >= 0 and 0 <= now - _tof["rx"] <= TOF_STALE_S
    if _low_since[0] is None:
        had_baseline = _last_valid[0] is not None and (now - _last_valid[0]) <= APNEA_REQUIRE_PRIOR_S
        if presence and in_range(dist) and mv and move <= MOVE_REJECT and had_baseline:
            _low_since[0] = now
            _low_baseline[0] = _last_valid[0]
            if tof_known: _tof["ref"] = _tof["mm"]
        return
    if _low_baseline[0] is not None and now - _low_baseline[0] > APNEA_REQUIRE_PRIOR_S:
        _stop_low_breath(); return
    if tof_known:
        if _tof["ref"] is None: _tof["ref"] = _tof["mm"]
        elif abs(_tof["mm"] - _tof["ref"]) >= TOF_LEAVE_MM:
            _stop_low_breath(); return
    if presence and mv and move > MOVE_REJECT:   # 대상이 보이는 채로 크게 움직이면 숨이 멈춘 것이 아니다
        _stop_low_breath()


def step(now=None):
    """현재 수신 상태 → sensor/radar 메시지. 데이터 없거나 stale이면 None."""
    now = time.monotonic() if now is None else now
    fresh=lambda k: _field_rx[k] >= 0 and 0 <= now-_field_rx[k] <= STALE_S
    pv=fresh('presence') and _state['presence'] is not None
    presence=pv and _state['presence']
    dist=_state['distance'] if fresh('distance') else None
    move=_movement(now); mv=fresh('distance') and len(_dist_hist)>=2
    raw=_state['breath'] if fresh('breath') else None
    valid=mv and breath_is_trustworthy(raw,dist,presence,move)
    if _field_rx['breath'] != _used_breath[0]:
        _used_breath[0]=_field_rx['breath']
        if valid: _br_hist.append((_field_rx['breath'],float(raw)));_last_valid[0]=_field_rx['breath']
    update_low_breath(raw,presence,dist,mv,move,now)
    av=fresh('breath')   # 호흡 원본이 들어오고 있어야 판정할 수 있다
    low_s=None if _low_since[0] is None else now-_low_since[0]
    apnea=bool(av and low_s is not None and low_s >= APNEA_HOLD_S)
    smooth=smoothed_breath(now)
    # 방금 값이 게이트에서 떨어져도 창에 유효값이 남아 있는 동안은 그 중앙값을 유지한다(깜빡임 방지).
    # 호흡이 약하게 잡히는 중(low_s)에는 유지값 대신 경과 시간을 보여 주도록 breath_valid를 내린다.
    bv=smooth is not None and low_s is None
    out=smooth if bv else 0.0
    msg=topics.radar_msg(round(out,1),round(move,2),bool(presence))
    msg.update(valid=bool(pv),presence_valid=bool(pv),movement_valid=bool(mv),apnea_valid=bool(av),breath_valid=bool(bv),low_breath_s=low_s,apnea_hold_s=APNEA_HOLD_S,apnea_low_br=APNEA_LOW_BR,apnea=apnea,motion=move>.5,source='mr60bha2',heart_rate=_state['heart'] if fresh('heart') else None,distance=dist,raw_breath=raw,sample_id=_breath_seq[0],breath_age_s=None if _field_rx['breath']<0 else now-_field_rx['breath'],distance_age_s=None if _field_rx['distance']<0 else now-_field_rx['distance'],presence_age_s=None if _field_rx['presence']<0 else now-_field_rx['presence'])
    # 낮은 호흡값 하나로 바로 경보하지 않는다. 경보는 무호흡(지속 시간 판정)일 때만이다.
    msg['assessment']=dict(breath_abnormal=apnea,borderline=bool(bv and (8<=out<=15 or out>55)),movement_high=bool(presence and mv and move>.6),level='alert' if apnea else 'normal')
    return msg


def main():
    _cbv = getattr(mqtt, "CallbackAPIVersion", None)
    c = mqtt.Client(_cbv.VERSION1) if _cbv else mqtt.Client()
    c.on_message = lambda cl, ud, m: None if m.retain else on_esphome(m.topic, m.payload.decode("utf-8", "replace"))
    # 구독은 반드시 on_connect 안에서 한다. connect() 직후에 subscribe()하면 CONNACK 이전이라
    # 브로커가 무시할 수 있고(부팅 시 실측으로 데이터가 전혀 안 들어온 사례 있음),
    # 재접속 때도 구독이 복원되지 않는다.
    c.on_connect = lambda cl, ud, flags, rc: [cl.subscribe(t) for t in TOPICS]
    c.connect(HOST, 1883, 60)
    c.loop_start()
    print(f"[radar-bridge] ESPHome '{PREFIX}/*' -> {topics.RADAR} (stale {STALE_S}s)")
    warned = False
    while True:
        m = step()
        if m is not None:
            c.publish(topics.RADAR, json.dumps(m))
            warned = False
        elif not warned:
            print("[radar-bridge] ESPHome 수신 대기 중 (레이더 미연결/stale)")
            warned = True
        time.sleep(1)


if __name__ == "__main__":
    main()
