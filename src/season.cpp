#include "season.h"

#include <Arduino.h>
#include <ctime>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config.h"
#include "settings_actions.h"
#include "shared_state.h"
#include "time_sync.h"
#include "watchdog.h"

namespace {

// Сезон физически не может смениться быстрее чем раз в месяц — раз в 10 мин
// более чем достаточно, но при этом достаточно часто, чтобы подхватить смену
// сразу после первой NTP-синхронизации после включения устройства.
constexpr uint32_t kCheckIntervalMs = 10UL * 60 * 1000;

Season::Id g_current = Season::Id::Winter;
bool g_haveCheckedOnce = false;

void applySeasonProfile(Season::Id season) {
    RuntimeSettings settings = ShaState::getSettings();
    Settings::PresetValues profile = Season::profileFor(season);
    settings.rhTargetPercent = profile.rhTargetPercent;
    settings.hysteresisPercent = profile.hysteresisPercent;
    settings.freezeProtectC = profile.freezeProtectC;
    settings.minRuntimeMs = profile.minRuntimeMs;
    settings.minPauseMs = profile.minPauseMs;
    SettingsActions::applyRuntimeSettings(settings);
    Serial.printf("Season: применён профиль \"%s\" (Лотошино, МО)\n", Season::toString(season));
}

void seasonTask(void*) {
    Watchdog::registerCurrentTask();
    for (;;) {
        if (TimeSync::isSynced()) {
            Season::Id season = Season::forEpoch(TimeSync::nowEpoch());
            bool changed = !g_haveCheckedOnce || season != g_current;
            g_current = season;
            g_haveCheckedOnce = true;
            if (changed && ShaState::getSettings().seasonAutoEnabled) {
                applySeasonProfile(season);
            }
        }
        Watchdog::feed();
        vTaskDelay(pdMS_TO_TICKS(kCheckIntervalMs));
    }
}

}  // namespace

namespace Season {

const char* toString(Id season) {
    switch (season) {
        case Id::Winter: return "winter";
        case Id::Spring: return "spring";
        case Id::Summer: return "summer";
        case Id::Autumn: return "autumn";
    }
    return "winter";
}

Id forLocalMonth(int month1to12) {
    switch (month1to12) {
        case 12:
        case 1:
        case 2:
            return Id::Winter;
        case 3:
        case 4:
        case 5:
            return Id::Spring;
        case 6:
        case 7:
        case 8:
            return Id::Summer;
        default:  // 9, 10, 11
            return Id::Autumn;
    }
}

Id forEpoch(uint32_t utcEpoch) {
    time_t localT = static_cast<time_t>(utcEpoch) + LOCAL_TZ_OFFSET_SEC;
    struct tm tmResult{};
    gmtime_r(&localT, &tmResult);
    return forLocalMonth(tmResult.tm_mon + 1);
}

// ---------------------------------------------------------------------------
// Значения по сезону для Лотошино, Московская область — см.
// docs/SEASONAL_LOTOSHINO.md за подробным разбором климата и обоснованием
// каждого числа. Коротко:
//
// - Зима: безопасные окна вентиляции редки и коротки (только оттепели около
//   0°C) — порог мороза чуть выше базового отсекает пограничные оттепели с
//   высокой влажностью (риск наледи на конструкциях), широкий гистерезис и
//   длинная пауза берегут реле и тепло подпола от частых включений в мороз.
// - Весна: снеготаяние и оттаивание грунта — пик влажности подпола за год.
//   Самая жёсткая цель влажности и самый быстрый цикл, чтобы успевать
//   выгонять влагу; порог мороза ещё повышен — заморозки на почве в
//   марте-апреле обычны.
// - Лето: заморозки практически исключены — порог мороза почти символический.
//   Главный ограничитель в жару — защита от конденсата (сравнение абсолютной
//   влажности, действует всегда, не зависит от сезона): в душные дни после
//   дождя она и так не даст тянуть с улицы воздух. Более длинный runtime и
//   широкий гистерезис — чтобы не дёргать реле в те редкие окна, когда
//   уличный воздух суше подпольного.
// - Осень: второй пик влажности за год (осенние дожди) на фоне
//   приближающихся заморозков — порог мороза плавно возвращается к зимнему,
//   цель и тайминги — среднее между летом и весной.
//
// Это осмысленные отправные точки под конкретный климат, а не измеренная
// калибровка конкретного подпола — донастройте после первого сезона
// наблюдений (см. "Аналитика и статистика" в README §4.2).
// ---------------------------------------------------------------------------
Settings::PresetValues profileFor(Id season) {
    Settings::PresetValues v;
    switch (season) {
        case Id::Winter:
            v.rhTargetPercent = 75.0f;
            v.hysteresisPercent = 6.0f;
            v.freezeProtectC = 3.0f;
            v.minRuntimeMs = 10UL * 60 * 1000;
            v.minPauseMs = 20UL * 60 * 1000;
            break;
        case Id::Spring:
            v.rhTargetPercent = 60.0f;
            v.hysteresisPercent = 4.0f;
            v.freezeProtectC = 2.5f;
            v.minRuntimeMs = 10UL * 60 * 1000;
            v.minPauseMs = 10UL * 60 * 1000;
            break;
        case Id::Summer:
            v.rhTargetPercent = 72.0f;
            v.hysteresisPercent = 6.0f;
            v.freezeProtectC = 1.0f;
            v.minRuntimeMs = 15UL * 60 * 1000;
            v.minPauseMs = 15UL * 60 * 1000;
            break;
        case Id::Autumn:
        default:
            v.rhTargetPercent = 65.0f;
            v.hysteresisPercent = 5.0f;
            v.freezeProtectC = 2.5f;
            v.minRuntimeMs = 10UL * 60 * 1000;
            v.minPauseMs = 12UL * 60 * 1000;
            break;
    }
    return v;
}

Id current() { return g_current; }

void begin() {
    xTaskCreatePinnedToCore(seasonTask, "seasonTask", 3072, nullptr, 1, nullptr, 0);
}

}  // namespace Season
