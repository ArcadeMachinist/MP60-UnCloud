#!/usr/bin/env python3
"""End-to-end HTTP tests against an MP60 simulator instance."""
import base64
import json
import re
import sys
import urllib.error
import urllib.request

BASE = sys.argv[1] if len(sys.argv) > 1 else "http://192.168.5.65:8081"


def request(path, method="GET", body=None, password=None):
    headers = {}
    if body is not None:
        body = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    if password is not None:
        token = base64.b64encode(("admin:" + password).encode()).decode()
        headers["Authorization"] = "Basic " + token
    req = urllib.request.Request(BASE + path, data=body, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=8) as response:
            return response.status, json.loads(response.read())
    except urllib.error.HTTPError as error:
        data = error.read()
        return error.code, json.loads(data) if data else {}


def expect(condition, message):
    if not condition:
        raise AssertionError(message)
    print("PASS:", message)


status, state = request("/api/status")
expect(status == 200 and state["backend"] == "simulator", "simulator status")
expect(state["version"] == "1.5.1" and state["firmware"] == "OpenWrt 25.12.6" and state["kernel"],
       "standalone firmware and running-kernel versions")
expect(len(state["outlets"]) == 6 and "voltage_v" in state["hardware"], "six outlets and metering")
expect(state["hardware"]["age_s"] >= 0 and state["hardware"]["last_poll_ok"] and not state["hardware"]["stale"],
       "metering sample age and successful-poll status")
status, state = request("/api/outlets/3", "POST", {"state": "on"})
expect(status == 200 and state["outlets"][2]["state"] == "ON", "direct outlet 3 on")
expect(state["outlets"][2]["state"] == state["outlets"][2]["reported_state"] and
       state["outlets"][2]["confirmed"] and state["outlets"][2]["desired_state"] == "ON",
       "UI state is confirmed by hardware-reported mask")
expect(state["outlets"][2]["active_w"] > 0, "simulated per-outlet power")
status, state = request("/cm?cmnd=Power3%20Toggle")
expect(status == 200 and state == {"POWER3": "OFF"}, "Tasmota toggle")
status, state = request("/cm?cmnd=Power%20On")
expect(status == 200 and all(state[f"POWER{i}"] == "ON" for i in range(1, 7)), "Tasmota all on")
status, state = request("/api/outlets/all", "POST", {"state": "off"})
expect(status == 200 and all(o["state"] == "OFF" for o in state["outlets"]), "direct all off")
status, _ = request("/api/outlets/2", "POST", {"state": "invalid"})
expect(status == 400, "invalid outlet state rejected")
status, config = request("/api/config")
expect(status == 200 and "mqtt_password" not in config and "ui_hash" not in config and "ui_salt" not in config,
       "secrets omitted from configuration response")
expect(re.fullmatch(r"mp60-[0-9a-f]{8}", config["mqtt_topic"]) is not None,
       "default MQTT topic uses last four MAC bytes")
expect(config["ha_discovery"] is False, "HA discovery disabled by default")
status, _ = request("/api/config", "POST", {"ha_discovery": True})
expect(status == 200, "enable HA discovery setting")
status, config = request("/api/config")
expect(config["ha_discovery"] is True, "HA discovery setting persists")
status, _ = request("/api/config", "POST", {"ha_discovery": False})
expect(status == 200, "disable HA discovery setting")
status, wifi = request("/api/wifi")
expect(status == 200 and wifi["ethernet_priority"] and "ip_address" in wifi and "password" not in wifi,
       "Wi-Fi status includes IP address and omits secrets")
status, scan = request("/api/wifi/scan")
expect(status == 200 and scan["count"] == 2 and scan["networks"][0]["ssid"] == "MP60-Test" and
       scan["networks"][0]["signal_dbm"] == -42.0 and scan["networks"][1]["open_network"],
       "Wi-Fi scan returns sorted SSID, signal, frequency, and security data")
status, _ = request("/api/wifi", "POST", {"enabled": True, "ssid": ""})
expect(status == 400, "enabled Wi-Fi requires SSID")
status, _ = request("/api/wifi", "POST", {"enabled": True, "ssid": "MP60-Test"})
expect(status == 400, "secured Wi-Fi requires password or open selection")
status, _ = request("/api/wifi", "POST", {"enabled": True, "ssid": "MP60-Test", "password": "short"})
expect(status == 400, "short Wi-Fi password rejected")
status, _ = request("/api/wifi", "POST", {"enabled": False, "ssid": "MP60-Test", "password": "test-password"})
expect(status == 200, "valid Wi-Fi credentials saved in simulator")
status, wifi = request("/api/wifi")
expect(wifi["ssid"] == "MP60-Test" and wifi["password_set"], "Wi-Fi password-set state reported securely")
status, _ = request("/api/wifi", "POST", {"enabled": True, "ssid": "MP60-Test"})
expect(status == 200, "blank Wi-Fi password preserves saved credential")
status, _ = request("/api/wifi", "POST", {"enabled": True, "ssid": "MP60-Test", "open_network": True})
expect(status == 200, "open Wi-Fi network accepted")
status, wifi = request("/api/wifi")
expect(not wifi["password_set"], "open-network selection clears saved password")
status, _ = request("/api/wifi", "POST", {"enabled": False, "ssid": "", "open_network": True})
expect(status == 200, "Wi-Fi can be disabled without credentials")
status, _ = request("/api/config", "POST", {"ui_password": "test-secret"})
expect(status == 200, "set UI password")
status, _ = request("/api/status")
expect(status == 401, "unauthenticated request rejected")
status, _ = request("/api/status", password="wrong")
expect(status == 401, "wrong password rejected")
status, state = request("/api/status", password="test-secret")
expect(status == 200, "correct HTTP Basic password accepted")
status, _ = request("/api/config", "POST", {"ui_password": ""}, password="test-secret")
expect(status == 200, "clear UI password")
status, _ = request("/api/status")
expect(status == 200, "authentication disabled after clearing password")
print("All HTTP/API/authentication/Wi-Fi tests passed")
