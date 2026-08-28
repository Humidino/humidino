#include "shared_state.h"

#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {
SystemState g_state;
SemaphoreHandle_t g_mutex = nullptr;
constexpr TickType_t kLockTimeout = pdMS_TO_TICKS(50);
}  // namespace

const char* toString(OperatingMode mode) {
    switch (mode) {
        case OperatingMode::ManualOn:
            return "manual_on";
        case OperatingMode::ManualOff:
            return "manual_off";
        case OperatingMode::Auto:
        default:
            return "auto";
    }
}

OperatingMode operatingModeFromString(const char* s) {
    if (s == nullptr) return OperatingMode::Auto;
    if (strcmp(s, "manual_on") == 0) return OperatingMode::ManualOn;
    if (strcmp(s, "manual_off") == 0) return OperatingMode::ManualOff;
    return OperatingMode::Auto;
}

const char* toString(RelayControlState state) {
    switch (state) {
        case RelayControlState::Running:
            return "running";
        case RelayControlState::LockedOutCondensation:
            return "locked_condensation";
        case RelayControlState::LockedOutFreeze:
            return "locked_freeze";
        case RelayControlState::MinPauseHold:
            return "min_pause_hold";
        case RelayControlState::LockedOutSensorFault:
            return "locked_sensor_fault";
        case RelayControlState::Idle:
        default:
            return "idle";
    }
}

namespace ShaState {

void begin() {
    if (g_mutex == nullptr) {
        SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
        if (mutex == nullptr) return;
        g_mutex = mutex;
    }
}

bool getSnapshot(SystemState& out) {
    if (g_mutex == nullptr) return false;
    if (xSemaphoreTake(g_mutex, kLockTimeout) != pdTRUE) return false;
    out = g_state;
    xSemaphoreGive(g_mutex);
    return true;
}

void updateSensor(SensorId id, const SensorReading& reading) {
    if (g_mutex == nullptr) return;
    if (xSemaphoreTake(g_mutex, kLockTimeout) != pdTRUE) return;
    g_state.readings[static_cast<size_t>(id)] = reading;
    xSemaphoreGive(g_mutex);
}

void updateRelay(const RelayStatus& status) {
    if (g_mutex == nullptr) return;
    if (xSemaphoreTake(g_mutex, kLockTimeout) != pdTRUE) return;
    g_state.relay = status;
    xSemaphoreGive(g_mutex);
}

void updateSettings(const RuntimeSettings& settings) {
    if (g_mutex == nullptr) return;
    if (xSemaphoreTake(g_mutex, kLockTimeout) != pdTRUE) return;
    g_state.settings = settings;
    xSemaphoreGive(g_mutex);
}

void updateWifi(bool connected, int8_t rssi) {
    if (g_mutex == nullptr) return;
    if (xSemaphoreTake(g_mutex, kLockTimeout) != pdTRUE) return;
    g_state.wifiConnected = connected;
    g_state.wifiRssi = rssi;
    xSemaphoreGive(g_mutex);
}

RuntimeSettings getSettings() {
    RuntimeSettings s;
    if (g_mutex == nullptr) return s;
    if (xSemaphoreTake(g_mutex, kLockTimeout) == pdTRUE) {
        s = g_state.settings;
        xSemaphoreGive(g_mutex);
    }
    return s;
}

}  // namespace ShaState
