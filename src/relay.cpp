#include "relay.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "backlight.h"
#include "config.h"
#include "run_log.h"
#include "settings_store.h"
#include "watchdog.h"

namespace {

void setRelayPin(bool on) {
    bool level = RELAY_ACTIVE_HIGH ? on : !on;
    digitalWrite(PIN_RELAY_SSR, level ? HIGH : LOW);
}

// Причина остановки для журнала (run_log.h) — определяется состоянием, в
// которое реле переходит, а для обычного (не аварийного) конца цикла ещё и
// режимом: Idle/MinPauseHold после ручного выключения — это ManualOff, после
// авто-цикла по гистерезису — HysteresisReached.
RunLog::StopReason stopReasonFor(RelayControlState next, OperatingMode mode) {
    switch (next) {
        case RelayControlState::LockedOutFreeze:
            return RunLog::StopReason::LockedFreeze;
        case RelayControlState::LockedOutCondensation:
            return RunLog::StopReason::LockedCondensation;
        case RelayControlState::LockedOutSensorFault:
            return RunLog::StopReason::SensorFault;
        default:
            return (mode == OperatingMode::ManualOff) ? RunLog::StopReason::ManualOff
                                                       : RunLog::StopReason::HysteresisReached;
    }
}

// Сводка по всем живым (valid && !error) датчикам подпола. Порог включения/
// выключения по влажности берёт MAX по зонам — иначе одна сухая зона может
// маскировать сырую, а именно сырые места и важно сушить. Порог защиты от
// конденсата берёт MIN абсолютной влажности — самую сухую зону подпола:
// нельзя пускать уличный воздух, если он более влажный (абсолютно), чем
// воздух в самой сухой части подпола, иначе именно там станет хуже, даже
// если в среднем по подполу воздух остаётся суше уличного. avgTempC — только
// для журнала (не участвует в пороговых решениях), поэтому усреднение здесь
// уместно в отличие от RH/abs-humidity.
struct CrawlspaceSummary {
    bool anyLive = false;
    uint8_t liveCount = 0;
    float maxRhPercent = NAN;
    float minAbsHumidityGm3 = NAN;
    float avgTempC = NAN;
};

CrawlspaceSummary summarizeCrawlspace(const SensorReading readings[static_cast<size_t>(SensorId::Count)]) {
    CrawlspaceSummary s;
    float tempSum = 0.0f;
    for (size_t i = 0; i < static_cast<size_t>(SensorId::Count); i++) {
        SensorId id = static_cast<SensorId>(i);
        if (!isCrawlspaceSensor(id)) continue;
        const SensorReading& r = readings[i];
        if (!r.valid || r.error) continue;

        if (!s.anyLive || r.humidityPct > s.maxRhPercent) s.maxRhPercent = r.humidityPct;
        if (!s.anyLive || r.absHumidityGm3 < s.minAbsHumidityGm3) s.minAbsHumidityGm3 = r.absHumidityGm3;
        tempSum += r.temperatureC;
        s.anyLive = true;
        s.liveCount++;
    }
    if (s.anyLive) s.avgTempC = tempSum / s.liveCount;
    return s;
}

class RelayController {
public:
    void begin(uint32_t initialMinPauseMs) {
        pinMode(PIN_RELAY_SSR, OUTPUT);
        setRelayPin(false);
        status_ = RelayStatus{};
        status_.stateEnteredMs = millis();
        status_.lastOffMs = status_.stateEnteredMs - initialMinPauseMs - 1;
        status_.cycleCount = Settings::loadCycleCount();
    }

    void update(const SensorReading readings[static_cast<size_t>(SensorId::Count)], const RuntimeSettings& cfg, uint32_t nowMs) {
        const SensorReading& outside = readings[static_cast<size_t>(SensorId::Outside)];
        CrawlspaceSummary crawl = summarizeCrawlspace(readings);
        status_.crawlspaceLiveSensors = crawl.liveCount;

        // Уличный датчик не резервирован (в отличие от подпола) — его отказ
        // всегда блокирует реле целиком: без него нельзя проверить ни мороз,
        // ни конденсат. Подпол блокирует реле, только если живых датчиков не
        // осталось вообще (crawl.anyLive=false); пока жив хотя бы один —
        // работаем по summarizeCrawlspace() дальше (деградированный режим,
        // а не блокировка).
        bool sensorsHealthy = outside.valid && !outside.error && crawl.anyLive;

        // Заморозка — единственная защита, которая действует безусловно даже
        // в ручных режимах (физическая безопасность конструкции важнее любой
        // команды пользователя). Без исправных датчиков её проверить нельзя,
        // поэтому при неисправности считаем условие небезопасным.
        bool freezeSafe = sensorsHealthy && (outside.temperatureC > cfg.freezeProtectC);

        RelayControlState next = status_.state;

        if (cfg.mode == OperatingMode::ManualOff) {
            // Прямая команда пользователя — выключаем немедленно, не дожидаясь
            // min-runtime (это не автоматический цикл, а явный запрос "выкл").
            next = RelayControlState::Idle;
        } else if (cfg.mode == OperatingMode::ManualOn) {
            if (!sensorsHealthy) {
                next = RelayControlState::LockedOutSensorFault;
            } else if (!freezeSafe) {
                next = RelayControlState::LockedOutFreeze;
            } else {
                // Защита от конденсата и порог влажности намеренно
                // игнорируются в ручном режиме — пользователь явно просит
                // работать, min-pause тоже не действует (это не автоцикл).
                next = RelayControlState::Running;
            }
        } else if (!sensorsHealthy) {
            next = RelayControlState::LockedOutSensorFault;
        } else {
            float crawlRh = crawl.maxRhPercent;
            bool condensationSafe = outside.absHumidityGm3 < crawl.minAbsHumidityGm3;

            if (status_.state == RelayControlState::Running) {
                // Отключения по физической защите немедленно перекрывают
                // минимальное время работы — оно защищает реле только от износа
                // при частых включениях/выключениях и не является блокировкой
                // по безопасности.
                if (!freezeSafe) {
                    next = RelayControlState::LockedOutFreeze;
                } else if (!condensationSafe) {
                    next = RelayControlState::LockedOutCondensation;
                } else {
                    bool minRuntimeElapsed = (nowMs - status_.stateEnteredMs) >= cfg.minRuntimeMs;
                    bool belowHysteresis = crawlRh < (cfg.rhTargetPercent - cfg.hysteresisPercent);
                    if (minRuntimeElapsed && belowHysteresis) {
                        next = RelayControlState::Idle;
                    }
                }
            } else {
                if (!freezeSafe) {
                    next = RelayControlState::LockedOutFreeze;
                } else if (!condensationSafe) {
                    next = RelayControlState::LockedOutCondensation;
                } else if (crawlRh > cfg.rhTargetPercent) {
                    bool minPauseElapsed = (nowMs - status_.lastOffMs) > cfg.minPauseMs;
                    next = minPauseElapsed ? RelayControlState::Running : RelayControlState::MinPauseHold;
                } else {
                    next = RelayControlState::Idle;
                }
            }
        }

        if (next != status_.state) {
            transitionTo(next, nowMs, outside, crawl, cfg.mode);
        }
    }

    RelayStatus status() const { return status_; }

private:
    RelayStatus status_;

    void transitionTo(RelayControlState next, uint32_t nowMs, const SensorReading& outside,
                       const CrawlspaceSummary& crawl, OperatingMode mode) {
        bool wasRunning = status_.relayOn;
        status_.state = next;
        status_.stateEnteredMs = nowMs;
        Backlight::noteActivity();

        bool shouldRun = (next == RelayControlState::Running);
        if (shouldRun != wasRunning) {
            setRelayPin(shouldRun);
            status_.relayOn = shouldRun;
            if (shouldRun) {
                status_.lastOnMs = nowMs;
                status_.cycleCount++;
                Settings::saveCycleCount(status_.cycleCount);
                RunLog::recordStart(crawl.maxRhPercent, crawl.avgTempC, outside.humidityPct, outside.temperatureC);
            } else {
                status_.lastOffMs = nowMs;
                RunLog::recordStop(crawl.maxRhPercent, crawl.avgTempC, outside.humidityPct, outside.temperatureC,
                                    stopReasonFor(next, mode), nowMs - status_.lastOnMs);
            }
        }
    }
};

RelayController g_controller;

void controlTask(void*) {
    Watchdog::registerCurrentTask();
    g_controller.begin(ShaState::getSettings().minPauseMs);

    for (;;) {
        SystemState snapshot;
        if (ShaState::getSnapshot(snapshot)) {
            uint32_t now = millis();
            g_controller.update(snapshot.readings, snapshot.settings, now);
            ShaState::updateRelay(g_controller.status());
        }
        Watchdog::feed();
        vTaskDelay(pdMS_TO_TICKS(CONTROL_EVAL_INTERVAL_MS));
    }
}

}  // namespace

namespace Relay {

void begin() {
    // Стек увеличен с 3072 до 5120 байт: с добавлением журнала запусков
    // (run_log.h) controlTask теперь на каждом включении/выключении реле
    // делает файловый ввод-вывод на LittleFS (открытие/запись/закрытие через
    // VFS) — это заметно тяжелее по стеку, чем прежние только NVS-записи
    // (Settings::saveCycleCount()), и не проверялось на реальном железе.
    xTaskCreatePinnedToCore(controlTask, "controlTask", 5120, nullptr, 4, nullptr, 0);
}

const char* modeBadgeText(OperatingMode mode) {
    switch (mode) {
        case OperatingMode::ManualOn:
            return "РУЧНОЕ ВКЛ";
        case OperatingMode::ManualOff:
            return "РУЧНОЕ ВЫКЛ";
        case OperatingMode::Auto:
        default:
            return "АВТО";
    }
}

}  // namespace Relay
