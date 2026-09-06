#include "backlight.h"

#include <Arduino.h>
#include <atomic>

#include "config.h"

// Другие задачи только сообщают об активности. Яркостью, таймером и
// плавным гашением владеет исключительно lvglTask.
namespace {

constexpr uint32_t kPwmFreqHz = 5000;
constexpr uint8_t kPwmResolutionBits = 8;
constexpr uint8_t kPwmMaxDuty = 255;
// В этой версии ядра доступен старый (до 3.0) API ledc на основе каналов
// (ledcSetup/ledcAttachPin/ledcWrite по номеру канала, а не по пину).
constexpr uint8_t kPwmChannel = 0;

uint8_t g_currentPct = 0;
uint8_t g_targetPct = 0;
uint8_t g_fadeStartPct = 0;
uint32_t g_fadeStartMs = 0;
bool g_fading = false;
uint32_t g_lastActivityMs = 0;
std::atomic<bool> g_activityPending{false};

void writeDuty(uint8_t pct) {
    uint32_t duty = (static_cast<uint32_t>(pct) * kPwmMaxDuty) / 100;
    ledcWrite(kPwmChannel, duty);
}

void startFadeTo(uint8_t targetPct, uint32_t nowMs) {
    g_fadeStartPct = g_currentPct;
    g_targetPct = targetPct;
    g_fadeStartMs = nowMs;
    g_fading = true;
}

}  // namespace

namespace Backlight {

void begin() {
    ledcSetup(kPwmChannel, kPwmFreqHz, kPwmResolutionBits);
    ledcAttachPin(PIN_TFT_BACKLIGHT, kPwmChannel);
    g_lastActivityMs = millis();
    setLevel(BACKLIGHT_FULL_PCT);
}

void setLevel(uint8_t pct) {
    g_fading = false;
    g_currentPct = pct;
    g_targetPct = pct;
    writeDuty(pct);
}

void noteActivity() {
    g_activityPending.store(true, std::memory_order_relaxed);
}

void update(uint32_t nowMs) {
    if (g_activityPending.exchange(false, std::memory_order_relaxed)) {
        g_lastActivityMs = nowMs;
        // Пробуждение немедленное: повторные касания не перезапускают fade.
        if (g_fading || g_currentPct != BACKLIGHT_FULL_PCT) setLevel(BACKLIGHT_FULL_PCT);
        return;
    }
    if (!g_fading && g_targetPct == BACKLIGHT_FULL_PCT &&
        (nowMs - g_lastActivityMs) >= BACKLIGHT_DIM_TIMEOUT_MS) {
        startFadeTo(BACKLIGHT_DIM_PCT, nowMs);
    }

    if (!g_fading) return;

    uint32_t elapsed = nowMs - g_fadeStartMs;
    if (elapsed >= BACKLIGHT_FADE_MS) {
        g_currentPct = g_targetPct;
        g_fading = false;
        writeDuty(g_currentPct);
        return;
    }

    float progress = static_cast<float>(elapsed) / static_cast<float>(BACKLIGHT_FADE_MS);
    int16_t delta = static_cast<int16_t>(g_targetPct) - static_cast<int16_t>(g_fadeStartPct);
    g_currentPct = static_cast<uint8_t>(g_fadeStartPct + delta * progress);
    writeDuty(g_currentPct);
}

}  // namespace Backlight
