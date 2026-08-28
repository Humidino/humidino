#pragma once

// Обёртка над PubSubClient: неблокирующее переподключение с бэкоффом,
// публикация телеметрии в JSON, Home Assistant MQTT Discovery. begin()
// вызывается один раз из netTask после подъёма Wi-Fi; loop() нужно вызывать
// часто (тоже из netTask) — никогда не блокирует.
namespace Mqtt {

void begin();
void loop();

// Перечитывает сетевые настройки из NVS и переподключается с ними —
// вызывается из web_server.cpp после сохранения новых настроек MQTT/сети,
// чтобы не требовать перезагрузки устройства.
void reconfigure();

}  // namespace Mqtt
