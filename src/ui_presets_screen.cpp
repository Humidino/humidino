#include "ui_presets_screen.h"

#include <Arduino.h>
#include <cstdint>
#include <cstring>

#include "fonts/fonts.h"
#include "settings_actions.h"
#include "settings_store.h"
#include "shared_state.h"
#include "ui_keyboard.h"

namespace {

lv_obj_t* g_listView;
lv_obj_t* g_rows;         // прокручиваемый контейнер строк-пресетов внутри g_listView
lv_obj_t* g_emptyLabel;   // "Пока нет сохранённых пресетов"
lv_obj_t* g_nameEntryView;
lv_obj_t* g_nameTextarea;
lv_obj_t* g_nameError;

// Локальный кэш списка пресетов на время жизни экрана — источник истины
// для строк списка и индексов в user_data кнопок "Применить"/"Удалить".
// Перечитывается из NVS только в refresh().
std::vector<Settings::Preset> g_cachedPresets;

void showListView() {
    lv_obj_clear_flag(g_listView, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_nameEntryView, LV_OBJ_FLAG_HIDDEN);
}

void showNameEntryView() {
    lv_textarea_set_text(g_nameTextarea, "");
    lv_label_set_text(g_nameError, "");
    lv_obj_add_flag(g_listView, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_nameEntryView, LV_OBJ_FLAG_HIDDEN);
}

void onApplyClicked(lv_event_t* e) {
    size_t idx = static_cast<size_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    if (idx >= g_cachedPresets.size()) return;
    RuntimeSettings applied;
    SettingsActions::applyPresetByName(g_cachedPresets[idx].name, applied);
}

void rebuildRows();  // fwd

// Первое нажатие меняет подпись на "Точно?" и ждёт повторного нажатия —
// без этого случайное касание безвозвратно стирает пресет (в вебе к этому
// действию нет отдельного подтверждения, но там сложнее промахнуться
// мышью, чем пальцем по маленькому экрану).
void onDeleteClicked(lv_event_t* e) {
    lv_obj_t* btn = lv_event_get_target_obj(e);
    size_t idx = static_cast<size_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    if (idx >= g_cachedPresets.size()) return;

    // Состояние "armed" храним прямо в тексте кнопки (а не в отдельном
    // флаге/переменной) — второе нажатие видит на кнопке "Точно?" и удаляет,
    // первое нажатие видит "Удалить" и только переспрашивает.
    lv_obj_t* lbl = lv_obj_get_child(btn, 0);
    if (strcmp(lv_label_get_text(lbl), "Точно?") != 0) {
        lv_label_set_text(lbl, "Точно?");
        return;
    }

    g_cachedPresets.erase(g_cachedPresets.begin() + static_cast<long>(idx));
    Settings::savePresets(g_cachedPresets);
    rebuildRows();
}

void buildRow(size_t index) {
    const Settings::Preset& preset = g_cachedPresets[index];
    void* userData = reinterpret_cast<void*>(static_cast<uintptr_t>(index));

    lv_obj_t* row = lv_obj_create(g_rows);
    lv_obj_set_size(row, LV_PCT(100), 40);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row, 4, 0);

    lv_obj_t* nameLbl = lv_label_create(row);
    lv_obj_set_style_text_font(nameLbl, &font_ru_14, 0);
    lv_label_set_text(nameLbl, preset.name);
    lv_obj_set_flex_grow(nameLbl, 1);

    lv_obj_t* applyBtn = lv_button_create(row);
    lv_obj_t* applyLbl = lv_label_create(applyBtn);
    lv_obj_set_style_text_font(applyLbl, &font_ru_14, 0);
    lv_label_set_text(applyLbl, "Применить");
    lv_obj_center(applyLbl);
    lv_obj_add_event_cb(applyBtn, onApplyClicked, LV_EVENT_CLICKED, userData);

    lv_obj_t* deleteBtn = lv_button_create(row);
    lv_obj_t* deleteLbl = lv_label_create(deleteBtn);
    lv_obj_set_style_text_font(deleteLbl, &font_ru_14, 0);
    lv_label_set_text(deleteLbl, "Удалить");
    lv_obj_center(deleteLbl);
    lv_obj_add_event_cb(deleteBtn, onDeleteClicked, LV_EVENT_CLICKED, userData);
}

void rebuildRows() {
    lv_obj_clean(g_rows);
    if (g_cachedPresets.empty()) {
        lv_obj_clear_flag(g_emptyLabel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_emptyLabel, LV_OBJ_FLAG_HIDDEN);
    }
    for (size_t i = 0; i < g_cachedPresets.size(); i++) buildRow(i);
}

void onAddClicked(lv_event_t*) { showNameEntryView(); }

void onNameCancelClicked(lv_event_t*) {
    UiKeyboard::hide();
    showListView();
}

void onNameSaveClicked(lv_event_t*) {
    const char* name = lv_textarea_get_text(g_nameTextarea);
    if (name == nullptr || strlen(name) == 0) {
        lv_label_set_text(g_nameError, "Введите имя пресета");
        return;
    }

    RuntimeSettings current = ShaState::getSettings();
    Settings::PresetValues values;
    values.rhTargetPercent = current.rhTargetPercent;
    values.hysteresisPercent = current.hysteresisPercent;
    values.freezeProtectC = current.freezeProtectC;
    values.minRuntimeMs = current.minRuntimeMs;
    values.minPauseMs = current.minPauseMs;

    bool replaced = false;
    for (auto& p : g_cachedPresets) {
        if (strcmp(p.name, name) == 0) {
            p.values = values;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        if (g_cachedPresets.size() >= Settings::kMaxPresets) {
            lv_label_set_text(g_nameError, "Достигнут лимит пресетов (8)");
            return;
        }
        Settings::Preset preset;
        strncpy(preset.name, name, sizeof(preset.name) - 1);
        preset.values = values;
        g_cachedPresets.push_back(preset);
    }

    Settings::savePresets(g_cachedPresets);
    UiKeyboard::hide();
    rebuildRows();
    showListView();
}

}  // namespace

namespace UiPresetsScreen {

void build(lv_obj_t* parent) {
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x101418), 0);
    lv_obj_set_style_pad_all(parent, 8, 0);

    // --- Вид "список пресетов" ---
    g_listView = lv_obj_create(parent);
    lv_obj_set_size(g_listView, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(g_listView, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_border_width(g_listView, 0, 0);
    lv_obj_set_style_pad_all(g_listView, 0, 0);
    lv_obj_set_style_pad_row(g_listView, 6, 0);
    lv_obj_set_style_bg_opa(g_listView, LV_OPA_TRANSP, 0);

    lv_obj_t* title = lv_label_create(g_listView);
    lv_obj_set_style_text_font(title, &font_ru_20, 0);
    lv_label_set_text(title, "Пресеты");

    g_emptyLabel = lv_label_create(g_listView);
    lv_obj_set_style_text_font(g_emptyLabel, &font_ru_14, 0);
    lv_label_set_text(g_emptyLabel, "Пока нет сохранённых пресетов");

    g_rows = lv_obj_create(g_listView);
    lv_obj_set_width(g_rows, LV_PCT(100));
    lv_obj_set_flex_grow(g_rows, 1);
    lv_obj_set_flex_flow(g_rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_rows, 4, 0);
    lv_obj_set_style_border_width(g_rows, 0, 0);
    lv_obj_set_style_bg_opa(g_rows, LV_OPA_TRANSP, 0);

    lv_obj_t* addBtn = lv_button_create(g_listView);
    lv_obj_set_width(addBtn, LV_PCT(100));
    lv_obj_add_event_cb(addBtn, onAddClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* addLbl = lv_label_create(addBtn);
    lv_obj_set_style_text_font(addLbl, &font_ru_14, 0);
    lv_label_set_text(addLbl, "Сохранить текущие настройки как пресет...");
    lv_obj_center(addLbl);

    // --- Вид "ввод имени нового пресета" ---
    g_nameEntryView = lv_obj_create(parent);
    lv_obj_set_size(g_nameEntryView, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(g_nameEntryView, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_border_width(g_nameEntryView, 0, 0);
    lv_obj_set_style_pad_all(g_nameEntryView, 0, 0);
    lv_obj_set_style_pad_row(g_nameEntryView, 8, 0);
    lv_obj_set_style_bg_opa(g_nameEntryView, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(g_nameEntryView, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* nameTitle = lv_label_create(g_nameEntryView);
    lv_obj_set_style_text_font(nameTitle, &font_ru_20, 0);
    lv_label_set_text(nameTitle, "Имя пресета");

    g_nameTextarea = lv_textarea_create(g_nameEntryView);
    lv_obj_set_width(g_nameTextarea, LV_PCT(100));
    lv_obj_set_style_text_font(g_nameTextarea, &font_ru_14, 0);
    lv_textarea_set_one_line(g_nameTextarea, true);
    lv_textarea_set_max_length(g_nameTextarea, sizeof(Settings::Preset::name) - 1);
    // Экранная клавиатура (lv_keyboard) — латинская раскладка без кириллицы,
    // поэтому пример в плейсхолдере тоже латиницей, чтобы не обещать лишнего;
    // кириллические имена пресетов, заданные через веб-интерфейс, здесь всё
    // равно корректно отображаются (font_ru_14 её поддерживает).
    lv_textarea_set_placeholder_text(g_nameTextarea, "Например: Leto");
    UiKeyboard::bindFocus(g_nameTextarea);

    g_nameError = lv_label_create(g_nameEntryView);
    lv_obj_set_style_text_font(g_nameError, &font_ru_14, 0);
    lv_obj_set_style_text_color(g_nameError, lv_color_hex(0xE04040), 0);
    lv_label_set_text(g_nameError, "");

    lv_obj_t* nameFooter = lv_obj_create(g_nameEntryView);
    lv_obj_set_size(nameFooter, LV_PCT(100), 44);
    lv_obj_set_flex_flow(nameFooter, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(nameFooter, 12, 0);
    lv_obj_set_style_border_width(nameFooter, 0, 0);

    lv_obj_t* saveBtn = lv_button_create(nameFooter);
    lv_obj_add_event_cb(saveBtn, onNameSaveClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* saveLbl = lv_label_create(saveBtn);
    lv_obj_set_style_text_font(saveLbl, &font_ru_14, 0);
    lv_label_set_text(saveLbl, "Сохранить");
    lv_obj_center(saveLbl);

    lv_obj_t* cancelBtn = lv_button_create(nameFooter);
    lv_obj_add_event_cb(cancelBtn, onNameCancelClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* cancelLbl = lv_label_create(cancelBtn);
    lv_obj_set_style_text_font(cancelLbl, &font_ru_14, 0);
    lv_label_set_text(cancelLbl, "Отмена");
    lv_obj_center(cancelLbl);

    refresh();
}

void refresh() {
    g_cachedPresets = Settings::loadPresets();
    rebuildRows();
    showListView();
}

}  // namespace UiPresetsScreen
