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
        const SensorReading& intake = readings[static_cast<size_t>(SensorId::CrawlspaceIntake)];
        const SensorReading& outside = readings[static_cast<size_t>(SensorId::Outside)];

        bool sensorsHealthy = intake.valid && outside.valid && !intake.error && !outside.error;

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
            float crawlRh = intake.humidityPct;
            float crawlAbsMin = intake.absHumidityGm3;
            bool condensationSafe = outside.absHumidityGm3 < crawlAbsMin;

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
            transitionTo(next, nowMs, readings, cfg.mode);
        }
    }

    RelayStatus status() const { return status_; }

private:
    RelayStatus status_;

    void transitionTo(RelayControlState next, uint32_t nowMs,
                       const SensorReading readings[static_cast<size_t>(SensorId::Count)], OperatingMode mode) {
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
                RunLog::recordStart(readings);
            } else {
                status_.lastOffMs = nowMs;
                RunLog::recordStop(readings, stopReasonFor(next, mode), nowMs - status_.lastOnMs);
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
