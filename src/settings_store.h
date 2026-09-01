#pragma once
#include <cstdint>
#include <vector>

#include "shared_state.h"

// Сохраняет редактируемую пользователем конфигурацию в NVS через
// Preferences.h. Небольшой плоский набор скаляров — NVS даёт атомарную
// запись по ключу без риска повредить всё при потере питания на середине
// записи (в отличие от одного JSON-файла на LittleFS).
namespace Settings {

void begin();

RuntimeSettings load();
void save(const RuntimeSettings& settings);

// Именованный набор порогов/таймингов, который пользователь может сохранить
// и применить одной кнопкой из веб-интерфейса. Режим (Auto/ManualOn/Off) в
// пресет намеренно не входит — это ортогональный выбор.
struct PresetValues {
    float rhTargetPercent = DEFAULT_RH_TARGET_PERCENT;
    float hysteresisPercent = DEFAULT_HYSTERESIS_PERCENT;
    float freezeProtectC = FREEZE_PROTECT_TEMP_C;
    uint32_t minRuntimeMs = MIN_RUNTIME_MS;
    uint32_t minPauseMs = MIN_PAUSE_MS;
};

struct Preset {
    char name[24] = "";
    PresetValues values;
};

constexpr size_t kMaxPresets = 8;

std::vector<Preset> loadPresets();
// Полностью заменяет сохранённый список (простая модель CRUD — клиент
// присылает весь актуальный список при любом добавлении/удалении/переименовании).
// Список обрезается до kMaxPresets записей.
void savePresets(const std::vector<Preset>& presets);

}  // namespace Settings
