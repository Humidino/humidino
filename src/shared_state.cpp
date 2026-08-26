#include "shared_state.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {
SystemState g_state;
SemaphoreHandle_t g_mutex = nullptr;
constexpr TickType_t kLockTimeout = pdMS_TO_TICKS(50);
}  // namespace

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
