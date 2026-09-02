#pragma once
#include "settings_store.h"
#include "shared_state.h"

// Общая логика применения настроек — раньше жила только внутри обработчиков
// web_server.cpp; вынесена сюда, чтобы touch-интерфейс на дисплее
// (ui_settings_screen.cpp, ui_presets_screen.cpp) и локальный веб-интерфейс
// делали ровно одно и то же (обновление SharedState + запись в NVS), а не
// дублировали и не расходились в поведении.
namespace SettingsActions {

// Публикует settings в SharedState (немедленно влияет на controlTask) и
// сохраняет в NVS.
void applyRuntimeSettings(const RuntimeSettings& settings);

// Ищет пресет по имени, при находке применяет его пороги/тайминги (режим не
// трогает — он ортогонален пресетам) через applyRuntimeSettings() и
// возвращает итоговые настройки в outApplied. false, если пресет с таким
// именем не найден.
bool applyPresetByName(const char* name, RuntimeSettings& outApplied);

}  // namespace SettingsActions
