"""Appliance demonstrator rules (2026-11-05: one set of thresholds for slides, code and demo).

air_conditioner: temp >= 26 C for `hold` s -> ON, temp <= 24 C -> OFF (cooling)
air_purifier:    CO2 > 1000 ppm for `hold` s -> ON, CO2 <= 800 ppm -> OFF (air circulation demo)
humidifier:      humidity < 35 % for `hold` s -> ON, humidity >= 40 % -> OFF (HumidityRule)
"""
import math

ROLES = ("air_conditioner", "air_purifier")
PREFIX = "demo/appliances"


def number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


class RisingRule:
    """ON after `value` stays at/above `on_at` for `hold` s; OFF once it falls to `off_at` or below.

    `strict` makes the ON test `value > on_at` (CO2 "over 1000 ppm") instead of `>=`.
    """
    key = "value"
    valid_range = (-math.inf, math.inf)
    above = "value_above_threshold"
    confirming = "confirming_value_rise"
    recovered = "value_recovered"

    def __init__(self, on_at, off_at, hold=3.0, stale=20.0, strict=False):
        if not (number(on_at) and number(off_at) and off_at < on_at and number(hold) and hold >= 0
                and number(stale) and stale > 0):
            raise ValueError(f"invalid {self.key} rule")
        self.on_at, self.off_at, self.hold, self.stale, self.strict = on_at, off_at, hold, stale, strict
        self.on = False
        self.reason = "waiting_for_environment"
        self.value = None
        self.sample_id = None
        self._sample_at = self._received = self._pending = None

    def invalidate(self, reason):
        self.on = False
        self.reason = reason
        self._received = self._pending = None

    def update(self, data, wall, mono):
        value, ts, age = data.get(self.key), data.get("ts"), data.get("age_s", 0)
        sample = data.get("sample_id")
        lo, hi = self.valid_range
        valid = (data.get("valid") is True and number(value) and lo <= value <= hi
                 and number(ts) and number(age) and -2 <= wall - ts <= self.stale
                 and 0 <= age <= self.stale and isinstance(sample, str) and 0 < len(sample) < 200)
        if not valid:
            self.invalidate("invalid_or_stale_environment")
            return False
        # Repeated forwarding of one source sample must not refresh its lifetime.
        if sample == self.sample_id:
            return False
        self.value, self.sample_id = float(value), sample
        self._sample_at = mono - max(0, wall - ts, age)
        self._received = mono
        self.tick(mono)
        return True

    def _high(self):
        return self.value > self.on_at if self.strict else self.value >= self.on_at

    def tick(self, mono, connected=True):
        if not connected:
            self.invalidate("broker_disconnected")
        elif self._received is None:
            self.on = False
        elif mono - self._sample_at > self.stale:
            self.invalidate("environment_timeout")
        elif self.value <= self.off_at:
            self.on = False; self._pending = None; self.reason = self.recovered
        elif not self.on and self._high():
            if self._pending is None: self._pending = mono
            self.on = mono - self._pending >= self.hold
            self.reason = self.above if self.on else self.confirming
        elif not self.on:
            self._pending = None; self.reason = "standby"
        return self.on


class TemperatureRule(RisingRule):
    """Air conditioner: cool at 26 C and above, stop at 24 C and below."""
    key = "temp"
    valid_range = (-10, 60)
    above = "temperature_above_threshold"
    confirming = "confirming_temperature_rise"
    recovered = "temperature_recovered"

    def __init__(self, on_at=26.0, off_at=24.0, hold=3.0, stale=20.0):
        super().__init__(on_at, off_at, hold, stale)

    @property
    def temp(self):
        return self.value


class Co2Rule(RisingRule):
    """Air purifier demonstrator: on above 1000 ppm CO2 (air circulation), off at 800 ppm and below."""
    key = "co2"
    valid_range = (0, 40000)  # SCD41 output range
    above = "co2_above_threshold"
    confirming = "confirming_co2_rise"
    recovered = "co2_recovered"

    def __init__(self, on_above=1000.0, off_at=800.0, hold=3.0, stale=20.0):
        super().__init__(on_above, off_at, hold, stale, strict=True)

    @property
    def co2(self):
        return self.value


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
