#pragma once

// Запуск captive-portal через WiFiManager — вынесен в отдельную единицу
// трансляции, потому что WiFiManager.h (через WebServer.h из ядра ESP32) и
// ESPAsyncWebServer.h определяют конфликтующие макросы HTTP_GET/HTTP_POST/...
// и не могут быть подключены в одном файле одновременно.
namespace WifiProvision {

// Блокирует вызывающую задачу до подключения или до истечения таймаута
// портала настройки. Возвращает true только после успешного подключения.
bool begin();

// Забывает сохранённые Wi-Fi credentials (хранятся самим esp_wifi/
// WiFiManager, а не в наших NVS-namespace — см. settings_store.cpp) — не
// трогает пороги, пресеты и веб-пароль. Следующий begin() (после
// перезагрузки) снова откроет портал настройки. Не перезагружает
// устройство сам — это решает вызывающая сторона (см. web_server.cpp).
void forgetCredentials();

}  // namespace WifiProvision
