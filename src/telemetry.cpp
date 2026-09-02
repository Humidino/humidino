#include "telemetry.h"

#include <Arduino.h>

#include "shared_state.h"

namespace Telemetry {

void buildStateJson(JsonDocument& doc) {
    SystemState s;
    if (!ShaState::getSnapshot(s)) return;

    doc["uptime_s"] = millis() / 1000;
    doc["wifi_rssi"] = s.wifiRssi;
    doc["free_heap"] = ESP.getFreeHeap();

    JsonObject relay = doc["relay"].to<JsonObject>();
    relay["on"] = s.relay.relayOn;
    relay["state"] = static_cast<int>(s.relay.state);      // числовой код, для обратной совместимости
    relay["state_str"] = toString(s.relay.state);           // машиночитаемый id, напр. "locked_freeze"

    static const char* kKeys[] = {"crawl_intake", "crawl_middle", "crawl_corner", "outside"};
    JsonObject zones = doc["zones"].to<JsonObject>();
    for (size_t i = 0; i < 4; i++) {
        const SensorReading& r = s.readings[i];
        JsonObject z = zones[kKeys[i]].to<JsonObject>();
        z["temp_c"] = r.temperatureC;
        z["rh_pct"] = r.humidityPct;
        if (i < 3) {  // точка росы имеет смысл только для трёх зон подпола
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
}

}  // namespace Telemetry
