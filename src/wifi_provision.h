#pragma once

// Запуск captive-portal через WiFiManager — вынесен в отдельную единицу
// трансляции, потому что WiFiManager.h (через WebServer.h из ядра ESP32) и
// ESPAsyncWebServer.h определяют конфликтующие макросы HTTP_GET/HTTP_POST/...
// и не могут быть подключены в одном файле одновременно.
namespace WifiProvision {

// Блокирует вызывающую задачу до подключения или до истечения таймаута
// портала настройки. Возвращает true только после успешного подключения.
bool begin();

}  // namespace WifiProvision
