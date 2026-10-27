"""메시지 버스 추상화.

기본(inproc): 단일 프로세스 안에서 pub/sub. 브로커 설치 없이 바로 실행.
분산(mqtt):  환경변수 NUNI_BUS=mqtt 로 전환. 로컬 Mosquitto 필요.
             -> 팀 병렬 개발 / 실물 배포 시 사용. 코드는 그대로.
"""
import os
import json
import threading


def topic_matches(pattern: str, topic: str) -> bool:
    """MQTT 스타일 와일드카드 매칭 (#, +)."""
    if pattern == "#":
        return True
    p = pattern.split("/")
    t = topic.split("/")
    for i, seg in enumerate(p):
        if seg == "#":
            return True
        if i >= len(t):
            return False
        if seg == "+":
            continue
        if seg != t[i]:
            return False
    return len(p) == len(t)


class InprocBus:
    def __init__(self):
        self.subs = []
        self.lock = threading.Lock()

    def subscribe(self, pattern, cb):
        with self.lock:
            self.subs.append((pattern, cb))

    def publish(self, topic, payload):
        with self.lock:
            subs = list(self.subs)
        for pat, cb in subs:
            if topic_matches(pat, topic):
                cb(topic, payload)


class MqttBus:
    def __init__(self, host):
        import paho.mqtt.client as mqtt
        self.subs = []
        self.lock = threading.Lock()
        # paho-mqtt 2.x는 CallbackAPIVersion 인자를 요구한다. VERSION1을 지정하면
        # 기존 콜백 시그니처(on_message(client, userdata, msg))가 그대로 유효하다.
        # 1.x에는 이 enum이 없으므로 인자 없이 생성한다.
        _cbv = getattr(mqtt, "CallbackAPIVersion", None)
        self.c = mqtt.Client(_cbv.VERSION1) if _cbv else mqtt.Client()
        self.c.on_message = self._on
        self.c.on_connect = self._on_connect
        self.c.connect(host, 1883, 60)
        self.c.loop_start()

    def _on_connect(self, client, userdata, flags, rc):
        """재접속 때 구독을 복원한다. 브로커가 재시작되거나 네트워크가 끊겼다
        붙으면 구독이 사라지는데, 이를 복원하지 않으면 서비스는 살아있는데
        메시지만 영영 안 오는 상태가 된다."""
        with self.lock:
            patterns = [p for p, _ in self.subs]
        for p in patterns:
            client.subscribe(p)

    def subscribe(self, pattern, cb):
        with self.lock:
            self.subs.append((pattern, cb))
        self.c.subscribe(pattern)

    def publish(self, topic, payload):
        self.c.publish(topic, json.dumps(payload))

    def _on(self, client, userdata, msg):
        raw = msg.payload.decode(errors="replace")
        try:
            payload = json.loads(raw)
        except json.JSONDecodeError:
            # ESPHome 등 외부 발행자는 "ON"/"OFF"·순수 숫자 같은 비JSON 문자열을
            # 그대로 보낸다. 여기서 예외가 새면 이 스레드 전체가 조용히 죽어서
            # "#" 구독 콜백이 전부 멈추므로(실측으로 확인됨), 원본 문자열로
            # 넘기고 계속 진행한다.
            payload = raw
        with self.lock:
            subs = list(self.subs)
        for pat, cb in subs:
            if topic_matches(pat, msg.topic):
                cb(msg.topic, payload)


def _make():
    if os.getenv("NUNI_BUS", "inproc") == "mqtt":
        return MqttBus(os.getenv("NUNI_MQTT_HOST", "localhost"))
    return InprocBus()


_bus = _make()


def subscribe(pattern, cb):
    _bus.subscribe(pattern, cb)


def publish(topic, payload):
    _bus.publish(topic, payload)
