#include "web_server.h"

#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <cstring>

#include "config.h"
#include "mqtt.h"
#include "settings_store.h"
#include "shared_state.h"
#include "telemetry.h"

namespace {

AsyncWebServer g_server(WEB_SERVER_PORT);

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
    LittleFS.begin(true);
    g_server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    g_server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        Telemetry::buildStateJson(doc);
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    g_server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        Telemetry::buildSettingsJson(doc);
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    g_server.addHandler(new AsyncCallbackJsonWebHandler(
        "/api/settings", [](AsyncWebServerRequest* req, JsonVariant& json) {
            JsonObject body = json.as<JsonObject>();
            RuntimeSettings settings = ShaState::getSettings();

            if (body["rh_target"].is<float>()) settings.rhTargetPercent = body["rh_target"];
            if (body["hysteresis_pct"].is<float>()) settings.hysteresisPercent = body["hysteresis_pct"];
            if (body["freeze_c"].is<float>()) settings.freezeProtectC = body["freeze_c"];
            if (body["min_runtime_ms"].is<uint32_t>()) settings.minRuntimeMs = body["min_runtime_ms"];
            if (body["min_pause_ms"].is<uint32_t>()) settings.minPauseMs = body["min_pause_ms"];
            if (body["mode"].is<const char*>()) settings.mode = operatingModeFromString(body["mode"]);

            ShaState::updateSettings(settings);
            Settings::save(settings);

            JsonDocument resp;
            Telemetry::buildSettingsJson(resp);
            String out;
            serializeJson(resp, out);
            req->send(200, "application/json", out);
        }));

    // --- Пресеты порогов ---
    g_server.on("/api/presets", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        writePresetsJson(doc, Settings::loadPresets());
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    g_server.addHandler(new AsyncCallbackJsonWebHandler(
        "/api/presets", [](AsyncWebServerRequest* req, JsonVariant& json) {
            JsonArray arr = json.as<JsonArray>();
            std::vector<Settings::Preset> presets;
            for (JsonObject p : arr) {
                if (presets.size() >= Settings::kMaxPresets) break;
                Settings::Preset preset;
                strncpy(preset.name, (p["name"] | ""), sizeof(preset.name) - 1);
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
            const char* name = json["name"] | "";
            std::vector<Settings::Preset> presets = Settings::loadPresets();
            const Settings::Preset* found = nullptr;
            for (const auto& p : presets) {
                if (strcmp(p.name, name) == 0) {
                    found = &p;
                    break;
                }
            }
            if (found == nullptr) {
                req->send(404, "application/json", "{\"error\":\"preset not found\"}");
                return;
            }

            RuntimeSettings settings = ShaState::getSettings();
            settings.rhTargetPercent = found->values.rhTargetPercent;
            settings.hysteresisPercent = found->values.hysteresisPercent;
            settings.freezeProtectC = found->values.freezeProtectC;
            settings.minRuntimeMs = found->values.minRuntimeMs;
            settings.minPauseMs = found->values.minPauseMs;

            ShaState::updateSettings(settings);
            Settings::save(settings);

            JsonDocument resp;
            Telemetry::buildSettingsJson(resp);
            String out;
            serializeJson(resp, out);
            req->send(200, "application/json", out);
        }));

    // --- Настройки сети / MQTT ---
    // Пароль брокера никогда не возвращается в GET — его видно только тому,
    // кто уже его знает; POST без поля mqtt_pass (или с пустым значением)
    // оставляет сохранённый пароль без изменений.
    g_server.on("/api/network", HTTP_GET, [](AsyncWebServerRequest* req) {
        Settings::NetConfig net = Settings::loadNet();
        JsonDocument doc;
        doc["mqtt_host"] = net.mqttHost;
        doc["mqtt_port"] = net.mqttPort;
        doc["mqtt_user"] = net.mqttUser;
        doc["mqtt_topic"] = net.mqttBaseTopic;
        doc["mqtt_pass_set"] = strlen(net.mqttPass) > 0;
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    g_server.addHandler(new AsyncCallbackJsonWebHandler(
        "/api/network", [](AsyncWebServerRequest* req, JsonVariant& json) {
            Settings::NetConfig net = Settings::loadNet();
            JsonObject body = json.as<JsonObject>();

            if (body["mqtt_host"].is<const char*>())
                strncpy(net.mqttHost, body["mqtt_host"].as<const char*>(), sizeof(net.mqttHost) - 1);
            if (body["mqtt_port"].is<uint16_t>()) net.mqttPort = body["mqtt_port"];
            if (body["mqtt_user"].is<const char*>())
                strncpy(net.mqttUser, body["mqtt_user"].as<const char*>(), sizeof(net.mqttUser) - 1);
            if (body["mqtt_pass"].is<const char*>() && strlen(body["mqtt_pass"].as<const char*>()) > 0)
                strncpy(net.mqttPass, body["mqtt_pass"].as<const char*>(), sizeof(net.mqttPass) - 1);
            if (body["mqtt_topic"].is<const char*>())
                strncpy(net.mqttBaseTopic, body["mqtt_topic"].as<const char*>(), sizeof(net.mqttBaseTopic) - 1);

            Settings::saveNet(net);
            Mqtt::reconfigure();

            JsonDocument resp;
            resp["mqtt_host"] = net.mqttHost;
            resp["mqtt_port"] = net.mqttPort;
            resp["mqtt_user"] = net.mqttUser;
            resp["mqtt_topic"] = net.mqttBaseTopic;
            resp["mqtt_pass_set"] = strlen(net.mqttPass) > 0;
            String out;
            serializeJson(resp, out);
            req->send(200, "application/json", out);
        }));

    g_server.begin();
}

}  // namespace LocalWebServer
