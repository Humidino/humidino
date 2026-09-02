#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "shared_state.h"

// Журнал запусков реле — кольцевой буфер фиксированного размера на LittleFS
// (переживает перезагрузки), в который RelayController (relay.cpp) пишет
// одну запись на каждый цикл включения/выключения: когда реле включилось и
// выключилось (реальное время, если доступно — см. time_sync.h) и что
// показывали датчики в оба момента. Отвечает на вопрос "прийти на следующий
// день и посмотреть, сколько было запусков и какая была влажность".
//
// Пишется только из controlTask (relay.cpp), читается из HTTP-обработчиков
// (web_server.cpp, другой поток) и UI-экрана статистики — весь доступ идёт
// под мьютексом, как и в settings_store.cpp.
namespace RunLog {

enum class StopReason : uint8_t {
    Unknown = 0,             // запись ещё открыта (реле сейчас работает)
    HysteresisReached,       // обычный конец цикла: влажность опустилась ниже целевой минус гистерезис
    ManualOff,                // пользователь выключил вручную
    LockedFreeze,              // прервано защитой от замерзания
    LockedCondensation,        // прервано защитой от конденсата
    SensorFault,                // датчики отказали посреди работы
    Interrupted,                // устройство перезагрузилось во время работы — запись закрыта задним числом при старте
};

// Машиночитаемый идентификатор для JSON-API (по аналогии с
// toString(RelayControlState) в shared_state.h).
const char* toString(StopReason reason);

struct RunRecord {
    uint32_t startEpoch = 0;      // unix-время (UTC) начала цикла, 0 = не синхронизировано
    uint32_t endEpoch = 0;         // 0, пока запись открыта
    uint32_t durationMs = 0;        // 0, пока запись открыта
    // NAN (как и SensorReading в shared_state.h), а не 0 — 0% влажности или
    // 0°C сами по себе легитимные показания, ими нельзя кодировать "нет
    // данных". end*-поля остаются NAN, пока запись не закрыта recordStop().
    float startCrawlRh = NAN;
    float startCrawlTempC = NAN;
    float startOutsideRh = NAN;
    float startOutsideTempC = NAN;
    float endCrawlRh = NAN;
    float endCrawlTempC = NAN;
    float endOutsideRh = NAN;
    float endOutsideTempC = NAN;
    StopReason stopReason = StopReason::Unknown;
};

// Монтирует LittleFS (если ещё не смонтирован) и открывает/создаёт файл
// журнала. Вызывать один раз при старте, до Relay::begin().
void begin();

// Открывает новую запись (реле только что включилось) — снимок показаний
// датчиков на этот момент.
void recordStart(const SensorReading readings[static_cast<size_t>(SensorId::Count)]);

// Закрывает текущую открытую запись (реле только что выключилось).
// durationMs — сколько реле реально проработало в этом цикле (в relay.cpp
// уже есть точный millis()-таймер начала, пересчитывать его здесь не нужно).
void recordStop(const SensorReading readings[static_cast<size_t>(SensorId::Count)], StopReason reason,
                 uint32_t durationMs);

struct Summary {
    bool timeSynced = false;
    uint32_t runsToday = 0;        // только если timeSynced — иначе 0
    uint32_t runtimeTodayMs = 0;     // сумма durationMs завершённых сегодня циклов
    uint32_t runsTotal = 0;           // за всё время жизни устройства (Settings::loadCycleCount())
};

Summary getSummary();

// Копирует до maxCount последних записей (от самой новой к самой старой,
// offset пропускает offset самых свежих) в out. Возвращает, сколько реально
// записано.
size_t getRecent(RunRecord* out, size_t maxCount, size_t offset = 0);

// Сколько записей сейчас реально хранится в кольцевом буфере (<= capacity()).
size_t count();
size_t capacity();

}  // namespace RunLog
