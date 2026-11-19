#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
HTTP_PID=""
BROKER_PID=""
MQTT_PID=""

stop_pid() {
    local pid="$1"
    [ -n "$pid" ] || return 0
    kill "$pid" 2>/dev/null || true
    for _ in $(seq 1 30); do
        kill -0 "$pid" 2>/dev/null || return 0
        sleep 0.1
    done
    kill -9 "$pid" 2>/dev/null || true
}

cleanup() {
    stop_pid "$HTTP_PID"
    stop_pid "$MQTT_PID"
    stop_pid "$BROKER_PID"
    rm -rf "$TMP"
}
trap cleanup EXIT

cd "$ROOT"
make CC=gcc
./build/mp60d --self-test

cat >"$TMP/http.conf" <<EOF
http_enabled=1
http_port=18081
mqtt_enabled=0
EOF
./build/mp60d --simulate --config "$TMP/http.conf" &
HTTP_PID=$!
sleep 1
python3 tests/http_api_test.py http://127.0.0.1:18081
stop_pid "$HTTP_PID"
HTTP_PID=""

cat >"$TMP/mqtt.conf" <<EOF
http_enabled=0
mqtt_enabled=1
ha_discovery=1
mqtt_host=127.0.0.1
mqtt_port=18883
mqtt_topic=mp60test
mqtt_client_id=mp60-simulator-test
mqtt_interval=5
EOF
python3 tests/mqtt_broker_test.py "$TMP/mqtt-results.json" &
BROKER_PID=$!
sleep 1
./build/mp60d --simulate --config "$TMP/mqtt.conf" &
MQTT_PID=$!
wait "$BROKER_PID"
BROKER_PID=""
stop_pid "$MQTT_PID"
MQTT_PID=""
printf 'MQTT test report: %s\n' "$TMP/mqtt-results.json"
printf 'All host-side MP60 daemon tests passed\n'
