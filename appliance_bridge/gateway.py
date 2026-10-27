"""Pi 4 USB gateway. Explicit enrolled ports, identity checks and expiring commands."""
import argparse
import json
import logging
import os
import queue
import signal
import time
import uuid
import serial
import paho.mqtt.client as mqtt
from rule import PREFIX, ROLES, validate_command

LOG = logging.getLogger("nuni-appliance-usb")


class Device:
    def __init__(self, role, config, publish, serial_factory=serial.Serial):
        self.role, self.config, self.publish = role, config, publish
        self.factory = serial_factory
        self.port = None
        self.pending = b""
        self.identity = None
        self.sequence = 0
        self.last_rx = self.next_open = self.next_send = 0
        self.command = None
        self.seen = set()
        self.actual = None
        self.last_report = 0
        self.status = "waiting_for_usb"

    def accept(self, data, wall):
        if not validate_command(data, self.role, wall) or data["id"] in self.seen: return False
        if len(self.seen) >= 256: self.seen.clear()
        self.seen.add(data["id"])
        self.command = data
        self.next_send = 0
        return True

    def clear_command(self):
        self.command = None
        self.next_send = 0

    def close(self):
        if self.port:
            try: self.port.close()
            except OSError: pass
        self.port, self.identity, self.actual = None, None, None
        self.pending = b""

    def frame(self, line, mono):
        if not line.startswith(b"NUNIA1 "): return
        try: data = json.loads(line[7:])
        except (ValueError, UnicodeDecodeError): return
        if (not isinstance(data, dict) or data.get("v") != 1 or data.get("kind") != "state"
            or data.get("role") != self.role or data.get("device_id") != self.config.get("device_id")
            or not isinstance(data.get("boot"), str) or len(data["boot"]) != 16
            or type(data.get("on")) is not bool or type(data.get("seq")) is not int
            or not 0 <= data["seq"] <= 0xffffffff): return
        if self.identity != data["boot"]:
            self.identity = data["boot"]
            self.sequence = data["seq"]
            self.next_send = 0
        self.sequence = max(self.sequence, data["seq"])
        self.actual, self.last_rx = data["on"], mono
        self.status = "usb_state_received"

    def step(self, mono, wall, broker_connected):
        path = self.config.get("port")
        if not path or not self.config.get("device_id"):
            self.status = "not_enrolled"
        else:
            try:
                if self.port is None and mono >= self.next_open:
                    self.next_open = mono + 2
                    port = self.factory()
                    port.port, port.baudrate, port.timeout, port.write_timeout = path, 115200, 0, .2
                    port.dtr = port.rts = False
                    if os.name == "posix": port.exclusive = True
                    port.open()
                    self.port, self.last_rx = port, mono
                    self.status = "verifying_usb_identity"
                if self.port:
                    self.pending += self.port.read(2048)
                    for _ in range(32):
                        if b"\n" not in self.pending: break
                        line, self.pending = self.pending.split(b"\n", 1)
                        self.frame(line.strip(), mono)
                    if len(self.pending) > 4096: self.pending = b""
                    if mono - self.last_rx > 4:
                        raise OSError("USB heartbeat timeout")
                    if self.identity and mono >= self.next_send:
                        command = self.command if broker_connected else None
                        remaining = command["expires_at"] - wall if command else 0
                        on = bool(command and command["on"] and 1 <= remaining <= 8)
                        lease = min(4000, max(1000, int(remaining * 1000))) if on else 1000
                        self.sequence += 1
                        request = {"v": 1, "kind": "set", "role": self.role,
                            "boot": self.identity, "seq": self.sequence, "on": on, "lease_ms": lease}
                        frame = b"NUNIA1 " + json.dumps(request, separators=(",", ":")).encode() + b"\n"
                        if self.port.write(frame) != len(frame): raise OSError("partial USB write")
                        self.next_send = mono + 1
            except (OSError, serial.SerialException, ValueError) as exc:
                LOG.warning("%s USB unavailable: %s", self.role, exc)
                self.close(); self.status = "usb_disconnected"; self.next_open = mono + 2
        if mono - self.last_report >= 1:
            connected = self.identity is not None and mono - self.last_rx <= 3
            cmd = self.command
            self.publish(self.role, {"v": 1, "mode": "usb_appliance_demo", "cloud_confirmed": False,
                "role": self.role, "usb_connected": connected, "actual_on": self.actual if connected else None,
                "desired_on": bool(cmd and broker_connected and 1 <= cmd["expires_at"] - wall <= 8 and cmd["on"]),
                "command_id": cmd["id"] if cmd else None, "status": self.status,
                "device_id": self.config.get("device_id"), "ts": wall})
            self.last_report = mono


def run(args):
    with open(args.config, encoding="utf-8") as f: config = json.load(f)
    incoming = queue.Queue(maxsize=64)
    connected, stopped = False, False
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="nuni-appliance-usb-" + uuid.uuid4().hex[:12])
    client.max_queued_messages_set(16)

    def publish(role, data):
        if connected: client.publish(f"{PREFIX}/{role}/state", json.dumps(data), qos=0, retain=False)

    devices = {role: Device(role, config.get("devices", {}).get(role, {}), publish) for role in ROLES}

    def on_connect(c, userdata, flags, code, properties):
        nonlocal connected
        connected = code == 0
        if connected:
            for role in ROLES: c.subscribe(f"{PREFIX}/{role}/command", qos=1)

    def on_disconnect(*unused):
        nonlocal connected
        connected = False

    def on_message(c, userdata, msg):
        if msg.retain or len(msg.payload) > 8192: return
        role = msg.topic.split("/")[-2]
        try:
            data = json.loads(msg.payload)
            if role in devices and isinstance(data, dict): incoming.put_nowait((role, data))
        except (ValueError, queue.Full): pass

    def halt(*unused):
        nonlocal stopped
        stopped = True

    signal.signal(signal.SIGTERM, halt); signal.signal(signal.SIGINT, halt)
    client.on_connect, client.on_disconnect, client.on_message = on_connect, on_disconnect, on_message
    client.connect_async(args.host, args.port, keepalive=10); client.loop_start()
    try:
        while not stopped:
            mono, wall = time.monotonic(), time.time()
            if not connected:
                for device in devices.values(): device.clear_command()
            for _ in range(64):
                try: role, data = incoming.get_nowait()
                except queue.Empty: break
                if connected: devices[role].accept(data, wall)
            for device in devices.values(): device.step(mono, wall, connected)
            time.sleep(.05)
    finally:
        for device in devices.values():
            device.clear_command(); device.next_send = 0
            device.step(time.monotonic(), time.time(), False); device.close()
        client.disconnect(); client.loop_stop()


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--host", default="10.77.0.5"); p.add_argument("--port", type=int, default=1883)
    p.add_argument("--config", default="/data/nuni/appliances/devices.json")
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    run(p.parse_args())
