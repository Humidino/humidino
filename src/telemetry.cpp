#include "telemetry.h"

#include <Arduino.h>
#include <vector>

#include "config.h"
#include "run_log.h"
#include "season.h"
#include "shared_state.h"

namespace Telemetry {

void buildStateJson(JsonDocument& doc) {
    SystemState s;
    if (!ShaState::getSnapshot(s)) return;

    doc["uptime_s"] = millis() / 1000;
    doc["wifi_rssi"] = s.wifiRssi;
    doc["free_heap"] = ESP.getFreeHeap();
    // Определённый по календарю сезон (см. season.h) — чисто информационно,
    // для бейджа на дашборде/веб-странице. Сами пороги, применённые ли
    // сейчас автосезонные значения — в rh_target/etc. и в season_auto из
    // buildSettingsJson() ниже.
    doc["season"] = Season::toString(Season::current());

    JsonObject relay = doc["relay"].to<JsonObject>();
    relay["on"] = s.relay.relayOn;
    relay["state"] = static_cast<int>(s.relay.state);      // числовой код, для обратной совместимости
    relay["state_str"] = toString(s.relay.state);           // машиночитаемый id, напр. "locked_freeze"
    relay["cycle_count"] = s.relay.cycleCount;               // сколько раз включалось за всё время (переживает перезагрузки)

    static const char* kKeys[] = {"crawl_intake", "crawl_mid", "crawl_far", "outside"};
    JsonObject zones = doc["zones"].to<JsonObject>();
    for (size_t i = 0; i < static_cast<size_t>(SensorId::Count); i++) {
        const SensorReading& r = s.readings[i];
        JsonObject z = zones[kKeys[i]].to<JsonObject>();
        z["temp_c"] = r.temperatureC;
        z["rh_pct"] = r.humidityPct;
        // Точка росы/абс. влажность имеют смысл только для зон подпола, не
        // для улицы (см. алгоритм осушения в relay.cpp — condensationSafe
        // сравнивает абс. влажность улицы с подполом, а не наоборот).
        if (static_cast<SensorId>(i) != SensorId::Outside) {
            z["dew_c"] = r.dewPointC;
            z["abs_h_gm3"] = r.absHumidityGm3;
        }
        z["error"] = r.error;
    }
}

void buildSettingsJson(JsonDocument& doc) {
    RuntimeSettings settings = ShaState::getSettings();
    doc["rh_target"] = settings.rhTargetPercent;
    doc["hysteresis_pct"] = settings.hysteresisPercent;
    doc["freeze_c"] = settings.freezeProtectC;
    doc["min_runtime_ms"] = settings.minRuntimeMs;
    doc["min_pause_ms"] = settings.minPauseMs;
    doc["mode"] = toString(settings.mode);
    doc["season_auto"] = settings.seasonAutoEnabled;
}

void buildHistorySummaryJson(JsonDocument& doc) {
    RunLog::Summary s = RunLog::getSummary();
    doc["time_synced"] = s.timeSynced;
    doc["local_tz_offset_sec"] = LOCAL_TZ_OFFSET_SEC;
    doc["runs_today"] = s.runsToday;
    doc["runtime_today_s"] = s.runtimeTodayMs / 1000;
    doc["runs_total"] = s.runsTotal;
    doc["log_count"] = RunLog::count();
    doc["log_capacity"] = RunLog::capacity();
}

void buildHistoryJson(JsonDocument& doc, size_t limit, size_t offset) {
    constexpr size_t kMaxLimit = 200;  // потолок для одного запроса — не даём клиенту раздуть память запросом ?limit=100000
    if (limit == 0) limit = 50;
    if (limit > kMaxLimit) limit = kMaxLimit;

    std::vector<RunLog::RunRecord> records(limit);
    size_t n = RunLog::getRecent(records.data(), limit, offset);

    JsonArray arr = doc.to<JsonArray>();
    for (size_t i = 0; i < n; i++) {
        const RunLog::RunRecord& r = records[i];
        JsonObject o = arr.add<JsonObject>();
        o["start_epoch"] = r.startEpoch;
        o["end_epoch"] = r.endEpoch;
        o["duration_s"] = r.durationMs / 1000;
        o["stop_reason"] = RunLog::toString(r.stopReason);
        // "unknown" — только у ещё не закрытой (текущей) записи, см.
        // комментарий у RunLog::StopReason::Unknown.
        o["in_progress"] = (r.stopReason == RunLog::StopReason::Unknown);

        JsonObject start = o["start"].to<JsonObject>();
        auto setOrNull = [](JsonObject obj, const char* key, float v) {
            if (isnan(v)) obj[key] = nullptr; else obj[key] = v;
        };
        setOrNull(start, "crawl_rh", r.startCrawlRh);
        setOrNull(start, "crawl_temp_c", r.startCrawlTempC);
        setOrNull(start, "outside_rh", r.startOutsideRh);
        setOrNull(start, "outside_temp_c", r.startOutsideTempC);

        JsonObject end = o["end"].to<JsonObject>();
        setOrNull(end, "crawl_rh", r.endCrawlRh);
        setOrNull(end, "crawl_temp_c", r.endCrawlTempC);
        setOrNull(end, "outside_rh", r.endOutsideRh);
        setOrNull(end, "outside_temp_c", r.endOutsideTempC);
    }
}

}  // namespace Telemetry
