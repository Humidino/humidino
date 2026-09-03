#include "web_server.h"

#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <cstring>
#include <esp_random.h>

#include "config.h"
#include "settings_actions.h"
#include "settings_store.h"
#include "shared_state.h"
#include "telemetry.h"

namespace {

AsyncWebServer g_server(WEB_SERVER_PORT);
char g_webPassword[33] = "";

bool requireAuthentication(AsyncWebServerRequest* req) {
    if (req->authenticate(DEVICE_HOSTNAME, g_webPassword)) return true;
    req->requestAuthentication(DEVICE_HOSTNAME, true);
    return false;
}

void loadWebPassword() {
    Settings::NetConfig net = Settings::loadNet();
    if (net.webPassword[0] == '\0') {
        snprintf(net.webPassword, sizeof(net.webPassword), "%08lx%08lx%08lx%08lx",
                 static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()),
                 static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()));
        Settings::saveNet(net);
    }
    strncpy(g_webPassword, net.webPassword, sizeof(g_webPassword) - 1);
    g_webPassword[sizeof(g_webPassword) - 1] = '\0';
    Serial.printf("Web login: %s / %s\n", DEVICE_HOSTNAME, g_webPassword);
}

void writePresetsJson(JsonDocument& doc, const std::vector<Settings::Preset>& presets) {
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& preset : presets) {
        JsonObject p = arr.add<JsonObject>();
        p["name"] = preset.name;
        p["rh_target"] = preset.values.rhTargetPercent;
        p["hysteresis_pct"] = preset.values.hysteresisPercent;
        p["freeze_c"] = preset.values.freezeProtectC;
        p["min_runtime_ms"] = preset.values.minRuntimeMs;
        p["min_pause_ms"] = preset.values.minPauseMs;
    }
}

}  // namespace

namespace LocalWebServer {

void begin() {
    if (!LittleFS.begin(false)) {
        Serial.println("LittleFS mount failed; web server not started");
        return;
    }
    loadWebPassword();
    g_server.serveStatic("/", LittleFS, "/")
        .setDefaultFile("index.html")
        .setAuthentication(DEVICE_HOSTNAME, g_webPassword);

    g_server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!requireAuthentication(req)) return;
        JsonDocument doc;
        Telemetry::buildStateJson(doc);
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    g_server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!requireAuthentication(req)) return;
        JsonDocument doc;
        Telemetry::buildSettingsJson(doc);
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    // --- Аналитика / журнал запусков (run_log.h) ---
    g_server.on("/api/history/summary", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!requireAuthentication(req)) return;
        JsonDocument doc;
        Telemetry::buildHistorySummaryJson(doc);
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    g_server.on("/api/history", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!requireAuthentication(req)) return;
        size_t limit = req->hasParam("limit") ? req->getParam("limit")->value().toInt() : 50;
        size_t offset = req->hasParam("offset") ? req->getParam("offset")->value().toInt() : 0;
        JsonDocument doc;
        Telemetry::buildHistoryJson(doc, limit, offset);
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    g_server.addHandler(new AsyncCallbackJsonWebHandler(
        "/api/settings", [](AsyncWebServerRequest* req, JsonVariant& json) {
            if (!requireAuthentication(req)) return;
            JsonObject body = json.as<JsonObject>();
            RuntimeSettings settings = ShaState::getSettings();

            if (body["rh_target"].is<float>()) settings.rhTargetPercent = body["rh_target"];
            if (body["hysteresis_pct"].is<float>()) settings.hysteresisPercent = body["hysteresis_pct"];
            if (body["freeze_c"].is<float>()) settings.freezeProtectC = body["freeze_c"];
            if (body["min_runtime_ms"].is<uint32_t>()) settings.minRuntimeMs = body["min_runtime_ms"];
            if (body["min_pause_ms"].is<uint32_t>()) settings.minPauseMs = body["min_pause_ms"];
            if (body["mode"].is<const char*>()) settings.mode = operatingModeFromString(body["mode"]);

            SettingsActions::applyRuntimeSettings(settings);

            JsonDocument resp;
            Telemetry::buildSettingsJson(resp);
            String out;
            serializeJson(resp, out);
            req->send(200, "application/json", out);
        }));

    // --- Пресеты порогов ---
    g_server.on("/api/presets", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!requireAuthentication(req)) return;
        JsonDocument doc;
        writePresetsJson(doc, Settings::loadPresets());
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    g_server.addHandler(new AsyncCallbackJsonWebHandler(
        "/api/presets", [](AsyncWebServerRequest* req, JsonVariant& json) {
            if (!requireAuthentication(req)) return;
            JsonArray arr = json.as<JsonArray>();
            std::vector<Settings::Preset> presets;
            for (JsonObject p : arr) {
                if (presets.size() >= Settings::kMaxPresets) break;
                Settings::Preset preset{};
                strncpy(preset.name, (p["name"] | ""), sizeof(preset.name) - 1);
                preset.name[sizeof(preset.name) - 1] = '\0';
                if (strlen(preset.name) == 0) continue;  // без имени пресет не имеет смысла
                preset.values.rhTargetPercent = p["rh_target"] | DEFAULT_RH_TARGET_PERCENT;
                preset.values.hysteresisPercent = p["hysteresis_pct"] | DEFAULT_HYSTERESIS_PERCENT;
                preset.values.freezeProtectC = p["freeze_c"] | FREEZE_PROTECT_TEMP_C;
                preset.values.minRuntimeMs = p["min_runtime_ms"] | MIN_RUNTIME_MS;
                preset.values.minPauseMs = p["min_pause_ms"] | MIN_PAUSE_MS;
                presets.push_back(preset);
            }
            Settings::savePresets(presets);

            JsonDocument resp;
            writePresetsJson(resp, presets);
            String out;
            serializeJson(resp, out);
            req->send(200, "application/json", out);
        }));

    g_server.addHandler(new AsyncCallbackJsonWebHandler(
        "/api/presets/apply", [](AsyncWebServerRequest* req, JsonVariant& json) {
            if (!requireAuthentication(req)) return;
            const char* name = json["name"] | "";
            RuntimeSettings settings;
            if (!SettingsActions::applyPresetByName(name, settings)) {
                req->send(404, "application/json", "{\"error\":\"preset not found\"}");
                return;
            }

            JsonDocument resp;
            Telemetry::buildSettingsJson(resp);
            String out;
            serializeJson(resp, out);
            req->send(200, "application/json", out);
        }));

    g_server.begin();
}

}  // namespace LocalWebServer
