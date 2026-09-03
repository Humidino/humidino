#include "ui_root.h"

#include <cstdint>
#include <lvgl.h>

#include "ui_dashboard.h"
#include "ui_keyboard.h"
#include "ui_presets_screen.h"
#include "ui_settings_screen.h"
#include "ui_stats_screen.h"

namespace {

enum TabIndex { kTabMain = 0, kTabSettings, kTabPresets, kTabStats, kTabCount };

lv_obj_t* g_tabs[kTabCount];
lv_obj_t* g_navButtons[kTabCount];
int g_activeTab = -1;

// Настройки/Пресеты — это формы редактирования, а не живая телеметрия:
// их поля перечитываются из NVS/SharedState только при переключении на
// вкладку (здесь), а не на каждый тик UiRoot::update() — иначе можно
// затереть то, что пользователь ещё не успел сохранить, посреди набора
// текста или числа.
void showTab(int idx) {
    if (idx == g_activeTab) return;
    g_activeTab = idx;

    for (int i = 0; i < kTabCount; i++) {
        if (i == idx) {
            lv_obj_clear_flag(g_tabs[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(g_navButtons[i], lv_color_hex(0x2E6DA4), 0);
        } else {
            lv_obj_add_flag(g_tabs[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(g_navButtons[i], lv_color_hex(0x2A2F36), 0);
        }
    }

    switch (idx) {
        case kTabSettings:
            UiSettingsScreen::refresh();
            break;
        case kTabPresets:
            UiPresetsScreen::refresh();
            break;
        case kTabStats:
            UiStatsScreen::refresh();
            break;
        default:
            break;
    }
}

void onNavClicked(lv_event_t* e) {
    int idx = static_cast<int>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    showTab(idx);
}

lv_obj_t* buildNavButton(lv_obj_t* parent, const char* text, int idx) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_height(btn, LV_PCT(100));
    lv_obj_add_event_cb(btn, onNavClicked, LV_EVENT_CLICKED,
                         reinterpret_cast<void*>(static_cast<uintptr_t>(idx)));

    lv_obj_t* lbl = lv_label_create(btn);
    // Подписи навигации — латиницей и на дефолтном шрифте LVGL (не задаём
    // свой font_ru_*): дефолтный шрифт кириллицу не содержит (см.
    // include/lv_conf.h: LV_FONT_DEFAULT и комментарий в fonts/fonts.h),
    // а весь остальной интерфейс кириллический только там, где мы явно
    // подключаем свой шрифт на каждом виджете.
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    return btn;
}

}  // namespace

namespace UiRoot {

void build() {
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_pad_row(scr, 0, 0);

    // Контейнер контента — все kTabCount экранов лежат в нём рядом, виден
    // только тот, что выбран сейчас (см. showTab()).
    lv_obj_t* content = lv_obj_create(scr);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);

    for (int i = 0; i < kTabCount; i++) {
        g_tabs[i] = lv_obj_create(content);
        lv_obj_set_size(g_tabs[i], LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_border_width(g_tabs[i], 0, 0);
        lv_obj_set_style_radius(g_tabs[i], 0, 0);
    }

    UiDashboard::build(g_tabs[kTabMain]);
    UiSettingsScreen::build(g_tabs[kTabSettings]);
    UiPresetsScreen::build(g_tabs[kTabPresets]);
    UiStatsScreen::build(g_tabs[kTabStats]);

    // Нижняя панель навигации.
    lv_obj_t* navBar = lv_obj_create(scr);
    lv_obj_set_size(navBar, LV_PCT(100), 40);
    lv_obj_set_flex_flow(navBar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(navBar, 2, 0);
    lv_obj_set_style_pad_all(navBar, 2, 0);
    lv_obj_set_style_border_width(navBar, 0, 0);
    lv_obj_set_style_radius(navBar, 0, 0);

    g_navButtons[kTabMain] = buildNavButton(navBar, "Main", kTabMain);
    g_navButtons[kTabSettings] = buildNavButton(navBar, "Settings", kTabSettings);
    g_navButtons[kTabPresets] = buildNavButton(navBar, "Presets", kTabPresets);
    g_navButtons[kTabStats] = buildNavButton(navBar, "Stats", kTabStats);

    // Клавиатура — оверлей поверх всего экрана (создана на самом scr, а не
    // внутри конкретной вкладки), чтобы всплывать над полями ввода на
    // "Пресетах" одной и той же клавиатурой.
    UiKeyboard::init(scr);

    showTab(kTabMain);
}

void update() {
    UiDashboard::update();  // единственный экран с живой телеметрией — обновляется всегда
}

}  // namespace UiRoot
