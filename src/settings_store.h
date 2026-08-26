#pragma once
#include <cstdint>

#include "shared_state.h"

// Сохраняет редактируемую пользователем конфигурацию в NVS через
// Preferences.h. Небольшой плоский набор скаляров — NVS даёт атомарную
// запись по ключу без риска повредить всё при потере питания на середине
// записи (в отличие от одного JSON-файла на LittleFS).
namespace Settings {

void begin();

RuntimeSettings load();
void save(const RuntimeSettings& settings);

struct NetConfig {
    char mqttHost[64] = "";
    uint16_t mqttPort = DEFAULT_MQTT_PORT;
    char mqttUser[32] = "";
    char mqttPass[32] = "";
    char mqttBaseTopic[32] = "humidino";
};

NetConfig loadNet();
void saveNet(const NetConfig& net);

}  // namespace Settings
