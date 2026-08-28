#!/usr/bin/env python3
"""
Локальный мок-сервер для веб-интерфейса Humidino.

Отдаёт data/index.html и имитирует REST API прошивки (/api/state,
/api/settings, /api/presets, /api/network) со случайными, но правдоподобными
данными — позволяет открыть и потестировать веб-интерфейс в браузере без
реального ESP32: анимацию вентилятора, переключение режимов, пресеты,
форму настроек сети/MQTT.

Использование:
    python tools/mock_web_server.py [порт]

По умолчанию порт 8000. Затем открыть http://localhost:8000/ в браузере.

Зависимостей нет — используется только стандартная библиотека Python 3.
"""

import http.server
import json
import math
import random
import sys
import time
from pathlib import Path

DATA_DIR = Path(__file__).resolve().parent.parent / "data"
START_TIME = time.time()

# Строковые идентификаторы совпадают с ShaState::toString(RelayControlState)
# в shared_state.cpp. Цикл нужен только для демонстрации — на реальном
# устройстве состояние определяется алгоритмом, а не таймером.
RELAY_STATE_CYCLE = [
    "idle", "running", "locked_condensation", "locked_freeze",
    "min_pause_hold", "locked_sensor_fault",
]
STATE_HOLD_SECONDS = 8  # держим каждое состояние N секунд, чтобы увидеть все баннеры

settings = {
    "rh_target": 70.0,
    "hysteresis_pct": 5.0,
    "freeze_c": 2.0,
    "min_runtime_ms": 10 * 60 * 1000,
    "min_pause_ms": 15 * 60 * 1000,
    "mode": "auto",
}

presets = [
    {"name": "Лето", "rh_target": 65.0, "hysteresis_pct": 5.0, "freeze_c": 2.0,
     "min_runtime_ms": 10 * 60 * 1000, "min_pause_ms": 15 * 60 * 1000},
    {"name": "Зима", "rh_target": 75.0, "hysteresis_pct": 5.0, "freeze_c": 3.0,
     "min_runtime_ms": 15 * 60 * 1000, "min_pause_ms": 20 * 60 * 1000},
]

network = {
    "mqtt_host": "",
    "mqtt_port": 1883,
    "mqtt_user": "",
    "mqtt_pass": "",
    "mqtt_topic": "humidino",
}


def fake_zone(base_temp, base_rh, with_dew, error=False):
    t = base_temp + random.uniform(-0.3, 0.3)
    rh = max(0.0, min(100.0, base_rh + random.uniform(-1.5, 1.5)))
    zone = {"temp_c": round(t, 1), "rh_pct": round(rh, 1), "error": error}
    if with_dew:
        # Грубая имитация точки росы/абс. влажности для правдоподобия UI —
        # не физическая формула, только для визуального теста интерфейса.
        zone["dew_c"] = round(t - (100 - rh) / 5, 1)
        zone["abs_h_gm3"] = round(rh * 0.15 + t * 0.05, 2)
    return zone


def build_state():
    elapsed = time.time() - START_TIME
    state_str = RELAY_STATE_CYCLE[int(elapsed // STATE_HOLD_SECONDS) % len(RELAY_STATE_CYCLE)]
    if settings["mode"] == "manual_off":
        state_str = "idle"
    elif settings["mode"] == "manual_on" and state_str not in ("locked_freeze", "locked_sensor_fault"):
        state_str = "running"
    wobble = math.sin(elapsed / 5.0) * 3

    return {
        "uptime_s": int(elapsed),
        "wifi_rssi": -55 + int(wobble),
        "free_heap": 210000 + random.randint(-5000, 5000),
        "relay": {"on": state_str == "running", "state_str": state_str},
        "zones": {
            "crawl_intake": fake_zone(18 + wobble * 0.2, 74, True),
            "crawl_corner": fake_zone(17.5 + wobble * 0.2, 76, True),
            "outside": fake_zone(9 + wobble, 55, False),
            "house": fake_zone(21, 48, False, error=(int(elapsed) % 30 < 3)),
        },
    }


def network_public():
    return {
        "mqtt_host": network["mqtt_host"],
        "mqtt_port": network["mqtt_port"],
        "mqtt_user": network["mqtt_user"],
        "mqtt_topic": network["mqtt_topic"],
        "mqtt_pass_set": bool(network["mqtt_pass"]),
    }


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(DATA_DIR), **kwargs)

    def _send_json(self, payload, status=200):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_json(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)
        try:
            return json.loads(body)
        except json.JSONDecodeError:
            return None

    def do_GET(self):
        if self.path == "/api/state":
            self._send_json(build_state())
        elif self.path == "/api/settings":
            self._send_json(settings)
        elif self.path == "/api/presets":
            self._send_json(presets)
        elif self.path == "/api/network":
            self._send_json(network_public())
        else:
            super().do_GET()

    def do_POST(self):
        if self.path == "/api/settings":
            incoming = self._read_json() or {}
            settings.update({k: v for k, v in incoming.items() if k in settings})
            self._send_json(settings)
        elif self.path == "/api/presets/apply":
            incoming = self._read_json() or {}
            name = incoming.get("name")
            preset = next((p for p in presets if p["name"] == name), None)
            if preset is None:
                self._send_json({"error": "preset not found"}, status=404)
                return
            for key in ("rh_target", "hysteresis_pct", "freeze_c", "min_runtime_ms", "min_pause_ms"):
                settings[key] = preset[key]
            self._send_json(settings)
        elif self.path == "/api/network":
            incoming = self._read_json() or {}
            for key in ("mqtt_host", "mqtt_port", "mqtt_user", "mqtt_topic"):
                if key in incoming:
                    network[key] = incoming[key]
            if incoming.get("mqtt_pass"):
                network["mqtt_pass"] = incoming["mqtt_pass"]
            self._send_json(network_public())
        else:
            self.send_error(404)

    def do_PUT(self):
        if self.path == "/api/presets":
            global presets
            incoming = self._read_json()
            if isinstance(incoming, list):
                presets = incoming
            self._send_json(presets)
        else:
            self.send_error(404)

    def log_message(self, fmt, *args):
        pass  # тише — не засорять консоль обычными GET-запросами опроса


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    server = http.server.ThreadingHTTPServer(("0.0.0.0", port), Handler)
    print(f"Мок веб-интерфейса Humidino: http://localhost:{port}/")
    print("Ctrl+C для остановки")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
