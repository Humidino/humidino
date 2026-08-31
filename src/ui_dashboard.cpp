#include "ui_dashboard.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

#include "config.h"
#include "fonts/fonts.h"
#include "relay.h"
#include "settings_actions.h"
#include "shared_state.h"

namespace {

struct ZonePanelWidgets {
    lv_obj_t* temp;
    lv_obj_t* rh;
    lv_obj_t* dew;      // nullptr для зон без точки росы (Улица/Дом)
    lv_obj_t* errBadge;
};

lv_obj_t* g_uptimeLabel;
lv_obj_t* g_wifiLabel;
lv_obj_t* g_ramLabel;
lv_obj_t* g_modeLabel;
lv_obj_t* g_banner;
lv_obj_t* g_bannerLabel;
lv_obj_t* g_spinner;  // видна и крутится только пока реле включено — см. update()
lv_obj_t* g_modeButtons[3];  // Авто/Вкл/Выкл, индекс соответствует OperatingMode
ZonePanelWidgets g_panels[static_cast<size_t>(SensorId::Count)];

// Меняет только режим, остальные пороги/тайминги оставляет как есть —
// то же самое, что делает POST /api/settings с одним полем "mode" из
// веб-интерфейса (см. web_server.cpp).
void setMode(OperatingMode mode) {
    RuntimeSettings settings = ShaState::getSettings();
    if (settings.mode == mode) return;
    settings.mode = mode;
    SettingsActions::applyRuntimeSettings(settings);
}

void onModeAutoClicked(lv_event_t*) { setMode(OperatingMode::Auto); }
void onModeOnClicked(lv_event_t*) { setMode(OperatingMode::ManualOn); }
void onModeOffClicked(lv_event_t*) { setMode(OperatingMode::ManualOff); }

lv_obj_t* buildModeButton(lv_obj_t* parent, const char* text, lv_event_cb_t cb) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* label = lv_label_create(btn);
    lv_obj_set_style_text_font(label, &font_ru_14, 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);

    return btn;
}

const char* kZoneTitles[] = {
    "Приточка подпола",
    "Дальний угол подпола",
    "Улица",
    "Дом",
};

bool zoneHasDewPoint(SensorId id) {
    return id == SensorId::CrawlspaceIntake || id == SensorId::CrawlspaceCorner;
}

ZonePanelWidgets buildZonePanel(lv_obj_t* parent, const char* title, bool showDewPoint) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_style_pad_all(panel, 6, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_border_width(panel, 1, 0);

    lv_obj_t* titleLabel = lv_label_create(panel);
    lv_label_set_text(titleLabel, title);
    lv_obj_set_style_text_font(titleLabel, &font_ru_14, 0);

    ZonePanelWidgets w{};

    w.temp = lv_label_create(panel);
    lv_obj_set_style_text_font(w.temp, &font_ru_20, 0);
    lv_label_set_text(w.temp, "--.- °C");

    w.rh = lv_label_create(panel);
    lv_obj_set_style_text_font(w.rh, &font_ru_20, 0);
    lv_label_set_text(w.rh, "--.- %");

    if (showDewPoint) {
        w.dew = lv_label_create(panel);
        lv_obj_set_style_text_font(w.dew, &font_ru_14, 0);
        lv_label_set_text(w.dew, "Точка росы: --.- °C");
    } else {
        w.dew = nullptr;
    }

    w.errBadge = lv_label_create(panel);
    lv_obj_set_style_text_font(w.errBadge, &font_ru_14, 0);
    lv_obj_set_style_text_color(w.errBadge, lv_color_hex(0xE04040), 0);
    lv_label_set_text(w.errBadge, "");

    return w;
}

void formatUptime(char* out, size_t outSize, uint32_t ms) {
    uint32_t totalSec = ms / 1000;
    uint32_t days = totalSec / 86400;
    uint32_t hours = (totalSec % 86400) / 3600;
    uint32_t mins = (totalSec % 3600) / 60;
    snprintf(out, outSize, "Время работы: %luд %02lu:%02lu", (unsigned long)days, (unsigned long)hours,
              (unsigned long)mins);
}

void updateZonePanel(const ZonePanelWidgets& w, const SensorReading& r) {
    char buf[32];

    if (r.valid) {
        snprintf(buf, sizeof(buf), "%.1f °C", r.temperatureC);
        lv_label_set_text(w.temp, buf);

        snprintf(buf, sizeof(buf), "%.1f %%", r.humidityPct);
        lv_label_set_text(w.rh, buf);

        if (w.dew != nullptr) {
            snprintf(buf, sizeof(buf), "Точка росы: %.1f °C", r.dewPointC);
            lv_label_set_text(w.dew, buf);
        }
    } else {
        lv_label_set_text(w.temp, "--.- °C");
        lv_label_set_text(w.rh, "--.- %");
        if (w.dew != nullptr) lv_label_set_text(w.dew, "Точка росы: --.- °C");
    }

    lv_label_set_text(w.errBadge, r.error ? "ERR" : "");
}

lv_color_t bannerColorFor(RelayControlState state) {
    switch (state) {
        case RelayControlState::Running:
            return lv_color_hex(0x2E8B45);  // зелёный
        case RelayControlState::LockedOutCondensation:
        case RelayControlState::LockedOutFreeze:
        case RelayControlState::LockedOutSensorFault:
            return lv_color_hex(0xC97A1E);  // оранжевый
        case RelayControlState::Idle:
        case RelayControlState::MinPauseHold:
        default:
            return lv_color_hex(0x3A4A5A);  // серо-синий
    }
}

}  // namespace

namespace UiDashboard {

void build(lv_obj_t* parent) {
    lv_obj_t* scr = parent;
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
    lv_obj_set_style_pad_all(scr, 4, 0);
    lv_obj_set_style_pad_row(scr, 4, 0);

    // --- Строка статуса ---
    lv_obj_t* statusBar = lv_obj_create(scr);
    lv_obj_set_size(statusBar, LV_PCT(100), 28);
    lv_obj_set_flex_flow(statusBar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(statusBar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    g_uptimeLabel = lv_label_create(statusBar);
    lv_obj_set_style_text_font(g_uptimeLabel, &font_ru_14, 0);
    lv_label_set_text(g_uptimeLabel, "Время работы: --");

    g_wifiLabel = lv_label_create(statusBar);
    lv_obj_set_style_text_font(g_wifiLabel, &font_ru_14, 0);
    lv_label_set_text(g_wifiLabel, "WiFi: --");

    g_ramLabel = lv_label_create(statusBar);
    lv_obj_set_style_text_font(g_ramLabel, &font_ru_14, 0);
    lv_label_set_text(g_ramLabel, "ОЗУ: --");

    g_modeLabel = lv_label_create(statusBar);
    lv_obj_set_style_text_font(g_modeLabel, &font_ru_14, 0);
    lv_obj_set_style_text_color(g_modeLabel, lv_color_hex(0x8AA0B8), 0);
    lv_label_set_text(g_modeLabel, "АВТО");

    // --- Кнопки переключения режима (дублируют веб-интерфейс) ---
    lv_obj_t* modeRow = lv_obj_create(scr);
    lv_obj_set_size(modeRow, LV_PCT(100), 34);
    lv_obj_set_flex_flow(modeRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(modeRow, 4, 0);
    lv_obj_set_style_pad_all(modeRow, 0, 0);
    lv_obj_set_style_border_width(modeRow, 0, 0);

    g_modeButtons[static_cast<size_t>(OperatingMode::Auto)] =
        buildModeButton(modeRow, "АВТО", onModeAutoClicked);
    g_modeButtons[static_cast<size_t>(OperatingMode::ManualOn)] =
        buildModeButton(modeRow, "ВКЛ", onModeOnClicked);
    g_modeButtons[static_cast<size_t>(OperatingMode::ManualOff)] =
        buildModeButton(modeRow, "ВЫКЛ", onModeOffClicked);

    // --- Сетка зон 2x2 ---
    static int32_t colDsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static int32_t rowDsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};

    lv_obj_t* grid = lv_obj_create(scr);
    lv_obj_set_width(grid, LV_PCT(100));
    // flex-grow вместо фиксированного процента высоты: строка режима теперь
    // делит экран вместе с баннером и статус-баром, а доступная высота
    // родителя зависит от того, в каком контейнере лежит дашборд (вкладка
    // tabview или весь экран) — пусть сетка просто займёт всё, что осталось.
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_grid_dsc_array(grid, colDsc, rowDsc);
    lv_obj_set_style_pad_all(grid, 2, 0);
    lv_obj_set_style_pad_gap(grid, 4, 0);

    for (size_t i = 0; i < static_cast<size_t>(SensorId::Count); i++) {
        SensorId id = static_cast<SensorId>(i);
        int col = i % 2;
        int row = i / 2;
        lv_obj_t* cell = lv_obj_create(grid);
        lv_obj_set_grid_cell(cell, LV_GRID_ALIGN_STRETCH, col, 1, LV_GRID_ALIGN_STRETCH, row, 1);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        g_panels[i] = buildZonePanel(cell, kZoneTitles[i], zoneHasDewPoint(id));
    }

    // --- Баннер статуса ---
    g_banner = lv_obj_create(scr);
    lv_obj_set_size(g_banner, LV_PCT(100), 56);
    lv_obj_set_flex_align(g_banner, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_flex_flow(g_banner, LV_FLEX_FLOW_ROW);

    g_spinner = lv_spinner_create(g_banner);
    lv_obj_set_size(g_spinner, 32, 32);
    lv_spinner_set_anim_params(g_spinner, 1000, 200);  // оборот в секунду — визуально читается как "вращается"
    lv_obj_set_style_arc_color(g_spinner, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_add_flag(g_spinner, LV_OBJ_FLAG_HIDDEN);  // виден только пока реле включено, см. update()

    g_bannerLabel = lv_label_create(g_banner);
    lv_obj_set_style_text_font(g_bannerLabel, &font_ru_28_bold, 0);
    lv_label_set_text(g_bannerLabel, "ОЖИДАНИЕ");
}

void update() {
    SystemState snapshot;
    if (!ShaState::getSnapshot(snapshot)) return;

    for (size_t i = 0; i < static_cast<size_t>(SensorId::Count); i++) {
        updateZonePanel(g_panels[i], snapshot.readings[i]);
    }

    char buf[48];
    formatUptime(buf, sizeof(buf), millis());
    lv_label_set_text(g_uptimeLabel, buf);

    if (snapshot.wifiConnected) {
        snprintf(buf, sizeof(buf), "WiFi: %d дБм", snapshot.wifiRssi);
    } else {
        snprintf(buf, sizeof(buf), "WiFi: --");
    }
    lv_label_set_text(g_wifiLabel, buf);

    size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    snprintf(buf, sizeof(buf), "ОЗУ: %u КБ", static_cast<unsigned>(freeHeap / 1024));
    lv_label_set_text(g_ramLabel, buf);

    lv_label_set_text(g_modeLabel, Relay::modeBadgeText(snapshot.settings.mode));

    for (size_t i = 0; i < 3; i++) {
        bool active = (i == static_cast<size_t>(snapshot.settings.mode));
        lv_obj_set_style_bg_color(g_modeButtons[i],
                                   active ? lv_color_hex(0x2E6DA4) : lv_color_hex(0x3A4048), 0);
    }

    lv_label_set_text(g_bannerLabel, Relay::bannerText(snapshot.relay.state));
    lv_obj_set_style_bg_color(g_banner, bannerColorFor(snapshot.relay.state), 0);

    if (snapshot.relay.relayOn) {
        lv_obj_clear_flag(g_spinner, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_spinner, LV_OBJ_FLAG_HIDDEN);
    }
}

}  // namespace UiDashboard
