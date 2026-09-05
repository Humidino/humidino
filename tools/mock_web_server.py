#!/usr/bin/env python3
"""
Локальный мок-сервер для веб-интерфейса Humidino.

Отдаёт data/index.html и имитирует REST API прошивки (/api/state,
/api/settings, /api/presets, /api/history, /api/history/summary) со
случайными, но правдоподобными данными — позволяет открыть и потестировать
веб-интерфейс в браузере без реального ESP32: анимацию вентилятора,
переключение режимов, пресеты, раздел аналитики (график и таблицу
последних запусков).

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
LOCAL_TZ_OFFSET_SEC = 3 * 3600

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
    "season_auto": True,
}

# Совпадает с Season::forLocalMonth() в src/season.cpp (профили под климат
# Лотошино, МО — см. docs/SEASONAL_LOTOSHINO.md): дек-фев зима, мар-май
# весна, июн-авг лето, сен-ноя осень. Как и прошивка, мок переводит текущую
# UTC-эпоху в локальное время с помощью LOCAL_TZ_OFFSET_SEC.
SEASON_BY_MONTH = {
    12: "winter", 1: "winter", 2: "winter",
    3: "spring", 4: "spring", 5: "spring",
    6: "summer", 7: "summer", 8: "summer",
    9: "autumn", 10: "autumn", 11: "autumn",
}


def current_season():
    local_epoch = time.time() + LOCAL_TZ_OFFSET_SEC
    return SEASON_BY_MONTH[time.gmtime(local_epoch).tm_mon]

presets = [
    {"name": "Лето", "rh_target": 65.0, "hysteresis_pct": 5.0, "freeze_c": 2.0,
     "min_runtime_ms": 10 * 60 * 1000, "min_pause_ms": 15 * 60 * 1000},
    {"name": "Зима", "rh_target": 75.0, "hysteresis_pct": 5.0, "freeze_c": 3.0,
     "min_runtime_ms": 15 * 60 * 1000, "min_pause_ms": 20 * 60 * 1000},
]

# --- Аналитика / журнал запусков (см. src/run_log.h) ---
# Правдоподобная, но не настоящая история циклов реле за последние ~10 дней —
# только чтобы потестировать вёрстку графика и таблицы /api/history без
# реального устройства (см. README §5.1).
STOP_REASONS = ["hysteresis_reached"] * 6 + ["locked_freeze", "locked_condensation", "manual_off"]


def build_fake_history():
    records = []
    t = time.time() - random.uniform(0, 3 * 3600)  # старт самого свежего цикла — недавно
    for _ in range(120):
        duration_s = random.randint(8, 40) * 60
        start_epoch = int(t)
        end_epoch = start_epoch + duration_s
        crawl_rh_start = random.uniform(72, 78)
        crawl_rh_end = crawl_rh_start - random.uniform(3, 8)
        crawl_t = random.uniform(16, 20)
        outside_rh_start = random.uniform(45, 65)
        outside_rh_end = outside_rh_start + random.uniform(-3, 3)
        outside_t = random.uniform(-5, 15)
        records.append({
            "start_epoch": start_epoch,
            "end_epoch": end_epoch,
            "duration_s": duration_s,
            "stop_reason": random.choice(STOP_REASONS),
            "in_progress": False,
            "start": {"crawl_rh": round(crawl_rh_start, 1), "crawl_temp_c": round(crawl_t, 1),
                       "outside_rh": round(outside_rh_start, 1), "outside_temp_c": round(outside_t, 1)},
            "end": {"crawl_rh": round(crawl_rh_end, 1), "crawl_temp_c": round(crawl_t - 0.1, 1),
                     "outside_rh": round(outside_rh_end, 1), "outside_temp_c": round(outside_t, 1)},
        })
        # пауза до предыдущего (по времени) цикла — идём назад по истории
        t -= duration_s + random.randint(15, 90) * 60
    return records  # от самого свежего к самому старому — как отдаёт настоящий /api/history


FAKE_HISTORY = build_fake_history()

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
    if settings["mode"] == "manual_off" and state_str not in ("locked_freeze", "locked_sensor_fault"):
        state_str = "idle"
    elif settings["mode"] == "manual_on" and state_str not in ("locked_freeze", "locked_sensor_fault"):
        state_str = "running"
    wobble = math.sin(elapsed / 5.0) * 3

    # Зона 1 периодически "отваливается" (как раньше) — демонстрирует
    # деградированный режим (2 из 3 живых), а не полную блокировку, раз
    # остальные две зоны подпола остаются исправны.
    zone1_error = int(elapsed) % 30 < 3
    crawl_live = 3 - (1 if zone1_error else 0)

    return {
        "uptime_s": int(elapsed),
        "wifi_rssi": -55 + int(wobble),
        "free_heap": 210000 + random.randint(-5000, 5000),
        "season": current_season(),
        "relay": {"on": state_str == "running", "state_str": state_str},
        "crawlspace": {
            "live_sensors": crawl_live,
            "total_sensors": 3,
            "degraded": crawl_live < 3,
        },
        "zones": {
            "crawl_intake": fake_zone(18 + wobble * 0.2, 74, True, error=(int(elapsed) % 30 < 3)),
            "crawl_mid": fake_zone(17.5 + wobble * 0.2, 71, True),
            "crawl_far": fake_zone(17 + wobble * 0.2, 69, True),
            "outside": fake_zone(9 + wobble, 55, False),
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
        elif self.path.startswith("/api/history/summary"):
            now = int(time.time())
            local_now = now + LOCAL_TZ_OFFSET_SEC
            today_start = local_now - local_now % 86400 - LOCAL_TZ_OFFSET_SEC
            today_end = today_start + 86400
            runs_today = sum(1 for r in FAKE_HISTORY if today_start <= r["start_epoch"] < today_end)
            runtime_today = sum(
                max(0, min(now if r["in_progress"] else r["end_epoch"], today_end)
                    - max(r["start_epoch"], today_start))
                for r in FAKE_HISTORY
            )
            self._send_json({
                "time_synced": True,
                "local_tz_offset_sec": LOCAL_TZ_OFFSET_SEC,
                "runs_today": runs_today,
                "runtime_today_s": runtime_today,
                "runs_total": len(FAKE_HISTORY) + 340,  # имитация: счётчик за всё время шире, чем хранящийся журнал
                "log_count": len(FAKE_HISTORY),
                "log_capacity": 2000,
            })
        elif self.path.startswith("/api/history"):
            from urllib.parse import urlparse, parse_qs
            qs = parse_qs(urlparse(self.path).query)
            limit = int(qs.get("limit", [50])[0])
            offset = int(qs.get("offset", [0])[0])
            self._send_json(FAKE_HISTORY[offset:offset + limit])
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
