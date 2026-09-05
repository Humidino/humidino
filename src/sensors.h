#pragma once
#include <cstdint>

// Запускает sensorTask (ядро 0), который опрашивает все 4 датчика SHT31 —
// два на аппаратных шинах I2C (Wire/Wire1) и два на программных bit-bang
// шинах (см. soft_i2c.h) — с периодом SENSOR_POLL_INTERVAL_MS, фильтрует
// показания скользящим средним, вычисляет точку росы / абсолютную влажность
// и публикует результат в ShaState.
namespace Sensors {

void begin();

}  // namespace Sensors
