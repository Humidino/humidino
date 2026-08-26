#pragma once

// Тонкая обёртка над esp_task_wdt, чтобы все долгоживущие задачи
// регистрировались единообразно. Инициализируется один раз в main.cpp до
// создания любых задач; каждая задача вызывает Watchdog::registerCurrentTask()
// в начале своей функции и Watchdog::feed() раз за итерацию цикла.
namespace Watchdog {

void begin();
void registerCurrentTask();
void feed();

}  // namespace Watchdog
