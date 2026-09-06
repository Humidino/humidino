#pragma once
#include <cstdint>

// Касание или смена состояния реле будит подсветку (GPIO2) немедленно.
// После BACKLIGHT_DIM_TIMEOUT_MS бездействия яркость плавно уменьшается.
namespace Backlight {

void begin();
void setLevel(uint8_t pct);   // 0-100 %, немедленно (без плавного перехода)
void noteActivity();          // потокобезопасное уведомление о касании/смене реле
void update(uint32_t nowMs);  // вызывать на каждом тике lvglTask; ведёт плавный переход

}  // namespace Backlight
