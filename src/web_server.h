#pragma once

// Локальный веб-интерфейс мониторинга/настроек (ESPAsyncWebServer + REST).
// Вынесен в отдельную единицу трансляции, отдельно от wifi_provision.cpp —
// причина, почему WiFiManager.h и ESPAsyncWebServer.h нельзя подключать в
// одном файле, описана в комментарии там.
namespace LocalWebServer {

void begin();

}  // namespace LocalWebServer
