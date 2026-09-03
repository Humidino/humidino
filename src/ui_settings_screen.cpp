#include "ui_settings_screen.h"

#include <Arduino.h>
#include <cmath>

#include "fonts/fonts.h"
#include "season.h"
#include "settings_actions.h"
#include "shared_state.h"

namespace {

// Индексы совпадают с порядком создания строк в build() — используется
// только внутри этого файла для чтения/записи спинбоксов при сохранении.
enum RowIndex {
    kRhTarget = 0,
    kHysteresis,
    kFreezeC,
    kMinRuntimeMin,
    kMinPauseMin,
    kRowCount
};

lv_obj_t* g_spinboxes[kRowCount];
lv_obj_t* g_seasonAutoSwitch;
lv_obj_t* g_seasonNowLabel;
lv_obj_t* g_savedFlash;
lv_timer_t* g_flashTimer = nullptr;

const char* seasonRuName(Season::Id season) {
    switch (season) {
        case Season::Id::Winter: return "зима";
        case Season::Id::Spring: return "весна";
        case Season::Id::Summer: return "лето";
        case Season::Id::Autumn: return "осень";
    }
    return "?";
}

// x10 фиксированная точка на десятичных полях (влажность/гистерезис/°C);
// минуты — целые.
constexpr int32_t kDecimalScale = 10;

void hideFlash(lv_timer_t*) {
    lv_obj_add_flag(g_savedFlash, LV_OBJ_FLAG_HIDDEN);
    g_flashTimer = nullptr;
}

void showSavedFlash() {
    lv_obj_clear_flag(g_savedFlash, LV_OBJ_FLAG_HIDDEN);
    if (g_flashTimer != nullptr) lv_timer_reset(g_flashTimer);
    else g_flashTimer = lv_timer_create(hideFlash, 1500, nullptr);
    lv_timer_set_repeat_count(g_flashTimer, 1);
}

void onIncrementClicked(lv_event_t* e) {
    lv_spinbox_increment(static_cast<lv_obj_t*>(lv_event_get_user_data(e)));
}

void onDecrementClicked(lv_event_t* e) {
    lv_spinbox_decrement(static_cast<lv_obj_t*>(lv_event_get_user_data(e)));
}

lv_obj_t* buildStepButtonLabel(lv_obj_t* parent, const char* text) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_size(btn, 40, 34);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(lbl, &font_ru_14, 0);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    return btn;
}

// Строка "подпись | [-] [спинбокс] [+]". Возвращает сам спинбокс — только
// его значение читается/пишется при сохранении/обновлении экрана.
//
// Кнопки "-"/"+" создаются раньше спинбокса (чтобы он лёг между ними по
// порядку в DOM), а обработчики на них вешаются уже после того, как
// спинбокс существует — иначе им не на что было бы указывать.
lv_obj_t* buildRow(lv_obj_t* parent, const char* labelText, uint32_t digitCount, uint32_t sepPos,
                    int32_t rangeMin, int32_t rangeMax, int32_t step) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 40);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 2, 0);

    lv_obj_t* lbl = lv_label_create(row);
    lv_obj_set_style_text_font(lbl, &font_ru_14, 0);
    lv_label_set_text(lbl, labelText);
    lv_obj_set_flex_grow(lbl, 1);

    lv_obj_t* minusBtn = buildStepButtonLabel(row, "-");
    lv_obj_t* plusBtn = buildStepButtonLabel(row, "+");

    // Создан последним -> сейчас после [lbl, minus, plus] (индекс 3).
    // Переносим на индекс 2, чтобы лёг между "-" и "+": [lbl, minus, sb, plus].
    lv_obj_t* sb = lv_spinbox_create(row);
    lv_obj_move_to_index(sb, 2);
    lv_obj_set_width(sb, 84);
    lv_obj_set_style_text_font(sb, &font_ru_14, 0);
    lv_spinbox_set_digit_format(sb, digitCount, sepPos);
    lv_spinbox_set_range(sb, rangeMin, rangeMax);
    lv_spinbox_set_step(sb, step);

    lv_obj_add_event_cb(minusBtn, onDecrementClicked, LV_EVENT_CLICKED, sb);
    lv_obj_add_event_cb(minusBtn, onDecrementClicked, LV_EVENT_LONG_PRESSED_REPEAT, sb);
    lv_obj_add_event_cb(plusBtn, onIncrementClicked, LV_EVENT_CLICKED, sb);
    lv_obj_add_event_cb(plusBtn, onIncrementClicked, LV_EVENT_LONG_PRESSED_REPEAT, sb);

    return sb;
}

void onSaveClicked(lv_event_t*) {
    RuntimeSettings previous = ShaState::getSettings();  // сохраняем текущий mode как есть
    RuntimeSettings settings = previous;
    settings.rhTargetPercent =
        static_cast<float>(lv_spinbox_get_value(g_spinboxes[kRhTarget])) / kDecimalScale;
    settings.hysteresisPercent =
        static_cast<float>(lv_spinbox_get_value(g_spinboxes[kHysteresis])) / kDecimalScale;
    settings.freezeProtectC =
        static_cast<float>(lv_spinbox_get_value(g_spinboxes[kFreezeC])) / kDecimalScale;
    settings.minRuntimeMs =
        static_cast<uint32_t>(lv_spinbox_get_value(g_spinboxes[kMinRuntimeMin])) * 60000UL;
    settings.minPauseMs =
        static_cast<uint32_t>(lv_spinbox_get_value(g_spinboxes[kMinPauseMin])) * 60000UL;
    settings.seasonAutoEnabled = lv_obj_has_state(g_seasonAutoSwitch, LV_STATE_CHECKED);

    // Включили автосезон этим же сохранением — подставляем профиль текущего
    // сезона сразу (см. SettingsActions::withSeasonSyncOnEnable), иначе поля
    // выше и останутся тем, что было введено вручную, пока сезон не сменится.
    SettingsActions::applyRuntimeSettings(SettingsActions::withSeasonSyncOnEnable(previous, settings));
    UiSettingsScreen::refresh();  // если автосезон подставил свои цифры — тут же показать их на спинбоксах
    showSavedFlash();
}

}  // namespace

namespace UiSettingsScreen {

void build(lv_obj_t* parent) {
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x101418), 0);
    lv_obj_set_style_pad_all(parent, 8, 0);
    lv_obj_set_style_pad_row(parent, 8, 0);

    lv_obj_t* title = lv_label_create(parent);
    lv_obj_set_style_text_font(title, &font_ru_20, 0);
    lv_label_set_text(title, "Пороги и тайминги осушения");

    // Влажность 0.0-100.0 %, шаг 0.5 %
    g_spinboxes[kRhTarget] = buildRow(parent, "Целевая влажность подпола, %", 4, 3, 0, 1000, 5);
    // Гистерезис 0.0-50.0 %, шаг 0.5 %
    g_spinboxes[kHysteresis] = buildRow(parent, "Гистерезис, %", 4, 3, 0, 500, 5);
    // Защита от замерзания -20.0..40.0 °C, шаг 0.5 °C
    g_spinboxes[kFreezeC] = buildRow(parent, "Защита от замерзания, °C", 4, 3, -200, 400, 5);
    // Мин. время работы/паузы, целые минуты 0-180
    g_spinboxes[kMinRuntimeMin] = buildRow(parent, "Мин. время работы, мин", 3, 0, 0, 180, 1);
    g_spinboxes[kMinPauseMin] = buildRow(parent, "Мин. пауза, мин", 3, 0, 0, 180, 1);

    // --- Автосезон: подставляет пороги/тайминги выше сама, по календарю ---
    // (профили под климат Лотошино, МО — см. docs/SEASONAL_LOTOSHINO.md).
    // Значения полей выше при включённом автосезоне носят временный
    // характер — их перезапишет ближайшая смена сезона, если не выключить.
    lv_obj_t* seasonRow = lv_obj_create(parent);
    lv_obj_set_size(seasonRow, LV_PCT(100), 40);
    lv_obj_set_flex_flow(seasonRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(seasonRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(seasonRow, 0, 0);
    lv_obj_set_style_pad_all(seasonRow, 2, 0);

    lv_obj_t* seasonLbl = lv_label_create(seasonRow);
    lv_obj_set_style_text_font(seasonLbl, &font_ru_14, 0);
    lv_label_set_text(seasonLbl, "Автосезон (Лотошино, МО)");
    lv_obj_set_flex_grow(seasonLbl, 1);

    g_seasonNowLabel = lv_label_create(seasonRow);
    lv_obj_set_style_text_font(g_seasonNowLabel, &font_ru_14, 0);
    lv_obj_set_style_text_color(g_seasonNowLabel, lv_color_hex(0x8AA0B8), 0);
    lv_label_set_text(g_seasonNowLabel, "");

    g_seasonAutoSwitch = lv_switch_create(seasonRow);

    lv_obj_t* footer = lv_obj_create(parent);
    lv_obj_set_size(footer, LV_PCT(100), 44);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(footer, 12, 0);
    lv_obj_set_style_border_width(footer, 0, 0);

    lv_obj_t* saveBtn = lv_button_create(footer);
    lv_obj_add_event_cb(saveBtn, onSaveClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* saveLbl = lv_label_create(saveBtn);
    lv_obj_set_style_text_font(saveLbl, &font_ru_14, 0);
    lv_label_set_text(saveLbl, "Сохранить");
    lv_obj_center(saveLbl);

    g_savedFlash = lv_label_create(footer);
    lv_obj_set_style_text_font(g_savedFlash, &font_ru_14, 0);
    lv_obj_set_style_text_color(g_savedFlash, lv_color_hex(0x4CAF50), 0);
    lv_label_set_text(g_savedFlash, "\xE2\x9C\x93 сохранено");  // "✓ сохранено"
    lv_obj_add_flag(g_savedFlash, LV_OBJ_FLAG_HIDDEN);

    refresh();
}

void refresh() {
    RuntimeSettings settings = ShaState::getSettings();
    lv_spinbox_set_value(g_spinboxes[kRhTarget],
                          static_cast<int32_t>(lroundf(settings.rhTargetPercent * kDecimalScale)));
    lv_spinbox_set_value(g_spinboxes[kHysteresis],
                          static_cast<int32_t>(lroundf(settings.hysteresisPercent * kDecimalScale)));
    lv_spinbox_set_value(g_spinboxes[kFreezeC],
                          static_cast<int32_t>(lroundf(settings.freezeProtectC * kDecimalScale)));
    lv_spinbox_set_value(g_spinboxes[kMinRuntimeMin],
                          static_cast<int32_t>(settings.minRuntimeMs / 60000UL));
    lv_spinbox_set_value(g_spinboxes[kMinPauseMin],
                          static_cast<int32_t>(settings.minPauseMs / 60000UL));

    if (settings.seasonAutoEnabled) lv_obj_add_state(g_seasonAutoSwitch, LV_STATE_CHECKED);
    else lv_obj_remove_state(g_seasonAutoSwitch, LV_STATE_CHECKED);

    char seasonBuf[24];
    snprintf(seasonBuf, sizeof(seasonBuf), "сейчас: %s", seasonRuName(Season::current()));
    lv_label_set_text(g_seasonNowLabel, seasonBuf);
}

}  // namespace UiSettingsScreen
