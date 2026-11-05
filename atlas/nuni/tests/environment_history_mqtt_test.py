"""Hardware-free MQTT -> real nunifusion -> SQLite integration test (stdlib only)."""
import json
import os
from pathlib import Path
import signal
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time


def exact(conn, count):
    result = b""
    while len(result) < count:
        data = conn.recv(count - len(result))
        if not data:
            raise RuntimeError("unexpected MQTT disconnect")
        result += data
    return result


def packet(conn):
    header = exact(conn, 1)[0]
    remaining, multiplier = 0, 1
    for _ in range(4):
        byte = exact(conn, 1)[0]
        remaining += (byte & 127) * multiplier
        if not byte & 128:
            return header, exact(conn, remaining)
        multiplier *= 128
    raise RuntimeError("bad MQTT length")


def publish(raw, retained=False, topic="sensor/env"):
    topic = topic.encode()
    body = len(topic).to_bytes(2, "big") + topic + raw
    length, encoded = len(body), bytearray()
    while True:
        byte = length % 128
        length //= 128
        encoded.append(byte | (128 if length else 0))
        if not length:
            break
    return bytes([0x31 if retained else 0x30]) + encoded + body


def main():
    with tempfile.TemporaryDirectory(prefix="nuni-mqtt-history-") as directory:
        db_path = Path(directory) / "history.db"
        with socket.socket() as broker, open(Path(directory)/"service.log", "w+") as log:
            broker.bind(("127.0.0.1", 0))
            broker.listen(2)
            broker.settimeout(12)
            env = dict(os.environ, NUNI_HISTORY_DB=str(db_path), NUNI_ENABLE_SOOTHE="0")
            service = subprocess.Popen([sys.argv[1], "--host", "127.0.0.1", "--port",
                str(broker.getsockname()[1]), "--no-cry", "--no-fusion", "--no-dbus", "--no-radar"],
                env=env, stdout=log, stderr=log)
            try:
                conn, _ = broker.accept()
                with conn:
                    conn.settimeout(8)
                    assert packet(conn)[0] == 0x10
                    conn.sendall(b"\x20\x02\x00\x00")
                    header, body = packet(conn)
                    assert header == 0x82 and b"sensor/env" in body
                    conn.sendall(b"\x90\x03" + body[:2] + b"\x00")
                    messages = []
                    for seq, temp in [(1,23),(2,24),(3,25),(3,99)]:
                        messages.append(publish(json.dumps(dict(sample_id=f"esp:{seq}",
                            source="pi4-usb",valid=True,age_s=0,temp=temp,humidity=45,co2=650)).encode()))
                    messages.append(publish(json.dumps(dict(sample_id="esp:4",source="pi4-usb",
                        valid=True,age_s=0,temp=99,humidity=45,co2=650)).encode(), retained=True))
                    messages.append(publish(b"not JSON"))
                    now = time.time()
                    for topic, sample in [
                        ("sensor/radar",dict(boot_id="radar-test",seq=1,source="pi4-usb",valid=True,presence=True,breath_valid=True,breathing_rate=31,movement=0.2)),
                        ("audio/cry",dict(boot_id="audio-test",seq=1,ts=now,is_crying=False,confidence=0.1)),
                        ("cry/episode",dict(event="end",id="mqtt-cry",t0=now-4,t1=now-1)),
                        ("cry/feedback",dict(event="answer",id="mqtt-cry",ts=now,action="feed")),
                        ("fusion/alert",dict(ts=now,level="attention")),
                        ("event/tof",dict(id="mqtt-rail",event="rail_intrusion")),
                        ("control/soothe",dict(id="mqtt-rail",ts=now,status="completed")),
                    ]:
                        messages.append(publish(json.dumps(sample).encode(), topic=topic))
                    data = b"".join(messages)
                    assert len(data) < 4096
                    conn.sendall(data)
                    conn.shutdown(socket.SHUT_WR)
                    # A subsequent reconnect establishes that the service consumed the
                    # preceding short packet batch and observed the disconnect. No sleeps.
                    again, _ = broker.accept()
                    again.close()
                service.send_signal(signal.SIGTERM)
                assert service.wait(timeout=10) == 0
                with sqlite3.connect(db_path) as db:
                    # Sum across bucket boundaries so the test is stable at xx:59:59.
                    row = db.execute("SELECT sum(total_count),sum(valid_count),sum(temp_sum),min(temp_min),max(temp_max),sum(radar_count),sum(presence_count) FROM environment_buckets").fetchone()
                assert row == (3,3,72.0,23.0,25.0,1,1), row
                with sqlite3.connect(db_path) as db:
                    # Radar/microphone per-minute statistics are no longer kept.
                    assert db.execute("SELECT count(*) FROM sqlite_master WHERE name='sensor_minutes'").fetchone()[0] == 0
                    assert db.execute("SELECT action FROM cry_episodes WHERE id='mqtt-cry'").fetchone()[0] == 'feed'
                    assert db.execute("SELECT count(*) FROM recorded_events").fetchone()[0] == 5
            except BaseException:
                log.flush();log.seek(0)
                print(log.read(), file=sys.stderr)
                raise
            finally:
                if service.poll() is None:
                    service.terminate()
                    try:
                        service.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        service.kill();service.wait()
    print("MQTT -> nunifusion -> SQLite persistence, radar, cry, events, feedback, retained and malformed data: OK")

if __name__ == "__main__":
    main()
