#pragma once
#include <ArduinoJson.h>

// Общий формат JSON для телеметрии/настроек, который использует и локальный
// веб-интерфейс (network.cpp), и публикация в MQTT (mqtt.cpp) — так они не
// расходятся между собой.
namespace Telemetry {

void buildStateJson(JsonDocument& doc);
void buildSettingsJson(JsonDocument& doc);

}  // namespace Telemetry
