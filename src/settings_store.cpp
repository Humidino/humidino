#include "settings_store.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "config.h"

namespace {
Preferences g_prefs;
SemaphoreHandle_t g_mutex = nullptr;
constexpr const char* kNamespace = "humidino";

bool lockPrefs() {
    return g_mutex != nullptr && xSemaphoreTake(g_mutex, portMAX_DELAY) == pdTRUE;
}

RuntimeSettings loadUnlocked() {
    RuntimeSettings s;
    g_prefs.begin(kNamespace, true);
    s.rhTargetPercent = g_prefs.getFloat("rh_target", DEFAULT_RH_TARGET_PERCENT);
    s.hysteresisPercent = g_prefs.getFloat("hyst_pct", DEFAULT_HYSTERESIS_PERCENT);
    s.freezeProtectC = g_prefs.getFloat("freeze_c", FREEZE_PROTECT_TEMP_C);
    s.minRuntimeMs = g_prefs.getULong("min_run_ms", MIN_RUNTIME_MS);
    s.minPauseMs = g_prefs.getULong("min_pause_ms", MIN_PAUSE_MS);
    String modeStr = g_prefs.getString("mode", toString(OperatingMode::Auto));
    s.mode = operatingModeFromString(modeStr.c_str());
    s.seasonAutoEnabled = g_prefs.getBool("season_auto", true);
    g_prefs.end();
    return s;
}
}  // namespace

namespace Settings {

void begin() {
    if (g_mutex == nullptr) g_mutex = xSemaphoreCreateMutex();
}

RuntimeSettings load() {
    if (!lockPrefs()) return RuntimeSettings{};
    RuntimeSettings s = loadUnlocked();
    xSemaphoreGive(g_mutex);
    return s;
}

void save(const RuntimeSettings& settings) {
    if (!lockPrefs()) return;
    RuntimeSettings current = loadUnlocked();

    g_prefs.begin(kNamespace, false);
    if (current.rhTargetPercent != settings.rhTargetPercent)
        g_prefs.putFloat("rh_target", settings.rhTargetPercent);
    if (current.hysteresisPercent != settings.hysteresisPercent)
        g_prefs.putFloat("hyst_pct", settings.hysteresisPercent);
    if (current.freezeProtectC != settings.freezeProtectC)
        g_prefs.putFloat("freeze_c", settings.freezeProtectC);
    if (current.minRuntimeMs != settings.minRuntimeMs)
        g_prefs.putULong("min_run_ms", settings.minRuntimeMs);
    if (current.minPauseMs != settings.minPauseMs)
        g_prefs.putULong("min_pause_ms", settings.minPauseMs);
    if (current.mode != settings.mode)
        g_prefs.putString("mode", toString(settings.mode));
    if (current.seasonAutoEnabled != settings.seasonAutoEnabled)
        g_prefs.putBool("season_auto", settings.seasonAutoEnabled);
    g_prefs.end();
    xSemaphoreGive(g_mutex);
}

NetConfig loadNet() {
    NetConfig n;
    if (!lockPrefs()) return n;
    g_prefs.begin(kNamespace, true);
    g_prefs.getString("web_pass", n.webPassword, sizeof(n.webPassword));
    g_prefs.end();
    xSemaphoreGive(g_mutex);
    return n;
}

void saveNet(const NetConfig& net) {
    if (!lockPrefs()) return;
    g_prefs.begin(kNamespace, false);
    g_prefs.putString("web_pass", net.webPassword);
    g_prefs.end();
    xSemaphoreGive(g_mutex);
}

// Пресеты хранятся одним JSON-блобом в ключе "presets_json" — их немного
// (до kMaxPresets) и они всегда читаются/пишутся целиком, так что отдельные
// NVS-ключи на каждое поле были бы лишней сложностью.
std::vector<Preset> loadPresets() {
    std::vector<Preset> result;
    if (!lockPrefs()) return result;

    g_prefs.begin(kNamespace, true);
    String json = g_prefs.getString("presets_json", "[]");
    g_prefs.end();
    xSemaphoreGive(g_mutex);

    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) return result;
    for (JsonObject p : doc.as<JsonArray>()) {
        if (result.size() >= kMaxPresets) break;
        Preset preset;
        strncpy(preset.name, p["name"] | "", sizeof(preset.name) - 1);
        preset.values.rhTargetPercent = p["rh_target"] | DEFAULT_RH_TARGET_PERCENT;
        preset.values.hysteresisPercent = p["hysteresis_pct"] | DEFAULT_HYSTERESIS_PERCENT;
        preset.values.freezeProtectC = p["freeze_c"] | FREEZE_PROTECT_TEMP_C;
        preset.values.minRuntimeMs = p["min_runtime_ms"] | MIN_RUNTIME_MS;
        preset.values.minPauseMs = p["min_pause_ms"] | MIN_PAUSE_MS;
        result.push_back(preset);
    }
    return result;
}

void savePresets(const std::vector<Preset>& presets) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    size_t count = presets.size() < kMaxPresets ? presets.size() : kMaxPresets;
    for (size_t i = 0; i < count; i++) {
        const Preset& preset = presets[i];
        JsonObject p = arr.add<JsonObject>();
        p["name"] = preset.name;
        p["rh_target"] = preset.values.rhTargetPercent;
        p["hysteresis_pct"] = preset.values.hysteresisPercent;
        p["freeze_c"] = preset.values.freezeProtectC;
        p["min_runtime_ms"] = preset.values.minRuntimeMs;
        p["min_pause_ms"] = preset.values.minPauseMs;
    }

    String json;
    serializeJson(doc, json);

    if (!lockPrefs()) return;
    g_prefs.begin(kNamespace, false);
    g_prefs.putString("presets_json", json);
    g_prefs.end();
    xSemaphoreGive(g_mutex);
}

bool loadTouchCalibration(uint16_t out[kTouchCalibrationValues]) {
    if (!lockPrefs()) return false;
    g_prefs.begin(kNamespace, true);
    size_t expected = kTouchCalibrationValues * sizeof(uint16_t);
    size_t got = g_prefs.getBytes("touch_cal", out, expected);
    g_prefs.end();
    xSemaphoreGive(g_mutex);
    return got == expected;
}

void saveTouchCalibration(const uint16_t data[kTouchCalibrationValues]) {
    if (!lockPrefs()) return;
    g_prefs.begin(kNamespace, false);
    g_prefs.putBytes("touch_cal", data, kTouchCalibrationValues * sizeof(uint16_t));
    g_prefs.end();
    xSemaphoreGive(g_mutex);
}

uint32_t loadCycleCount() {
    if (!lockPrefs()) return 0;
    g_prefs.begin(kNamespace, true);
    uint32_t count = g_prefs.getULong("cycle_count", 0);
    g_prefs.end();
    xSemaphoreGive(g_mutex);
    return count;
}

void saveCycleCount(uint32_t count) {
    if (!lockPrefs()) return;
    g_prefs.begin(kNamespace, false);
    g_prefs.putULong("cycle_count", count);
    g_prefs.end();
    xSemaphoreGive(g_mutex);
}

}  // namespace Settings
