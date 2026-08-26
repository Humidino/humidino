#pragma once
#include <cstdint>

// Запускает sensorTask (ядро 0), который опрашивает все 4 датчика SHT31 с
// периодом SENSOR_POLL_INTERVAL_MS, фильтрует показания скользящим средним,
// вычисляет точку росы / абсолютную влажность и публикует результат в ShaState.
namespace Sensors {

void begin();

}  // namespace Sensors
