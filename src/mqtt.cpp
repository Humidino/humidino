#include "mqtt.h"

#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "settings_store.h"
#include "shared_state.h"
#include "telemetry.h"

namespace {

WiFiClient g_wifiClient;
PubSubClient g_mqtt(g_wifiClient);
Settings::NetConfig g_netCfg;

char g_topicState[80];
char g_topicAvailability[80];
char g_topicSetWildcard[80];
char g_topicSetPrefix[80];

uint32_t g_lastReconnectAttemptMs = 0;
uint32_t g_lastPublishMs = 0;
constexpr uint32_t kReconnectIntervalMs = 5000;
constexpr uint32_t kPublishIntervalMs = 12000;

void buildTopics() {
    snprintf(g_topicState, sizeof(g_topicState), "%s/state", g_netCfg.mqttBaseTopic);
    snprintf(g_topicAvailability, sizeof(g_topicAvailability), "%s/availability", g_netCfg.mqttBaseTopic);
    snprintf(g_topicSetWildcard, sizeof(g_topicSetWildcard), "%s/set/#", g_netCfg.mqttBaseTopic);
    snprintf(g_topicSetPrefix, sizeof(g_topicSetPrefix), "%s/set/", g_netCfg.mqttBaseTopic);
}

struct DiscoveryEntity {
    const char* objectId;
    const char* name;
    const char* unit;        // nullptr, если единица измерения не нужна
    const char* deviceClass;  // nullptr, если класс устройства не нужен
    const char* valueTemplate;
    bool isBinarySensor;
};

const DiscoveryEntity kEntities[] = {
    {"crawl_intake_temp", "Crawlspace Intake Temperature", "°C", "temperature",
     "{{ value_json.zones.crawl_intake.temp_c }}", false},
    {"crawl_intake_rh", "Crawlspace Intake Humidity", "%", "humidity",
     "{{ value_json.zones.crawl_intake.rh_pct }}", false},
    {"crawl_intake_dew", "Crawlspace Intake Dew Point", "°C", "temperature",
     "{{ value_json.zones.crawl_intake.dew_c }}", false},
    {"crawl_corner_temp", "Crawlspace Corner Temperature", "°C", "temperature",
     "{{ value_json.zones.crawl_corner.temp_c }}", false},
    {"crawl_corner_rh", "Crawlspace Corner Humidity", "%", "humidity",
     "{{ value_json.zones.crawl_corner.rh_pct }}", false},
    {"crawl_corner_dew", "Crawlspace Corner Dew Point", "°C", "temperature",
     "{{ value_json.zones.crawl_corner.dew_c }}", false},
    {"outside_temp", "Outside Temperature", "°C", "temperature", "{{ value_json.zones.outside.temp_c }}", false},
    {"outside_rh", "Outside Humidity", "%", "humidity", "{{ value_json.zones.outside.rh_pct }}", false},
    {"house_temp", "House Temperature", "°C", "temperature", "{{ value_json.zones.house.temp_c }}", false},
    {"house_rh", "House Humidity", "%", "humidity", "{{ value_json.zones.house.rh_pct }}", false},
    {"wifi_rssi", "WiFi RSSI", "dBm", "signal_strength", "{{ value_json.wifi_rssi }}", false},
    {"free_heap", "Free Heap", "B", nullptr, "{{ value_json.free_heap }}", false},
};
constexpr size_t kEntityCount = sizeof(kEntities) / sizeof(kEntities[0]);

void publishDiscoveryEntity(const DiscoveryEntity& e) {
    char topic[128];
    const char* component = e.isBinarySensor ? "binary_sensor" : "sensor";
    snprintf(topic, sizeof(topic), "homeassistant/%s/humidino_%s/config", component, e.objectId);

    JsonDocument doc;
    char uniqueId[48];
    snprintf(uniqueId, sizeof(uniqueId), "humidino_%s", e.objectId);
    doc["name"] = e.name;
    doc["unique_id"] = uniqueId;
    doc["state_topic"] = g_topicState;
    doc["value_template"] = e.valueTemplate;
    doc["availability_topic"] = g_topicAvailability;
    if (e.unit != nullptr) doc["unit_of_measurement"] = e.unit;
    if (e.deviceClass != nullptr) doc["device_class"] = e.deviceClass;

    JsonObject device = doc["device"].to<JsonObject>();
    JsonArray ids = device["identifiers"].to<JsonArray>();
    ids.add("humidino_esp32s3");
    device["name"] = "Humidino";
    device["manufacturer"] = "DIY";
    device["model"] = "ESP32-S3";

    String payload;
    serializeJson(doc, payload);
    g_mqtt.publish(topic, payload.c_str(), true);
}

void publishFanDiscovery() {
    char topic[96];
    snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/humidino_fan/config");

    JsonDocument doc;
    doc["name"] = "Humidino Fan Running";
    doc["unique_id"] = "humidino_fan";
    doc["state_topic"] = g_topicState;
    doc["value_template"] = "{{ 'ON' if value_json.relay.on else 'OFF' }}";
    doc["payload_on"] = "ON";
    doc["payload_off"] = "OFF";
    doc["availability_topic"] = g_topicAvailability;

    JsonObject device = doc["device"].to<JsonObject>();
    JsonArray ids = device["identifiers"].to<JsonArray>();
    ids.add("humidino_esp32s3");
    device["name"] = "Humidino";

    String payload;
    serializeJson(doc, payload);
    g_mqtt.publish(topic, payload.c_str(), true);
}

void publishDiscovery() {
    for (size_t i = 0; i < kEntityCount; i++) {
        publishDiscoveryEntity(kEntities[i]);
    }
    publishFanDiscovery();
}

void onMessage(char* topic, uint8_t* payload, unsigned int len) {
    if (strncmp(topic, g_topicSetPrefix, strlen(g_topicSetPrefix)) != 0) return;
    const char* key = topic + strlen(g_topicSetPrefix);

    char valueBuf[32];
    unsigned int copyLen = len < sizeof(valueBuf) - 1 ? len : sizeof(valueBuf) - 1;
    memcpy(valueBuf, payload, copyLen);
    valueBuf[copyLen] = '\0';
    float value = atof(valueBuf);

    RuntimeSettings settings = ShaState::getSettings();
    if (strcmp(key, "rh_target") == 0) settings.rhTargetPercent = value;
    else if (strcmp(key, "hysteresis_pct") == 0) settings.hysteresisPercent = value;
    else if (strcmp(key, "freeze_c") == 0) settings.freezeProtectC = value;
    else return;

    ShaState::updateSettings(settings);
    Settings::save(settings);
}

bool tryConnect() {
    String clientId = String(DEVICE_HOSTNAME) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    const char* user = strlen(g_netCfg.mqttUser) > 0 ? g_netCfg.mqttUser : nullptr;
    const char* pass = strlen(g_netCfg.mqttPass) > 0 ? g_netCfg.mqttPass : nullptr;

    bool ok = g_mqtt.connect(clientId.c_str(), user, pass, g_topicAvailability, 1, true, "offline");
    if (ok) {
        g_mqtt.publish(g_topicAvailability, "online", true);
        g_mqtt.subscribe(g_topicSetWildcard);
        publishDiscovery();
    }
    return ok;
}

void publishState() {
    JsonDocument doc;
    Telemetry::buildStateJson(doc);
    String payload;
    serializeJson(doc, payload);
    g_mqtt.publish(g_topicState, payload.c_str(), true);
}

}  // namespace

namespace Mqtt {

void begin() {
    g_netCfg = Settings::loadNet();
    if (strlen(g_netCfg.mqttHost) == 0) return;  // MQTT не настроен — пропускаем полностью

    buildTopics();
    g_mqtt.setServer(g_netCfg.mqttHost, g_netCfg.mqttPort);
    g_mqtt.setCallback(onMessage);
    g_mqtt.setBufferSize(768);  // пейлоады discovery крупнее 256 байт по умолчанию
}

void loop() {
    if (strlen(g_netCfg.mqttHost) == 0) return;
    if (WiFi.status() != WL_CONNECTED) return;

    if (!g_mqtt.connected()) {
        uint32_t now = millis();
        if (now - g_lastReconnectAttemptMs >= kReconnectIntervalMs) {
            g_lastReconnectAttemptMs = now;
            tryConnect();
        }
        return;
    }

    g_mqtt.loop();

    uint32_t now = millis();
    if (now - g_lastPublishMs >= kPublishIntervalMs) {
        g_lastPublishMs = now;
        publishState();
    }
}

}  // namespace Mqtt
