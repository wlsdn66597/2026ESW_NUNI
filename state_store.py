"""대시보드가 읽는 스레드 안전 최신 상태 저장소."""
import threading
import time
import collections


class Store:
    def __init__(self):
        self.lock = threading.Lock()
        self.latest = {}                                    # topic -> payload
        self.radar_hist = collections.deque(maxlen=60)      # 호흡 파형
        self.events = collections.deque(maxlen=12)          # 이벤트 로그
        self.fusion = {"level": "normal", "reasons": []}
        self.sleep_state = {"state": "unknown", "reason": ""}   # 일상 수면/각성 상태
        self.actions = []                                       # 선제 환경 제어 권고
        self.personal = {}                                      # 개인화 baseline 요약
        self._inject = {}                                   # kind -> deadline(ts)

    def update(self, topic, payload):
        with self.lock:
            self.latest[topic] = payload
            if topic == "sensor/radar":
                self.radar_hist.append(payload.get("breathing_rate", 0))
            elif topic == "fusion/state":
                # 분산 모드: 대시보드는 이 메시지만으로 융합/수면/개인화/권고를 복원한다.
                self.fusion = {"level": payload.get("level", "normal"),
                               "reasons": payload.get("reasons", [])}
                if "sleep" in payload:
                    self.sleep_state = payload["sleep"]
                if "personal" in payload:
                    self.personal = payload["personal"]
                if "actions" in payload:
                    self.actions = payload["actions"]
            elif topic == "fusion/alert":
                # self.lock 보유 중이므로 log()를 재호출하지 않고 직접 추가(재진입 방지).
                self.events.appendleft(
                    time.strftime("%H:%M:%S") + "  "
                    + f"[{str(payload.get('level', '')).upper()}] {payload.get('reason', '')}")

    def set_fusion(self, level, reasons):
        with self.lock:
            self.fusion = {"level": level, "reasons": reasons}

    def set_sleep_state(self, state, reason):
        with self.lock:
            self.sleep_state = {"state": state, "reason": reason}

    def set_actions(self, actions):
        with self.lock:
            self.actions = list(actions)

    def set_personal(self, summary):
        with self.lock:
            self.personal = dict(summary)

    def log(self, msg):
        with self.lock:
            self.events.appendleft(time.strftime("%H:%M:%S") + "  " + msg)

    # --- 데모 시나리오 주입 ---
    def set_inject(self, kind, seconds):
        with self.lock:
            self._inject[kind] = time.time() + seconds

    def clear_inject(self):
        with self.lock:
            self._inject = {}

    def active_injects(self):
        now = time.time()
        with self.lock:
            return {k for k, dl in self._inject.items() if dl > now}

    def snapshot(self):
        with self.lock:
            return {
                "latest": dict(self.latest),
                "radar": list(self.radar_hist),
                "events": list(self.events),
                "fusion": dict(self.fusion),
                "sleep_state": dict(self.sleep_state),
                "actions": list(self.actions),
                "personal": dict(self.personal),
            }


store = Store()
