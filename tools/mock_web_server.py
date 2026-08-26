#!/usr/bin/env python3
"""
Локальный мок-сервер для веб-интерфейса Humidino.

Отдаёт data/index.html и имитирует REST API прошивки (/api/state,
/api/settings) со случайными, но правдоподобными данными — позволяет
открыть и потестировать веб-интерфейс в браузере без реального ESP32.

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

# Состояния реле совпадают с enum RelayControlState в shared_state.h:
# 0=Idle, 1=Running, 2=LockedOutCondensation, 3=LockedOutFreeze, 4=MinPauseHold
RELAY_STATE_CYCLE = [0, 1, 2, 3, 4]
STATE_HOLD_SECONDS = 8  # держим каждое состояние N секунд, чтобы увидеть все баннеры

settings = {
    "rh_target": 70.0,
    "hysteresis_pct": 5.0,
    "freeze_c": 2.0,
    "min_runtime_ms": 10 * 60 * 1000,
    "min_pause_ms": 15 * 60 * 1000,
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
    relay_state = RELAY_STATE_CYCLE[int(elapsed // STATE_HOLD_SECONDS) % len(RELAY_STATE_CYCLE)]
    wobble = math.sin(elapsed / 5.0) * 3

    return {
        "uptime_s": int(elapsed),
        "wifi_rssi": -55 + int(wobble),
        "free_heap": 210000 + random.randint(-5000, 5000),
        "relay": {"on": relay_state == 1, "state": relay_state},
        "zones": {
            "crawl_intake": fake_zone(18 + wobble * 0.2, 74, True),
            "crawl_corner": fake_zone(17.5 + wobble * 0.2, 76, True),
            "outside": fake_zone(9 + wobble, 55, False),
            "house": fake_zone(21, 48, False, error=(int(elapsed) % 30 < 3)),
        },
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

    def do_GET(self):
        if self.path == "/api/state":
            self._send_json(build_state())
        elif self.path == "/api/settings":
            self._send_json(settings)
        else:
            super().do_GET()

    def do_POST(self):
        if self.path == "/api/settings":
            length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(length)
            try:
                incoming = json.loads(body)
            except json.JSONDecodeError:
                incoming = {}
            settings.update({k: v for k, v in incoming.items() if k in settings})
            self._send_json(settings)
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
