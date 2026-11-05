import asyncio
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from rule import TemperatureRule, Co2Rule, validate_command
from gateway import Device
from thinq_mock import ThinQMock, payload_for


def sample(temp, seq=1, ts=100, co2=600):
    return {"valid": True, "temp": temp, "co2": co2, "ts": ts, "age_s": 0, "sample_id": str(seq)}


def command(on=True, **kwargs):
    d = {"v": 1, "role": "air_conditioner", "mode": "thinq_sdk_mock", "id": "command-1",
         "on": on, "issued_at": 100, "expires_at": 106}
    d.update(kwargs)
    return d


class RuleTests(unittest.TestCase):
    def test_heat_requires_hold_and_recovery_has_hysteresis(self):
        r = TemperatureRule()
        r.update(sample(27), 100, 0)
        self.assertFalse(r.tick(2.9)); self.assertTrue(r.tick(3))
        r.update(sample(25, 2, 104), 104, 4)
        self.assertTrue(r.tick(4))  # still above the 24 C OFF point
        r.update(sample(24, 3, 105), 105, 5)
        self.assertFalse(r.tick(5))

    def test_cold_room_never_turns_air_conditioner_on(self):
        r = TemperatureRule(hold=0)
        for i, t in enumerate((17.6, 21, 23.9, 25.9)):
            r.update(sample(t, i + 1, 100 + i), 100 + i, i)
            self.assertFalse(r.tick(i), t)
        r.update(sample(26, 9, 105), 105, 5)
        self.assertTrue(r.tick(5))

    def test_co2_rule_drives_air_purifier_with_hysteresis(self):
        r = Co2Rule(hold=0)
        r.update(sample(22, 1, 100, co2=1000), 100, 0)
        self.assertFalse(r.tick(0))  # "over 1000 ppm": 1000 itself stays OFF
        r.update(sample(22, 2, 101, co2=1001), 101, 1)
        self.assertTrue(r.tick(1)); self.assertEqual(r.reason, "co2_above_threshold")
        r.update(sample(22, 3, 102, co2=900), 102, 2)
        self.assertTrue(r.tick(2))
        r.update(sample(22, 4, 103, co2=800), 103, 3)
        self.assertFalse(r.tick(3)); self.assertEqual(r.reason, "co2_recovered")

    def test_repeated_samples_cannot_keep_rule_alive(self):
        r = TemperatureRule(hold=0)
        r.update(sample(27), 100, 0)
        r.update(sample(27, ts=119), 119, 19)
        self.assertFalse(r.tick(21))
        self.assertEqual(r.reason, "environment_timeout")

    def test_invalid_missing_future_and_stale_inputs_force_off(self):
        for change in ({"valid": False}, {"temp": float("nan")}, {"temp": True},
                       {"ts": 80}, {"ts": 104}, {"sample_id": None}, {"age_s": 21}):
            r = TemperatureRule(hold=0); r.update(sample(27), 100, 0)
            self.assertTrue(r.on)
            d = sample(27, 2); d.update(change)
            r.update(d, 101, 1)
            self.assertFalse(r.on, change)
        for change in ({"co2": float("nan")}, {"co2": None}, {"co2": -5}):
            r = Co2Rule(hold=0); r.update(sample(22, co2=1500), 100, 0)
            self.assertTrue(r.on)
            d = sample(22, 2, co2=1500); d.update(change)
            r.update(d, 101, 1)
            self.assertFalse(r.on, change)

    def test_broker_disconnect_needs_new_source_sample(self):
        r = TemperatureRule(hold=0); r.update(sample(27), 100, 0)
        self.assertFalse(r.tick(1, False)); self.assertFalse(r.tick(2, True))
        r.update(sample(27, 2, 103), 103, 3); self.assertTrue(r.on)

    def test_malformed_or_expired_commands_rejected(self):
        self.assertTrue(validate_command(command(), "air_conditioner", 101))
        for change in ({"on": 1}, {"role": "air_purifier"}, {"mode": "real"},
                       {"issued_at": 104}, {"expires_at": 100}, {"expires_at": 1000}):
            self.assertFalse(validate_command(command(**change), "air_conditioner", 101), change)


class FakeSerial:
    def __init__(self):
        self.incoming = b""; self.written = []; self.closed = False
    def open(self): pass
    def close(self): self.closed = True
    def read(self, n):
        data, self.incoming = self.incoming[:n], self.incoming[n:]; return data
    def write(self, frame): self.written.append(json.loads(frame[7:])); return len(frame)


def state(device="chip", role="air_conditioner", on=False, boot="a" * 16):
    return b"NUNIA1 " + json.dumps({"v": 1, "kind": "state", "role": role,
        "device_id": device, "boot": boot, "seq": 0, "on": on}).encode() + b"\n"


class GatewayTests(unittest.TestCase):
    def make_device(self):
        port, published = FakeSerial(), []
        d = Device("air_conditioner", {"device_id": "chip", "port": "/dev/fake"},
                   lambda role, data: published.append(data), serial_factory=lambda: port)
        return d, port, published

    def test_no_write_before_matching_device_identity(self):
        d, port, output = self.make_device()
        d.accept(command(), 100)
        port.incoming = state(device="other")
        d.step(1, 101, True)
        self.assertFalse(port.written); self.assertIsNone(output[-1]["actual_on"])
        port.incoming = state()
        d.step(2, 102, True)
        self.assertTrue(port.written[-1]["on"])
        # Requested ON is not mistaken for observed ON before board reports it.
        self.assertIs(output[-1]["actual_on"], False)

    def test_expiry_and_network_loss_write_off(self):
        for wall, connected in ((107, True), (102, False)):
            d, port, output = self.make_device(); d.accept(command(), 100)
            port.incoming = state(); d.step(1, wall, connected)
            self.assertIs(port.written[-1]["on"], False)

    def test_usb_loss_marks_actual_unknown(self):
        d, port, output = self.make_device()
        port.incoming = state(on=True); d.step(1, 101, True)
        d.step(6, 106, True)
        self.assertTrue(port.closed); self.assertIsNone(output[-1]["actual_on"])

    def test_duplicate_command_does_not_extend_expiry(self):
        d, port, output = self.make_device()
        self.assertTrue(d.accept(command(), 100))
        self.assertFalse(d.accept(command(expires_at=108), 102))
        self.assertEqual(d.command["expires_at"], 106)


class SdkTests(unittest.IsolatedAsyncioTestCase):
    async def test_official_mock_cannot_make_network_request(self):
        with patch("aiohttp.ClientSession.request", side_effect=AssertionError("network forbidden")):
            async with ThinQMock() as api:
                for role in ("air_conditioner", "air_purifier"):
                    for on in (True, False):
                        result = await api.control(role, on)
                        self.assertFalse(result["cloud_confirmed"])
                        self.assertEqual(result["response"]["body"], payload_for(role, on))


if __name__ == "__main__": unittest.main()
