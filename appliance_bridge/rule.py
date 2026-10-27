"""Temperature-drop DEMONSTRATION rule, not a room-care recommendation."""
import math

ROLES = ("air_conditioner", "air_purifier")
PREFIX = "demo/appliances"


def number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


class TemperatureRule:
    def __init__(self, on_below=22.0, off_at=22.5, hold=3.0, stale=20.0):
        if not (number(on_below) and number(off_at) and off_at > on_below and hold >= 0 and stale > 0):
            raise ValueError("invalid temperature rule")
        self.on_below, self.off_at, self.hold, self.stale = on_below, off_at, hold, stale
        self.on = False
        self.reason = "waiting_for_environment"
        self.temp = None
        self.sample_id = None
        self._sample_at = self._received = self._pending = None

    def invalidate(self, reason):
        self.on = False
        self.reason = reason
        self._received = self._pending = None

    def update(self, data, wall, mono):
        temp, ts, age = data.get("temp"), data.get("ts"), data.get("age_s", 0)
        sample = data.get("sample_id")
        valid = (data.get("valid") is True and number(temp) and -10 <= temp <= 60
                 and number(ts) and number(age) and -2 <= wall - ts <= self.stale
                 and 0 <= age <= self.stale and isinstance(sample, str) and 0 < len(sample) < 200)
        if not valid:
            self.invalidate("invalid_or_stale_environment")
            return False
        # Repeated forwarding of one source sample must not refresh its lifetime.
        if sample == self.sample_id:
            return False
        self.temp, self.sample_id = float(temp), sample
        self._sample_at = mono - max(0, wall - ts, age)
        self._received = mono
        self.tick(mono)
        return True

    def tick(self, mono, connected=True):
        if not connected:
            self.invalidate("broker_disconnected")
        elif self._received is None:
            self.on = False
        elif mono - self._sample_at > self.stale:
            self.invalidate("environment_timeout")
        elif self.temp >= self.off_at:
            self.on = False; self._pending = None; self.reason = "temperature_recovered"
        elif not self.on and self.temp < self.on_below:
            if self._pending is None: self._pending = mono
            self.on = mono - self._pending >= self.hold
            self.reason = "temperature_below_demo_threshold" if self.on else "confirming_temperature_drop"
        elif not self.on:
            self._pending = None; self.reason = "standby"
        return self.on


class HumidityRule:
    """SCD41 humidity hysteresis for the USB humidifier relay."""
    def __init__(self, on_below=35.0, off_at=40.0, hold=3.0, stale=20.0):
        if not (number(on_below) and number(off_at) and 0 <= on_below < off_at <= 100
                and number(hold) and hold >= 0 and number(stale) and stale > 0):
            raise ValueError("invalid humidity rule")
        self.on_below, self.off_at, self.hold, self.stale = on_below, off_at, hold, stale
        self.on = False
        self.reason = "waiting_for_environment"
        self.humidity = self.sample_id = None
        self._sample_at = self._pending = None

    def invalidate(self, reason):
        self.on = False; self.reason = reason
        self._sample_at = self._pending = None

    def update(self, data, wall, mono):
        humidity, ts, age = data.get("humidity"), data.get("ts"), data.get("age_s", 0)
        sample = data.get("sample_id")
        if not (data.get("valid") is True and number(humidity) and 0 <= humidity <= 100
                and number(ts) and number(age) and -2 <= wall-ts <= self.stale
                and 0 <= age <= self.stale and isinstance(sample, str) and 0 < len(sample) < 200):
            self.invalidate("invalid_or_stale_environment"); return False
        if sample == self.sample_id: return False
        self.humidity, self.sample_id = float(humidity), sample
        self._sample_at = mono-max(0, wall-ts, age)
        self.tick(mono); return True

    def tick(self, mono, connected=True):
        if not connected: self.invalidate("broker_disconnected")
        elif self._sample_at is None: self.on = False
        elif mono-self._sample_at > self.stale: self.invalidate("environment_timeout")
        elif self.humidity >= self.off_at:
            self.on = False; self._pending = None; self.reason = "humidity_recovered"
        elif not self.on and self.humidity < self.on_below:
            if self._pending is None: self._pending = mono
            self.on = mono-self._pending >= self.hold
            self.reason = "humidity_below_threshold" if self.on else "confirming_low_humidity"
        elif not self.on: self._pending = None; self.reason = "standby"
        return self.on


def validate_command(data, role, wall):
    if (not isinstance(data, dict) or data.get("v") != 1 or data.get("role") != role
            or type(data.get("on")) is not bool or data.get("mode") != "thinq_sdk_mock"
            or not isinstance(data.get("id"), str) or not 0 < len(data["id"]) <= 100):
        return False
    issued, expires = data.get("issued_at"), data.get("expires_at")
    return (number(issued) and number(expires) and -2 <= wall - issued <= 8
            and 0 < expires - wall <= 8 and 0 < expires - issued <= 8)
