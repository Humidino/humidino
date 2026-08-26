#pragma once
#include <cmath>
#include <cstdint>

#include "config.h"

// ============================================================================
// Модель данных, общая для sensorTask, controlTask, lvglTask и netTask.
// Любой доступ идёт только через ShaState::getSnapshot()/update* ниже —
// напрямую трогать структуру из более чем одной задачи нельзя.
// ============================================================================

struct SensorReading {
    float temperatureC = NAN;
    float humidityPct = NAN;
    float dewPointC = NAN;
    float absHumidityGm3 = NAN;  // г/м³
    bool valid = false;   // true после хотя бы одного успешного измерения
    bool error = false;   // true, если последний опрос не удался (бейдж «ERR»)
    uint32_t lastUpdateMs = 0;   // millis() на момент последнего обновления
};

enum class RelayControlState : uint8_t {
    Idle,
    Running,
    LockedOutCondensation,
    LockedOutFreeze,
    MinPauseHold
};

struct RelayStatus {
    RelayControlState state = RelayControlState::Idle;
    bool relayOn = false;
    uint32_t stateEnteredMs = 0;
    uint32_t lastOnMs = 0;
    uint32_t lastOffMs = 0;
};

struct RuntimeSettings {
    float rhTargetPercent = DEFAULT_RH_TARGET_PERCENT;
    float hysteresisPercent = DEFAULT_HYSTERESIS_PERCENT;
    float freezeProtectC = FREEZE_PROTECT_TEMP_C;
    uint32_t minRuntimeMs = MIN_RUNTIME_MS;
    uint32_t minPauseMs = MIN_PAUSE_MS;
};

struct SystemState {
    SensorReading readings[static_cast<size_t>(SensorId::Count)];
    RelayStatus relay;
    RuntimeSettings settings;
    bool wifiConnected = false;
    int8_t wifiRssi = 0;
};

// Доступ под мьютексом. Каждый вызов берёт блокировку с коротким таймаутом,
// чтобы зависшая операция I2C/SPI/сети в одной задаче не могла превратиться
// в общесистемное зависание другой задачи.
namespace ShaState {

void begin();

// Копирует всё состояние под блокировкой. Возвращает false (не трогая
// `out`), если не удалось взять блокировку за отведённый таймаут.
bool getSnapshot(SystemState& out);

void updateSensor(SensorId id, const SensorReading& reading);
void updateRelay(const RelayStatus& status);
void updateSettings(const RuntimeSettings& settings);
void updateWifi(bool connected, int8_t rssi);

// Вспомогательная функция для вызывающих, которым нужно только одно поле —
// не копирует всю структуру целиком.
RuntimeSettings getSettings();

}  // namespace ShaState
