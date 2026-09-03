#pragma once
#include <ArduinoJson.h>
#include <cstddef>

// Общий формат JSON для телеметрии/настроек, который использует локальный
// веб-интерфейс (web_server.cpp).
namespace Telemetry {

void buildStateJson(JsonDocument& doc);
void buildSettingsJson(JsonDocument& doc);

// Сводка статистики запусков (run_log.h) — сегодня/всего.
void buildHistorySummaryJson(JsonDocument& doc);

// Последние записи журнала запусков, от самой свежей к самой старой.
// limit ограничивается разумным потолком внутри (см. .cpp) — большой лимит
// от клиента не приводит к неограниченному выделению памяти.
void buildHistoryJson(JsonDocument& doc, size_t limit, size_t offset);

}  // namespace Telemetry
