#pragma once
#include <ArduinoJson.h>

// Общий формат JSON для телеметрии/настроек, который использует локальный
// веб-интерфейс (web_server.cpp).
namespace Telemetry {

void buildStateJson(JsonDocument& doc);
void buildSettingsJson(JsonDocument& doc);

}  // namespace Telemetry
