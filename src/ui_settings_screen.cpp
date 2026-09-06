#include "ui_settings_screen.h"

#include <Arduino.h>
#include <cmath>
#include <cstdlib>

#include "fonts/fonts.h"
#include "season.h"
#include "settings_actions.h"
#include "shared_state.h"

namespace {

// Индексы совпадают с порядком создания строк в build() — используется
// только внутри этого файла для чтения/записи значений при сохранении.
enum RowIndex {
    kRhTarget = 0,
    kHysteresis,
    kFreezeC,
    kMinRuntimeMin,
    kMinPauseMin,
    kRowCount
};

// x10 фиксированная точка только у температуры (там нужен шаг 0.5 °C из-за
// сезонных профилей, см. season.cpp); влажность/гистерезис/минуты — целые.
constexpr int32_t kDecimalScale = 10;

// Ручной счётчик вместо lv_spinbox: у спинбокса фиксированная ширина в
// цифрах и он всегда дополняет число ведущими нулями (007.5, 005 и т.п.),
// что нечитаемо для обычного пользователя — здесь просто печатаем значение.
struct CounterRow {
    lv_obj_t* valueLabel = nullptr;
    int32_t value = 0;
    int32_t rangeMin = 0;
    int32_t rangeMax = 0;
    int32_t step = 1;
    bool oneDecimal = false;  // true только для kFreezeC (x10 фиксированная точка)
};

CounterRow g_rows[kRowCount];
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

void updateRowLabel(RowIndex idx) {
    CounterRow& row = g_rows[idx];
    char buf[16];
    if (row.oneDecimal) {
        bool neg = row.value < 0;
        int32_t absVal = std::abs(row.value);
        snprintf(buf, sizeof(buf), "%s%d.%d", neg ? "-" : "", absVal / 10, absVal % 10);
    } else {
        snprintf(buf, sizeof(buf), "%d", static_cast<int>(row.value));
    }
    lv_label_set_text(row.valueLabel, buf);
}

void onIncrementClicked(lv_event_t* e) {
    RowIndex idx = static_cast<RowIndex>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    CounterRow& row = g_rows[idx];
    row.value = LV_MIN(row.value + row.step, row.rangeMax);
    updateRowLabel(idx);
}

void onDecrementClicked(lv_event_t* e) {
    RowIndex idx = static_cast<RowIndex>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    CounterRow& row = g_rows[idx];
    row.value = LV_MAX(row.value - row.step, row.rangeMin);
    updateRowLabel(idx);
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

// Строка "подпись | [-] [значение] [+]". Значение и его границы хранятся в
// g_rows[idx] — читаются/пишутся при сохранении и обновлении экрана.
void buildRow(lv_obj_t* parent, RowIndex idx, const char* labelText, int32_t rangeMin,
              int32_t rangeMax, int32_t step, bool oneDecimal) {
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
    // Переносим на индекс 2, чтобы лёг между "-" и "+": [lbl, minus, box, plus].
    lv_obj_t* valueBox = lv_obj_create(row);
    lv_obj_move_to_index(valueBox, 2);
    lv_obj_set_size(valueBox, 84, 34);
    lv_obj_set_style_bg_color(valueBox, lv_color_hex(0x1C232B), 0);
    lv_obj_set_style_border_color(valueBox, lv_color_hex(0x33404D), 0);
    lv_obj_set_style_border_width(valueBox, 1, 0);
    lv_obj_set_style_radius(valueBox, 4, 0);
    lv_obj_set_style_pad_all(valueBox, 0, 0);
    lv_obj_clear_flag(valueBox, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* valueLbl = lv_label_create(valueBox);
    lv_obj_set_style_text_font(valueLbl, &font_ru_14, 0);
    lv_obj_center(valueLbl);

    g_rows[idx].valueLabel = valueLbl;
    g_rows[idx].rangeMin = rangeMin;
    g_rows[idx].rangeMax = rangeMax;
    g_rows[idx].step = step;
    g_rows[idx].oneDecimal = oneDecimal;

    void* userData = reinterpret_cast<void*>(static_cast<intptr_t>(idx));
    lv_obj_add_event_cb(minusBtn, onDecrementClicked, LV_EVENT_SHORT_CLICKED, userData);
    lv_obj_add_event_cb(minusBtn, onDecrementClicked, LV_EVENT_LONG_PRESSED_REPEAT, userData);
    lv_obj_add_event_cb(plusBtn, onIncrementClicked, LV_EVENT_SHORT_CLICKED, userData);
    lv_obj_add_event_cb(plusBtn, onIncrementClicked, LV_EVENT_LONG_PRESSED_REPEAT, userData);
}

void onSaveClicked(lv_event_t*) {
    RuntimeSettings previous = ShaState::getSettings();  // сохраняем текущий mode как есть
    RuntimeSettings settings = previous;
    settings.rhTargetPercent = static_cast<float>(g_rows[kRhTarget].value);
    settings.hysteresisPercent = static_cast<float>(g_rows[kHysteresis].value);
    settings.freezeProtectC = static_cast<float>(g_rows[kFreezeC].value) / kDecimalScale;
    settings.minRuntimeMs = static_cast<uint32_t>(g_rows[kMinRuntimeMin].value) * 60000UL;
    settings.minPauseMs = static_cast<uint32_t>(g_rows[kMinPauseMin].value) * 60000UL;
    settings.seasonAutoEnabled = lv_obj_has_state(g_seasonAutoSwitch, LV_STATE_CHECKED);

    // Включили автосезон этим же сохранением — подставляем профиль текущего
    // сезона сразу (см. SettingsActions::withSeasonSyncOnEnable), иначе поля
    // выше и останутся тем, что было введено вручную, пока сезон не сменится.
    SettingsActions::applyRuntimeSettings(SettingsActions::withSeasonSyncOnEnable(previous, settings));
    UiSettingsScreen::refresh();  // если автосезон подставил свои цифры — тут же показать их
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
    lv_obj_set_style_text_color(title, lv_color_hex(0xF0F0F0), 0);
    lv_obj_set_width(title, LV_PCT(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_label_set_text(title, "Пороги и тайминги осушения");

    // Влажность 0-100 %, шаг 1 %
    buildRow(parent, kRhTarget, "Целевая влажность подпола, %", 0, 100, 1, false);
    // Гистерезис 0-50 %, шаг 1 %
    buildRow(parent, kHysteresis, "Гистерезис, %", 0, 50, 1, false);
    // Защита от замерзания -20.0..40.0 °C, шаг 0.5 °C (сезонные профили
    // используют половинки градуса — см. season.cpp)
    buildRow(parent, kFreezeC, "Защита от замерзания, °C", -200, 400, 5, true);
    // Мин. время работы/паузы, целые минуты 0-180
    buildRow(parent, kMinRuntimeMin, "Мин. время работы, мин", 0, 180, 1, false);
    buildRow(parent, kMinPauseMin, "Мин. пауза, мин", 0, 180, 1, false);

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
    g_rows[kRhTarget].value = static_cast<int32_t>(lroundf(settings.rhTargetPercent));
    g_rows[kHysteresis].value = static_cast<int32_t>(lroundf(settings.hysteresisPercent));
    g_rows[kFreezeC].value = static_cast<int32_t>(lroundf(settings.freezeProtectC * kDecimalScale));
    g_rows[kMinRuntimeMin].value = static_cast<int32_t>(settings.minRuntimeMs / 60000UL);
    g_rows[kMinPauseMin].value = static_cast<int32_t>(settings.minPauseMs / 60000UL);
    for (int i = 0; i < kRowCount; ++i) updateRowLabel(static_cast<RowIndex>(i));

    if (settings.seasonAutoEnabled) lv_obj_add_state(g_seasonAutoSwitch, LV_STATE_CHECKED);
    else lv_obj_remove_state(g_seasonAutoSwitch, LV_STATE_CHECKED);

    char seasonBuf[24];
    snprintf(seasonBuf, sizeof(seasonBuf), "сейчас: %s", seasonRuName(Season::current()));
    lv_label_set_text(g_seasonNowLabel, seasonBuf);
}

}  // namespace UiSettingsScreen
