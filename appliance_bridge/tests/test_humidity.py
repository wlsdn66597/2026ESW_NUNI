from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from rule import HumidityRule

def sample(humidity, seq=1, ts=100):
    return {"valid": True, "humidity": humidity, "sample_id": str(seq), "ts": ts, "age_s": 0}

class HumidityTests(unittest.TestCase):
    def test_hold_hysteresis_and_recovery(self):
        r=HumidityRule();r.update(sample(30),100,0)
        self.assertFalse(r.tick(2.9));self.assertTrue(r.tick(3))
        r.update(sample(37,2,104),104,4);self.assertTrue(r.on)
        r.update(sample(40,3,105),105,5);self.assertFalse(r.on)

    def test_duplicate_source_cannot_extend_lifetime(self):
        r=HumidityRule(hold=0);r.update(sample(30),100,0)
        self.assertTrue(r.on)
        self.assertFalse(r.update(sample(30,ts=119),119,19))
        self.assertFalse(r.tick(21));self.assertEqual(r.reason,"environment_timeout")

    def test_invalid_values_turn_off(self):
        for patch in ({"humidity":False},{"humidity":float("nan")},{"humidity":101},
                      {"humidity":-1},{"valid":False},{"age_s":21},{"ts":104}):
            r=HumidityRule(hold=0);r.update(sample(30),100,0)
            d=sample(30,2);d.update(patch);r.update(d,101,1)
            self.assertFalse(r.on,patch)

    def test_disconnect_needs_new_measurement(self):
        r=HumidityRule(hold=0);r.update(sample(30),100,0)
        self.assertFalse(r.tick(1,False));self.assertFalse(r.tick(2,True))
        r.update(sample(30,2,103),103,3);self.assertTrue(r.on)

    def test_high_humidity_is_valid_off(self):
        r=HumidityRule(hold=0);r.update(sample(90),100,0)
        self.assertEqual(r.humidity,90);self.assertFalse(r.on)
        self.assertEqual(r.reason,"humidity_recovered")

if __name__ == '__main__':unittest.main()
