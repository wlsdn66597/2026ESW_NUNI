"""Reference policy for the wired Pi 4 / Pi 5 deployment; ages use monotonic time."""
import json
import math
import threading
import time
import bus
import topics
import env_control
import sleep_state
from personalization import Personalizer
from state_store import store

def usable(x):
    return isinstance(x, dict) and bool(x) and x.get("valid", True)

def number(x, default=0):
    return x if isinstance(x, (int, float)) and math.isfinite(x) else default

def decide(radar, cry, env):
    r, c, e = radar or {}, cry or {}, env or {}
    present = usable(r) and r.get("presence") and r.get("presence_valid", True)
    br = number(r.get("breathing_rate", r.get("breathing_bpm")))
    bv = present and r.get("breath_valid", False) and br > 0
    apnea = present and r.get("apnea_valid", False) and r.get("apnea", False)
    assessment = r.get("assessment")
    abnormal = present and (assessment.get("breath_abnormal", False) if isinstance(assessment, dict) else (apnea or (bv and br < 8)))
    motion = present and r.get("movement_valid", False) and (assessment.get("movement_high", False) if isinstance(assessment, dict) else number(r.get("movement")) > .6)
    borderline = bv and (assessment.get("borderline", False) if isinstance(assessment, dict) else (8 <= br <= 15 or br > 55))
    crying = usable(c) and c.get("is_crying") and number(c.get("confidence", c.get("cry_score"))) >= .7
    level, reasons = "normal", []
    if abnormal:
        level = "alert"; reasons.append("호흡 신호 이상 확인 필요")
    if crying:
        if level != "alert": level = "attention"
        reasons.append(f"울음 감지({c.get('cls')})")
    if crying and (abnormal or motion):
        level = "alert"; reasons.append("다중 신호 동시 이상")
    if borderline and (crying or (usable(e) and number(e.get("co2")) > 1000)):
        level = "alert"; reasons.append("경계 호흡+보강신호 교차검증")
    if usable(e):
        if number(e.get("co2")) > 1000:
            if level != "alert": level = "attention"
            reasons.append("CO₂ 높음")
        t = e.get("temp")
        if isinstance(t, (int, float)) and math.isfinite(t) and (t < 18 or t > 26):
            if level != "alert": level = "attention"
            reasons.append("실내 온도 이상")
    return level, reasons

def single_sensor_decide(radar, cry, env):
    parts = (decide(radar, None, None), decide(None, cry, None), decide(None, None, env))
    order = {"normal": 0, "attention": 1, "alert": 2}
    return max((p[0] for p in parts), key=order.get), [r for p in parts for r in p[1]]

class Fusion:
    def __init__(self, debounce_n=2, cooldown_s=10, now_fn=time.monotonic):
        self.radar = self.cry = self.env = None
        self.last = self._candidate = "normal"
        self.debounce_n, self.cooldown_s, self._now = debounce_n, cooldown_s, now_fn
        self._since = None
        self._last_alert_key = None; self._last_alert_ts = 0
        self._received = {}; self._sequences = {}; self._retired = {}
        self._personal_id = None; self.personalizer = Personalizer()
        self._lock = threading.RLock(); self.last_state = None

    def _accept(self, topic, payload):
        if not isinstance(payload, dict): return False
        if "boot_id" in payload and "seq" in payload:
            boot, seq = payload["boot_id"], payload["seq"]
            if not isinstance(boot, str) or not isinstance(seq, int): return False
            previous = self._sequences.get(topic)
            if previous:
                if previous[0] == boot and seq <= previous[1]: return False
                if previous[0] != boot:
                    if boot in self._retired.get(topic, set()): return False
                    self._retired.setdefault(topic, set()).add(previous[0])
            self._sequences[topic] = (boot, seq)
        self._received[topic] = self._now()
        return True

    def _fresh(self, topic, value, ttl):
        age = self._now() - self._received.get(topic, float('-inf'))
        return value if 0 <= age <= ttl and usable(value) else None

    def current_sensors(self):
        return dict(radar=self._fresh(topics.RADAR,self.radar,3), cry=self._fresh(topics.CRY,self.cry,3), env=self._fresh(topics.ENV,self.env,20))

    def _process(self):
        sensors=self.current_sensors(); r,c,e=sensors['radar'],sensors['cry'],sensors['env']
        raw,reasons=decide(r,c,e); now=self._now()
        if raw == 'normal': self._candidate='normal'; self._since=None; level='normal'
        else:
            if self._candidate != raw or self._since is None: self._candidate=raw; self._since=now
            level=raw if now-self._since >= self.debounce_n else 'normal'
        reasons=reasons if level != 'normal' else []
        ss,why=sleep_state.classify(r,c); actions=env_control.recommend(e,ss)
        desired={a:False for a in ('ventilate','humidify','dehumidify','cool','heat','dim_light')}
        desired.update({a:True for a,_ in actions})
        bus.publish(topics.CONTROL,dict(schema=2,mode='recommendation_only',desired=desired,actuation_enabled=False,actual=None,ts=topics.now()))
        state=dict(level=level,raw_level=raw,reasons=reasons,sleep=dict(state=ss,reason=why),personal=self.personalizer.summary(),actions=[[a,w] for a,w in actions],quality={k:v is not None for k,v in sensors.items()},ts=topics.now())
        self.last_state=state
        store.set_fusion(level,reasons);store.set_sleep_state(ss,why);store.set_personal(state['personal']);store.set_actions(actions)
        bus.publish(topics.FUSION_STATE,state)
        emitted=False; key=(level,'|'.join(reasons))
        if level != 'normal' and level != self.last and not (key == self._last_alert_key and now-self._last_alert_ts < self.cooldown_s):
            self._last_alert_key=key;self._last_alert_ts=now
            bus.publish(topics.ALERT,dict(level=level,reason=', '.join(reasons),ts=topics.now()));emitted=True
        self.last=level
        return level,reasons,emitted

    def on_message(self, topic, payload):
        with self._lock:
            if topic in (topics.RADAR,topics.CRY,topics.ENV) and self._accept(topic,payload):
                if topic == topics.RADAR:
                    self.radar=payload; br=payload.get('breathing_rate'); identity=payload.get('sample_id',payload.get('ts'))
                    if identity is not None and identity != self._personal_id and usable(payload) and payload.get('presence') and payload.get('breath_valid',False) and payload.get('movement_valid',False) and isinstance(br,(int,float)) and math.isfinite(br) and 8 <= br <= 55 and number(payload.get('movement'),1) <= .4:
                        self.personalizer.update(br,payload.get('movement'));self._personal_id=identity
                elif topic == topics.CRY: self.cry=payload
                else: self.env=payload
            return self._process()

    def tick(self):
        with self._lock: return self._process()

    def update(self, radar=None, cry=None, env=None):
        for topic,value in ((topics.RADAR,radar),(topics.CRY,cry),(topics.ENV,env)):
            if value is not None: self.on_message(topic,value)
        return self.tick()
