#include "relay.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "backlight.h"
#include "config.h"
#include "watchdog.h"

namespace {

void setRelayPin(bool on) {
    bool level = RELAY_ACTIVE_HIGH ? on : !on;
    digitalWrite(PIN_RELAY_SSR, level ? HIGH : LOW);
}

class RelayController {
public:
    void begin() {
        pinMode(PIN_RELAY_SSR, OUTPUT);
        setRelayPin(false);
        status_ = RelayStatus{};
        status_.stateEnteredMs = millis();
    }

    void update(const SensorReading readings[4], const RuntimeSettings& cfg, uint32_t nowMs) {
        const SensorReading& intake = readings[static_cast<size_t>(SensorId::CrawlspaceIntake)];
        const SensorReading& corner = readings[static_cast<size_t>(SensorId::CrawlspaceCorner)];
        const SensorReading& outside = readings[static_cast<size_t>(SensorId::Outside)];

        bool sensorsHealthy = intake.valid && corner.valid && outside.valid &&
                               !intake.error && !corner.error && !outside.error;

        // Если хоть один из обязательных датчиков нездоров: не вычисляем
        // производные условия безопасности по устаревшим/мусорным данным —
        // считаем это состоянием «безопасность не подтверждена» и уходим в
        // ветку «не работать» ниже.
        float crawlRh = max(intake.humidityPct, corner.humidityPct);
        float crawlAbsMin = min(intake.absHumidityGm3, corner.absHumidityGm3);
        bool condensationSafe = sensorsHealthy && (outside.absHumidityGm3 < crawlAbsMin);
        bool freezeSafe = sensorsHealthy && (outside.temperatureC > cfg.freezeProtectC);

        RelayControlState next = status_.state;

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
            } else if (!sensorsHealthy) {
                next = RelayControlState::Idle;
            } else if (crawlRh > cfg.rhTargetPercent) {
                bool minPauseElapsed = (nowMs - status_.lastOffMs) > cfg.minPauseMs;
                next = minPauseElapsed ? RelayControlState::Running : RelayControlState::MinPauseHold;
            } else {
                next = RelayControlState::Idle;
            }
        }

        if (next != status_.state) {
            transitionTo(next, nowMs);
        }
    }

    RelayStatus status() const { return status_; }

private:
    RelayStatus status_;

    void transitionTo(RelayControlState next, uint32_t nowMs) {
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
            } else {
                status_.lastOffMs = nowMs;
            }
        }
    }
};

RelayController g_controller;

void controlTask(void*) {
    Watchdog::registerCurrentTask();
    g_controller.begin();

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
    xTaskCreatePinnedToCore(controlTask, "controlTask", 3072, nullptr, 4, nullptr, 0);
}

const char* bannerText(RelayControlState state) {
    switch (state) {
        case RelayControlState::Running:
            return "РАБОТА";
        case RelayControlState::Idle:
        case RelayControlState::MinPauseHold:
            return "ОЖИДАНИЕ";
        case RelayControlState::LockedOutCondensation:
            return "ЗАПРЕТ ПО КОНДЕНСАТУ";
        case RelayControlState::LockedOutFreeze:
            return "ЗАПРЕТ ПО МОРОЗУ";
    }
    return "?";
}

}  // namespace Relay
