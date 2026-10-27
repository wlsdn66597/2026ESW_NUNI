"""MR60BHA2(XIAO ESP32C6, ESPHome) → sensor/radar 브리지.

레이더 키트의 ESPHome 펌웨어(deploy/mr60bha2.yaml)가 상태를 MQTT로 발행하면,
이 브리지가 구독해 숨결지기 표준 스키마(sensor/radar)로 1초 주기 재발행한다.
상위 계층(fusion·대시보드)은 무수정.

유도 처리(MR60BHA2가 직접 안 주는 값):
- movement: 컴포넌트가 움직임 값을 안 주므로 distance 변동폭으로 유도한다
  (최근 창의 max-min 이 클수록 큰 움직임 = 뒤척임).
- apnea: 재실 중 호흡수 < APNEA_BPM_MIN 이 APNEA_HOLD_S 이상 지속되면 True.

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

APNEA_HOLD_S = float(os.getenv("NUNI_APNEA_HOLD_S", "6"))
# 이 시간 안에 유효 호흡을 관측한 적이 있어야 무호흡 판정을 한다(미측정과 구분).
APNEA_REQUIRE_PRIOR_S = float(os.getenv("NUNI_APNEA_PRIOR_S", "120"))
STALE_S = float(os.getenv("NUNI_RADAR_STALE_S", "10"))
MOVE_WINDOW_S = float(os.getenv("NUNI_MOVE_WINDOW_S", "2.0"))
MOVE_SCALE_CM = float(os.getenv("NUNI_MOVE_SCALE_CM", "30"))   # 이만큼 변동 = 움직임 1.0
HOST = os.getenv("NUNI_MQTT_HOST", "localhost")

# --- 호흡값 신뢰 게이팅 ---
# 레이더 칩은 조건이 나쁘면(멀거나·움직이거나) 0이나 튄 값을 그대로 뱉는다. 그대로
# 흘리면 호흡수가 요동치고 "가만히 있는데 무호흡 경보"가 뜬다. 아래 조건을 모두
# 만족하는 값만 유효로 보고, 유효값들의 중앙값을 발행한다.
BR_MIN = float(os.getenv("NUNI_BR_MIN", "5"))          # 이 미만은 미검출로 취급
BR_MAX = float(os.getenv("NUNI_BR_MAX", "60"))         # 영아 상한(성인 대상이면 더 좁혀도 됨)
DIST_MIN_CM = float(os.getenv("NUNI_DIST_MIN_CM", "20"))
DIST_MAX_CM = float(os.getenv("NUNI_DIST_MAX_CM", "150"))   # 호흡 유효거리(모듈 스펙 ~1.5m)
MOVE_REJECT = float(os.getenv("NUNI_MOVE_REJECT", "0.5"))   # 이보다 크게 움직이면 호흡값 신뢰 불가
SMOOTH_WINDOW_S = float(os.getenv("NUNI_BR_SMOOTH_S", "8"))  # 이 창의 중앙값을 발행
                                                             # (창이 유지되는 동안은 잠깐 결측이어도 값이 유지됨)

_state = {"breath": None, "heart": None, "presence": None,
          "distance": None, "last_rx": 0.0}
_dist_hist = collections.deque()            # (ts, distance_cm)
_br_hist = collections.deque()              # (ts, bpm) — 게이팅 통과한 유효값만(최근 창)
_no_valid_since = [None]                    # 유효 호흡이 끊긴 시각(무호흡 유도)
_last_valid = [None]                        # 마지막 유효 호흡 시각(창 밖까지 기억 — 무호흡/미측정 구분용)


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
        if key == "distance" and not math.isnan(val):
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


def breath_is_trustworthy(bpm, dist, presence, move):
    """이 순간의 호흡값을 믿을 수 있는가(측정 조건이 갖춰졌는가)."""
    if not presence or bpm is None or math.isnan(bpm):
        return False
    if not (BR_MIN <= bpm <= BR_MAX):        # 0·튄 값 배제
        return False
    if dist is None or math.isnan(dist) or not (DIST_MIN_CM <= dist <= DIST_MAX_CM):
        return False                          # 유효 거리 밖이면 호흡값 무의미
    return move <= MOVE_REJECT                # 크게 움직이면 호흡 신호가 묻힘


def smoothed_breath(now):
    """최근 창의 유효 호흡값 중앙값. 유효값이 없으면 None.
    중앙값이라 한두 개 튄 값에 흔들리지 않는다."""
    while _br_hist and now - _br_hist[0][0] > SMOOTH_WINDOW_S:
        _br_hist.popleft()
    if not _br_hist:
        return None
    return statistics.median(b for _, b in _br_hist)


def derive_apnea(presence, dist, move, now):
    """'잡히던 호흡이 멈춘' 상태만 무호흡 의심으로 본다.

    두 가지를 구분하는 게 핵심이다:
      - 무호흡  : 호흡을 잘 재고 있었는데 갑자기 사라짐        → 경보 대상
      - 미측정  : 애초에 호흡이 안 잡힘(멀거나·비스듬하거나·물체) → 판단 보류
    구분하지 않으면 사람이 자세만 바꿔도 무호흡 경보가 뜬다(실측에서 확인됨).
    그래서 ①측정 조건(재실·유효거리·정지)이 갖춰졌고 ②직전에 유효 호흡을
    실제로 관측했던 경우에만 '호흡 없음'을 카운트한다.
    """
    measurable = (presence and dist is not None and not math.isnan(dist)
                  and DIST_MIN_CM <= dist <= DIST_MAX_CM and move <= MOVE_REJECT)
    had_baseline = (_last_valid[0] is not None
                    and (now - _last_valid[0]) <= APNEA_REQUIRE_PRIOR_S)
    if not (measurable and had_baseline):
        _no_valid_since[0] = None             # 측정 불가 or 기준 호흡 없음 → 판단 보류
        return False
    if _br_hist and now - _br_hist[-1][0] <= 1.5:
        _no_valid_since[0] = None             # 방금 유효 호흡이 잡힘
        return False
    if _no_valid_since[0] is None:
        _no_valid_since[0] = now
    return (now - _no_valid_since[0]) >= APNEA_HOLD_S


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
    av=pv and presence and fresh('breath') and mv and dist is not None
    apnea=bool(av and derive_apnea(presence,dist,move,now))
    if not av: _no_valid_since[0]=None
    smooth=smoothed_breath(now)
    out=0.0 if smooth is None or not valid or apnea else smooth
    msg=topics.radar_msg(round(out,1),round(move,2),bool(presence))
    msg.update(valid=bool(pv),presence_valid=bool(pv),movement_valid=bool(mv),apnea_valid=bool(av),breath_valid=bool(valid and smooth is not None),apnea=apnea,motion=move>.5,source='mr60bha2',heart_rate=_state['heart'] if fresh('heart') else None,distance=dist,raw_breath=raw,sample_id=_breath_seq[0],breath_age_s=None if _field_rx['breath']<0 else now-_field_rx['breath'],distance_age_s=None if _field_rx['distance']<0 else now-_field_rx['distance'],presence_age_s=None if _field_rx['presence']<0 else now-_field_rx['presence'])
    msg['assessment']=dict(breath_abnormal=bool(apnea or (valid and 0<out<8)),borderline=bool(valid and (8<=out<=15 or out>55)),movement_high=bool(presence and mv and move>.6),level='alert' if apnea or (valid and 0<out<8) else 'normal')
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
