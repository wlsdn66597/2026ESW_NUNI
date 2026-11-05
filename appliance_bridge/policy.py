"""Pi 5: fresh environment -> ThinQ SDK mock -> leased MQTT commands to Pi 4."""
import argparse
import asyncio
import json
import logging
import queue
import signal
import time
import uuid
import paho.mqtt.client as mqtt
from rule import TemperatureRule, Co2Rule, HumidityRule, DemoOverride, DEMO_TOPIC, ROLES, PREFIX
from thinq_mock import ThinQMock

LOG = logging.getLogger("nuni-appliance-policy")


def appliance_rules(args, hold=None):
    # Each demonstrator follows its own reading: air conditioner = temperature, air purifier = CO2.
    hold = args.hold if hold is None else hold
    return {"air_conditioner": TemperatureRule(args.cool_on_at, args.cool_off_at, hold),
            "air_purifier": Co2Rule(args.co2_on_above, args.co2_off_at, hold)}


async def run(args):
    real_rules = appliance_rules(args)
    # Fed only by the dashboard demo button, reset for each demo. No confirm delay: the values are deliberate.
    demo_rules = appliance_rules(args, hold=0)
    demo = DemoOverride()
    humidity = HumidityRule(args.humidity_on_below, args.humidity_off_at, args.hold)
    incoming = queue.Queue(maxsize=64)
    stopped = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM): loop.add_signal_handler(sig, stopped.set)
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="nuni-appliance-policy-" + uuid.uuid4().hex[:12])
    client.max_queued_messages_set(16)
    connected = False

    def on_connect(c, userdata, flags, code, properties):
        nonlocal connected
        connected = code == 0
        if connected:
            c.subscribe(args.topic, qos=1)
            c.subscribe(DEMO_TOPIC, qos=1)

    def on_disconnect(*unused):
        nonlocal connected
        connected = False

    def on_message(c, userdata, msg):
        if msg.retain or len(msg.payload) > 8192: return
        try:
            data = json.loads(msg.payload)
            if isinstance(data, dict): incoming.put_nowait((msg.topic, data))
        except (ValueError, queue.Full): pass

    client.on_connect, client.on_disconnect, client.on_message = on_connect, on_disconnect, on_message
    client.connect_async(args.host, args.port, keepalive=10)
    client.loop_start()
    next_send = 0
    last_state = None
    try:
        async with ThinQMock() as thinq:
            while not stopped.is_set():
                mono, wall = time.monotonic(), time.time()
                for _ in range(64):
                    try: topic, data = incoming.get_nowait()
                    except queue.Empty: break
                    if topic == DEMO_TOPIC:
                        was = demo.active(wall)
                        if demo.accept(data, wall):
                            LOG.info("appliance demo %s (%.0f s left)", data.get("action"), demo.remaining(wall))
                            if not was: demo_rules = appliance_rules(args, hold=0)
                        continue
                    for rule in real_rules.values(): rule.update(data, wall, mono)
                    humidity.update(data, wall, mono)
                demo_on = demo.active(wall)
                if demo_on:
                    for rule in demo_rules.values(): rule.update(demo.sample(wall), wall, mono)
                for rule in list(real_rules.values()) + list(demo_rules.values()): rule.tick(mono, connected)
                humidity.tick(mono, connected)
                rules = demo_rules if demo_on else real_rules
                input_topic = DEMO_TOPIC if demo_on else args.topic
                state = (demo_on,) + tuple((r.on, r.reason) for r in rules.values()) + (humidity.on, humidity.reason)
                if connected and (mono >= next_send or state != last_state):
                    for role in ROLES:
                        rule = rules[role]
                        result = await thinq.control(role, rule.on)
                        command = {"v": 1, "id": uuid.uuid4().hex, "role": role, "on": rule.on,
                            "mode": "thinq_sdk_mock", "issued_at": wall, "expires_at": wall + 6,
                            "reason": ("demo:" if demo_on else "") + rule.reason, rule.key: rule.value,
                            "sample_id": rule.sample_id, "input_topic": input_topic, "demo": demo_on,
                            "thinq": result}
                        client.publish(f"{PREFIX}/{role}/command", json.dumps(command), qos=1, retain=False)
                    client.publish(f"{PREFIX}/policy", json.dumps({"v": 1, "mode": "thinq_sdk_mock",
                        "cloud_confirmed": False, "input_topic": input_topic, "ts": wall,
                        "demo": {"active": demo_on, "remaining_s": round(demo.remaining(wall), 1)},
                        "roles": {role: {"desired_on": r.on, "reason": r.reason, "input": r.key, "value": r.value,
                                         "on_at": r.on_at, "off_at": r.off_at} for role, r in rules.items()}}),
                        retain=False)
                    if not args.demo_input:
                        client.publish("control/humidifier/command", json.dumps({"v": 1,
                            "id": uuid.uuid4().hex, "role": "humidifier", "mode": "local_relay",
                            "on": humidity.on, "sample_id": humidity.sample_id,
                            "reason": humidity.reason, "humidity": humidity.humidity}), qos=1, retain=False)
                        client.publish("control/humidifier/policy", json.dumps({"v": 1,
                            "desired_on": humidity.on, "reason": humidity.reason,
                            "humidity": humidity.humidity, "on_below_pct": humidity.on_below,
                            "off_at_pct": humidity.off_at, "ts": wall}), retain=False)
                    next_send = mono + 2
                if state != last_state:
                    LOG.info("demo%s %s", "[button]" if demo_on else "", " ".join(f"{role}={r.on}({r.reason}, {r.key}={r.value})"
                                                 for role, r in rules.items()))
                    last_state = state
                await asyncio.sleep(.1)
    finally:
        client.disconnect(); client.loop_stop()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=1883)
    p.add_argument("--cool-on-at", type=float, default=26, help="air conditioner ON at/above this C")
    p.add_argument("--cool-off-at", type=float, default=24, help="air conditioner OFF at/below this C")
    p.add_argument("--co2-on-above", type=float, default=1000, help="air purifier ON above this ppm")
    p.add_argument("--co2-off-at", type=float, default=800, help="air purifier OFF at/below this ppm")
    p.add_argument("--hold", type=float, default=3)
    p.add_argument("--humidity-on-below", type=float, default=35)
    p.add_argument("--humidity-off-at", type=float, default=40)
    p.add_argument("--demo-input", action="store_true", help="use isolated demo input, never sensor/env")
    args = p.parse_args(); args.topic = f"{PREFIX}/environment" if args.demo_input else "sensor/env"
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    asyncio.run(run(args))


if __name__ == "__main__": main()
