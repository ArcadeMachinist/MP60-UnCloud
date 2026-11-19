#!/usr/bin/env python3
"""Minimal MQTT 3.1.1 test broker; validates MP60 topics, commands and reconnect."""
import json
import socket
import struct
import sys
import time
from pathlib import Path

HOST = "0.0.0.0"
PORT = 18883
OUTPUT = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("mqtt-test-results.json")


def encode_remaining(length):
    result = bytearray()
    while True:
        digit = length % 128
        length //= 128
        if length:
            digit |= 128
        result.append(digit)
        if not length:
            return bytes(result)


def packet(kind, body=b""):
    return bytes([kind]) + encode_remaining(len(body)) + body


def mqtt_string(value):
    data = value.encode()
    return struct.pack("!H", len(data)) + data


def publish(topic, payload):
    return packet(0x30, mqtt_string(topic) + payload.encode())


def receive(sock):
    first = sock.recv(1)
    if not first:
        raise EOFError("client closed")
    multiplier, remaining = 1, 0
    while True:
        digit = sock.recv(1)[0]
        remaining += (digit & 127) * multiplier
        if not digit & 128:
            break
        multiplier *= 128
    body = bytearray()
    while len(body) < remaining:
        chunk = sock.recv(remaining - len(body))
        if not chunk:
            raise EOFError("truncated packet")
        body.extend(chunk)
    return first[0], bytes(body)


def parse_connect(body):
    length = struct.unpack_from("!H", body, 0)[0]
    protocol = body[2:2 + length]
    pos = 2 + length
    level, flags = body[pos], body[pos + 1]
    keepalive = struct.unpack_from("!H", body, pos + 2)[0]
    pos += 4
    values = []
    for _ in range(3):
        length = struct.unpack_from("!H", body, pos)[0]
        pos += 2
        values.append(body[pos:pos + length].decode())
        pos += length
    return {"protocol": protocol.decode(), "level": level, "flags": flags,
            "keepalive": keepalive, "client_id": values[0],
            "will_topic": values[1], "will_payload": values[2]}


def session(listener, phase, events):
    connection, peer = listener.accept()
    connection.settimeout(30)
    kind, body = receive(connection)
    assert kind == 0x10, f"expected CONNECT, got {kind:#x}"
    connect = parse_connect(body)
    assert connect["protocol"] == "MQTT" and connect["level"] == 4
    assert connect["client_id"] == "mp60-simulator-test"
    assert connect["will_topic"] == "tele/mp60test/LWT"
    assert connect["will_payload"] == "Offline"
    assert connect["flags"] & 0x20, "retained will missing"
    events.append({"phase": phase, "connect": connect, "peer": peer})
    connection.sendall(packet(0x20, b"\x00\x00"))
    subscribed = False
    command_step = 0
    last_command = 0
    deadline = time.time() + 30
    while time.time() < deadline:
        try:
            kind, body = receive(connection)
        except socket.timeout:
            break
        if kind == 0x82:
            packet_id = body[:2]
            events.append({"phase": phase, "subscribe_raw": body.hex()})
            connection.sendall(packet(0x90, packet_id + b"\x00"))
            subscribed = True
            last_command = 0
        elif kind == 0xC0:
            connection.sendall(packet(0xD0))
        elif kind & 0xF0 == 0x30:
            topic_length = struct.unpack_from("!H", body, 0)[0]
            topic = body[2:2 + topic_length].decode()
            payload = body[2 + topic_length:].decode()
            events.append({"phase": phase, "publish": {"topic": topic, "payload": payload,
                                                        "retain": bool(kind & 1)}})
        if phase == "commands" and subscribed and time.time() - last_command > 1:
            commands = [("cmnd/mp60test/POWER2", "ON"),
                        ("cmnd/mp60test/POWER6", "TOGGLE"),
                        ("cmnd/mp60test/POWER", "OFF"),
                        ("cmnd/mp60test/STATE", "")]
            if command_step < len(commands):
                topic, payload = commands[command_step]
                connection.sendall(publish(topic, payload))
                events.append({"phase": phase, "sent_command": {"topic": topic, "payload": payload}})
                command_step += 1
                last_command = time.time()
            elif command_step == len(commands):
                time.sleep(2)
                connection.close()
                return
    connection.close()
    if phase == "commands":
        raise AssertionError("command session timed out")


def main():
    events = []
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((HOST, PORT))
    listener.listen(2)
    listener.settimeout(35)
    session(listener, "commands", events)
    session(listener, "reconnect", events)
    listener.close()
    messages = [e["publish"] for e in events if "publish" in e]
    assert any(m["topic"] == "tele/mp60test/LWT" and m["payload"] == "Online" and m["retain"] for m in messages)
    assert any(m["topic"] == "stat/mp60test/RESULT" and '"POWER2":"ON"' in m["payload"] for m in messages)
    assert any(m["topic"] == "stat/mp60test/RESULT" and '"POWER6":"ON"' in m["payload"] for m in messages)
    assert any(m["topic"] == "tele/mp60test/SENSOR" and '"ENERGY"' in m["payload"] for m in messages)
    assert any(m["topic"] == "tele/mp60test/STATE" and '"POWER1":"OFF"' in m["payload"] for m in messages)
    discovery = [m for m in messages if m["topic"].startswith("homeassistant/")]
    discovery_topics = {m["topic"] for m in discovery}
    assert len(discovery_topics) == 22, f"expected 22 HA entities, got {len(discovery_topics)}"
    assert all(m["retain"] for m in discovery), "HA discovery messages must be retained"
    switch_topic = "homeassistant/switch/mp60test_power1/config"
    switch = next(json.loads(m["payload"]) for m in discovery if m["topic"] == switch_topic)
    assert switch["command_topic"] == "cmnd/mp60test/POWER1"
    assert switch["state_topic"] == "stat/mp60test/POWER1"
    assert switch["availability_topic"] == "tele/mp60test/LWT"
    assert switch["device"]["manufacturer"] == "UnDatto"
    assert switch["device"]["model"] == "MP60 Powerstrip"
    sensor_topic = "homeassistant/sensor/mp60test_power3_power/config"
    sensor = next(json.loads(m["payload"]) for m in discovery if m["topic"] == sensor_topic)
    assert sensor["value_template"] == "{{ value_json.MP60.Outlets[2].Active }}"
    assert sensor["device_class"] == "power" and sensor["unit_of_measurement"] == "W"
    assert any(m["topic"] == "tele/mp60test/SENSOR" and '"Frequency":60' in m["payload"] for m in messages)
    result = {"passed": True, "event_count": len(events), "events": events}
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n")
    print(f"PASS: MQTT connect, retained LWT, HA discovery, subscribe, commands, telemetry and reconnect ({len(events)} events)")


if __name__ == "__main__":
    main()
