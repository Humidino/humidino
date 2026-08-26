#pragma once

// Отвечает за инициализацию LVGL + TFT_eSPI (буферы отрисовки в PSRAM) и
// запускает lvglTask на ядре 1, который крутит lv_timer_handler(), а через
// более редкий таймер LVGL — обновление данных дашборда (ui_dashboard.cpp)
// и плавный переход подсветки (backlight.cpp).
namespace Display {

void begin();

}  // namespace Display
