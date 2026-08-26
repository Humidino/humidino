#include "web_server.h"

#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>

#include "config.h"
#include "settings_store.h"
#include "shared_state.h"
#include "telemetry.h"

namespace {

AsyncWebServer g_server(WEB_SERVER_PORT);

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

            ShaState::updateSettings(settings);
            Settings::save(settings);

            JsonDocument resp;
            Telemetry::buildSettingsJson(resp);
            String out;
            serializeJson(resp, out);
            req->send(200, "application/json", out);
        }));

    g_server.begin();
}

}  // namespace LocalWebServer
