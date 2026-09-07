#include "ui_dashboard.h"

#include <Arduino.h>
#include <cstring>
#include <esp_heap_caps.h>
#include <lvgl.h>

#include "config.h"
#include "fonts/fonts.h"
#include "relay.h"
#include "season.h"
#include "settings_actions.h"
#include "shared_state.h"

namespace {

// LVGL выделяет память и инвалидирует текст даже при повторе той же строки.
// На SPI-дисплее не перерисовываем неизменившиеся показания каждые 500 мс.
void setLabelTextIfChanged(lv_obj_t* label, const char* text) {
    if (std::strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

// Статус датчика — красный крест (ошибка) или зелёная галочка (данные
// валидны), нарисованы двумя-тремя отрезками lv_line, а не символом Unicode:
// шрифты font_ru_* сгенерированы под узкий диапазон глифов (см. fonts.h) и
// таких значков не содержат.
constexpr int32_t kStatusIconSize = 20;

const lv_point_precise_t kCrossPointsA[] = {{0, 0}, {kStatusIconSize, kStatusIconSize}};
const lv_point_precise_t kCrossPointsB[] = {{kStatusIconSize, 0}, {0, kStatusIconSize}};
const lv_point_precise_t kCheckPoints[] = {
    {0, kStatusIconSize * 3 / 5}, {kStatusIconSize * 2 / 5, kStatusIconSize}, {kStatusIconSize, 0}};

struct StatusIconWidgets {
    lv_obj_t* crossA = nullptr;
    lv_obj_t* crossB = nullptr;
    lv_obj_t* check = nullptr;
};

struct ZonePanelWidgets {
    lv_obj_t* value;    // "23.4 °C   61.2 %" одной строкой
    lv_obj_t* dew;      // всегда существует, но не заполняется для зон без точки росы (Улица)
    lv_obj_t* statusSlot;  // фиксированный слот под крест/галочку, выравнивает колонки между зонами
    StatusIconWidgets status;
    bool hasDew;
};

lv_obj_t* g_uptimeLabel;
lv_obj_t* g_wifiLabel;
lv_obj_t* g_ramLabel;
lv_obj_t* g_modeLabel;
lv_obj_t* g_seasonLabel;
lv_obj_t* g_banner;
lv_obj_t* g_bannerLabel;
lv_obj_t* g_cycleCountLabel;
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
    UiDashboard::update();
}

void onModeAutoClicked(lv_event_t*) { setMode(OperatingMode::Auto); }
void onModeOnClicked(lv_event_t*) { setMode(OperatingMode::ManualOn); }
void onModeOffClicked(lv_event_t*) { setMode(OperatingMode::ManualOff); }

lv_obj_t* buildModeButton(lv_obj_t* parent, const char* text, lv_event_cb_t cb) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_height(btn, LV_PCT(100));
    lv_obj_set_style_pad_ver(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* label = lv_label_create(btn);
    lv_obj_set_style_text_font(label, &font_ru_14, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xF0F0F0), 0);
    setLabelTextIfChanged(label, text);
    lv_obj_center(label);

    return btn;
}

const char* kZoneTitles[] = {
    "Приточка",
    "Середина",
    "Дальний угол",
    "Улица",
};

bool zoneHasDewPoint(SensorId id) {
    return isCrawlspaceSensor(id);
}

StatusIconWidgets buildStatusIcon(lv_obj_t* slot) {
    StatusIconWidgets s{};

    s.crossA = lv_line_create(slot);
    lv_line_set_points(s.crossA, kCrossPointsA, 2);
    lv_obj_set_style_line_width(s.crossA, 3, 0);
    lv_obj_set_style_line_rounded(s.crossA, true, 0);
    lv_obj_set_style_line_color(s.crossA, lv_color_hex(0xE04040), 0);
    lv_obj_align(s.crossA, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_flag(s.crossA, LV_OBJ_FLAG_HIDDEN);

    s.crossB = lv_line_create(slot);
    lv_line_set_points(s.crossB, kCrossPointsB, 2);
    lv_obj_set_style_line_width(s.crossB, 3, 0);
    lv_obj_set_style_line_rounded(s.crossB, true, 0);
    lv_obj_set_style_line_color(s.crossB, lv_color_hex(0xE04040), 0);
    lv_obj_align(s.crossB, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_flag(s.crossB, LV_OBJ_FLAG_HIDDEN);

    s.check = lv_line_create(slot);
    lv_line_set_points(s.check, kCheckPoints, 3);
    lv_obj_set_style_line_width(s.check, 3, 0);
    lv_obj_set_style_line_rounded(s.check, true, 0);
    lv_obj_set_style_line_color(s.check, lv_color_hex(0x4CAF50), 0);
    lv_obj_align(s.check, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_flag(s.check, LV_OBJ_FLAG_HIDDEN);

    return s;
}

// Одна зона — одна горизонтальная строка на всю ширину экрана: title | value
// | точка росы | статус. При 4 зонах на книжной сетке 2x2 в альбомной
// ориентации 480x320 на ячейку остаётся ~67px высоты — этого не хватает даже
// на 3 строки текста, отсюда обрезанные подписи и невидимый бейдж статуса на
// фото с платы. Список строк вместо карточек использует свободную ширину
// экрана вместо тесной высоты.
ZonePanelWidgets buildZoneRow(lv_obj_t* parent, const char* title, bool hasDew) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_grow(row, 1);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, 6, 0);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x2A323C), 0);

    lv_obj_t* titleLabel = lv_label_create(row);
    lv_obj_set_style_text_font(titleLabel, &font_ru_14, 0);
    setLabelTextIfChanged(titleLabel, title);
    lv_obj_set_width(titleLabel, LV_PCT(22));
    lv_label_set_long_mode(titleLabel, LV_LABEL_LONG_DOT);

    ZonePanelWidgets w{};

    // 38%, а не прежние 28% — строка вида "-45.0 °C  100.0 %" (крайние
    // значения SHT31) на font_ru_20 в 28% ширины переносилась на вторую
    // строку вместо аккуратного усечения многоточием: LV_LABEL_LONG_DOT
    // обрезает только когда высота объекта ограничена одной строкой, а не
    // при авто-высоте внутри flex-строки — так что реальная защита от
    // переноса здесь именно ширина, а не сам long_mode.
    w.value = lv_label_create(row);
    lv_obj_set_style_text_font(w.value, &font_ru_20, 0);
    setLabelTextIfChanged(w.value, "Нет данных");
    lv_obj_set_width(w.value, LV_PCT(38));
    lv_label_set_long_mode(w.value, LV_LABEL_LONG_DOT);

    w.dew = lv_label_create(row);
    lv_obj_set_style_text_font(w.dew, &font_ru_14, 0);
    setLabelTextIfChanged(w.dew, "");
    lv_obj_set_width(w.dew, LV_PCT(24));
    lv_label_set_long_mode(w.dew, LV_LABEL_LONG_DOT);
    w.hasDew = hasDew;

    // Слот растёт на оставшуюся ширину строки и прижимает иконку статуса
    // к правому краю — тот же приём, что и с dew выше (виджет есть всегда,
    // видимое содержимое — не всегда).
    w.statusSlot = lv_obj_create(row);
    lv_obj_remove_style_all(w.statusSlot);
    lv_obj_set_height(w.statusSlot, kStatusIconSize);
    lv_obj_set_flex_grow(w.statusSlot, 1);
    w.status = buildStatusIcon(w.statusSlot);

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
        snprintf(buf, sizeof(buf), "%.1f °C %.1f%%", r.temperatureC, r.humidityPct);
        setLabelTextIfChanged(w.value, buf);

        if (w.hasDew) {
            snprintf(buf, sizeof(buf), "т.р. %.1f °C", r.dewPointC);
            setLabelTextIfChanged(w.dew, buf);
        }
    } else {
        // Единая явная надпись вместо прочерков — иначе строка отключённого
        // датчика выглядит как визуально пустая, а не как сообщение об
        // отсутствии данных.
        setLabelTextIfChanged(w.value, "Нет данных");
        if (w.hasDew) setLabelTextIfChanged(w.dew, "");
    }

    // Пока нет валидных данных (первые секунды после старта) — не показываем
    // ни крест, ни галочку: статуса без опоры на реальные показания ещё нет.
    if (r.error) {
        lv_obj_clear_flag(w.status.crossA, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(w.status.crossB, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(w.status.check, LV_OBJ_FLAG_HIDDEN);
    } else if (r.valid) {
        lv_obj_add_flag(w.status.crossA, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(w.status.crossB, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(w.status.check, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(w.status.crossA, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(w.status.crossB, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(w.status.check, LV_OBJ_FLAG_HIDDEN);
    }
}

const char* bannerTextFor(const RelayStatus& status) {
    if (status.relayOn) return "ВЕНТИЛЯТОР: ВКЛ";
    switch (status.state) {
        case RelayControlState::LockedOutSensorFault: return "ВЫКЛ: ошибка датчиков";
        case RelayControlState::LockedOutFreeze: return "ВЫКЛ: защита от холода";
        case RelayControlState::LockedOutCondensation: return "ВЫКЛ: риск конденсата";
        case RelayControlState::MinPauseHold: return "ВЫКЛ: минимальная пауза";
        default: return "ВЕНТИЛЯТОР: ВЫКЛ";
    }
}

lv_color_t bannerColorFor(bool relayOn) {
    return relayOn ? lv_color_hex(0x2E8B45)    // зелёный
                   : lv_color_hex(0x3A4A5A);   // серо-синий
}

// Сезон, под который сейчас подобраны пороги (см. season.h) — только для
// информации на дашборде, не влияет на сам алгоритм осушения.
const char* seasonRuLabel(Season::Id season) {
    switch (season) {
        case Season::Id::Winter: return "ЗИМА";
        case Season::Id::Spring: return "ВЕСНА";
        case Season::Id::Summer: return "ЛЕТО";
        case Season::Id::Autumn: return "ОСЕНЬ";
    }
    return "?";
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
    lv_obj_set_style_pad_all(statusBar, 2, 0);
    lv_obj_set_flex_flow(statusBar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(statusBar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    g_uptimeLabel = lv_label_create(statusBar);
    lv_obj_set_style_text_font(g_uptimeLabel, &font_ru_14, 0);
    setLabelTextIfChanged(g_uptimeLabel, "Время работы: --");

    g_wifiLabel = lv_label_create(statusBar);
    lv_obj_set_style_text_font(g_wifiLabel, &font_ru_14, 0);
    setLabelTextIfChanged(g_wifiLabel, "WiFi: --");

    g_ramLabel = lv_label_create(statusBar);
    lv_obj_set_style_text_font(g_ramLabel, &font_ru_14, 0);
    setLabelTextIfChanged(g_ramLabel, "ОЗУ: --");

    g_modeLabel = lv_label_create(statusBar);
    lv_obj_set_style_text_font(g_modeLabel, &font_ru_14, 0);
    lv_obj_set_style_text_color(g_modeLabel, lv_color_hex(0x8AA0B8), 0);
    setLabelTextIfChanged(g_modeLabel, "АВТО");

    g_seasonLabel = lv_label_create(statusBar);
    lv_obj_set_style_text_font(g_seasonLabel, &font_ru_14, 0);
    lv_obj_set_style_text_color(g_seasonLabel, lv_color_hex(0x8AA0B8), 0);
    setLabelTextIfChanged(g_seasonLabel, "");

    // --- Кнопки переключения режима (дублируют веб-интерфейс) ---
    lv_obj_t* modeRow = lv_obj_create(scr);
    lv_obj_set_size(modeRow, LV_PCT(100), 34);
    lv_obj_set_flex_flow(modeRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(modeRow, 4, 0);
    lv_obj_set_style_pad_all(modeRow, 0, 0);
    lv_obj_set_style_border_width(modeRow, 0, 0);
    // Кнопки целиком помещаются в строке; движение пальца не должно
    // превращать нажатие в прокрутку этого контейнера.
    lv_obj_clear_flag(modeRow, LV_OBJ_FLAG_SCROLLABLE);

    g_modeButtons[static_cast<size_t>(OperatingMode::Auto)] =
        buildModeButton(modeRow, "АВТО", onModeAutoClicked);
    g_modeButtons[static_cast<size_t>(OperatingMode::ManualOn)] =
        buildModeButton(modeRow, "ВКЛ", onModeOnClicked);
    g_modeButtons[static_cast<size_t>(OperatingMode::ManualOff)] =
        buildModeButton(modeRow, "ВЫКЛ", onModeOffClicked);

    // --- Список зон (по одной строке на всю ширину на каждый датчик) ---
    // Список вместо карточек: в альбомной ориентации высоты под сетку
    // остаётся мало, а ширины — много, поэтому каждая зона занимает одну
    // горизонтальную строку, а не тесную колонку из нескольких строк текста.
    lv_obj_t* list = lv_obj_create(scr);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_border_width(list, 1, 0);

    for (size_t i = 0; i < static_cast<size_t>(SensorId::Count); i++) {
        SensorId id = static_cast<SensorId>(i);
        g_panels[i] = buildZoneRow(list, kZoneTitles[i], zoneHasDewPoint(id));
    }

    // --- Баннер статуса: ВКЛ/ВЫКЛ сверху, счётчик запусков снизу ---
    g_banner = lv_obj_create(scr);
    lv_obj_set_size(g_banner, LV_PCT(100), 64);
    lv_obj_set_style_pad_all(g_banner, 4, 0);
    lv_obj_clear_flag(g_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(g_banner, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_banner, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_banner, 2, 0);

    lv_obj_t* statusRow = lv_obj_create(g_banner);
    lv_obj_set_size(statusRow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(statusRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(statusRow, 0, 0);
    lv_obj_set_style_pad_all(statusRow, 0, 0);
    lv_obj_set_flex_flow(statusRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(statusRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    g_spinner = lv_spinner_create(statusRow);
    lv_obj_set_size(g_spinner, 32, 32);
    lv_spinner_set_anim_params(g_spinner, 1000, 200);  // оборот в секунду — визуально читается как "вращается"
    lv_obj_set_style_arc_color(g_spinner, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_add_flag(g_spinner, LV_OBJ_FLAG_HIDDEN);  // виден только пока реле включено, см. update()

    g_bannerLabel = lv_label_create(statusRow);
    // font_ru_28_bold (использовался для коротких слов вроде "РАБОТА") не
    // помещается в баннер с текстом "ВЕНТИЛЯТОР: ВЫКЛ" — обрезался по правому
    // краю. font_ru_20 короче по ширине глифов и с запасом влезает во всю
    // ширину экрана даже с русским текстом такой длины.
    lv_obj_set_style_text_font(g_bannerLabel, &font_ru_20, 0);
    // Тема LVGL по умолчанию красит текст лейблов в тёмный цвет, рассчитанный
    // на светлый фон карточек — на нашем явно тёмном фоне баннера (см.
    // bannerColorFor()) он становится почти нечитаемым. Задаём светлый цвет
    // явно, а не полагаемся на тему.
    lv_obj_set_style_text_color(g_bannerLabel, lv_color_hex(0xF0F0F0), 0);
    setLabelTextIfChanged(g_bannerLabel, "ВЕНТИЛЯТОР: ВЫКЛ");

    g_cycleCountLabel = lv_label_create(g_banner);
    lv_obj_set_style_text_font(g_cycleCountLabel, &font_ru_14, 0);
    lv_obj_set_style_text_color(g_cycleCountLabel, lv_color_hex(0xC0C8D0), 0);
    setLabelTextIfChanged(g_cycleCountLabel, "Запусков всего: --");
}

void update() {
    SystemState snapshot;
    if (!ShaState::getSnapshot(snapshot)) return;

    for (size_t i = 0; i < static_cast<size_t>(SensorId::Count); i++) {
        updateZonePanel(g_panels[i], snapshot.readings[i]);
    }

    char buf[48];
    formatUptime(buf, sizeof(buf), millis());
    setLabelTextIfChanged(g_uptimeLabel, buf);

    if (snapshot.wifiConnected) {
        snprintf(buf, sizeof(buf), "WiFi: %d дБм", snapshot.wifiRssi);
    } else {
        snprintf(buf, sizeof(buf), "WiFi: --");
    }
    setLabelTextIfChanged(g_wifiLabel, buf);

    size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    snprintf(buf, sizeof(buf), "ОЗУ: %u КБ", static_cast<unsigned>(freeHeap / 1024));
    setLabelTextIfChanged(g_ramLabel, buf);

    setLabelTextIfChanged(g_modeLabel, Relay::modeBadgeText(snapshot.settings.mode));
    if (snapshot.settings.seasonAutoEnabled) {
        setLabelTextIfChanged(g_seasonLabel, seasonRuLabel(Season::current()));
        lv_obj_clear_flag(g_seasonLabel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_seasonLabel, LV_OBJ_FLAG_HIDDEN);
    }

    for (size_t i = 0; i < 3; i++) {
        bool active = (i == static_cast<size_t>(snapshot.settings.mode));
        lv_obj_set_style_bg_color(g_modeButtons[i],
                                   active ? lv_color_hex(0x2E6DA4) : lv_color_hex(0x3A4048), 0);
    }

    setLabelTextIfChanged(g_bannerLabel, bannerTextFor(snapshot.relay));
    lv_obj_set_style_bg_color(g_banner, bannerColorFor(snapshot.relay.relayOn), 0);

    snprintf(buf, sizeof(buf), "Запусков всего: %lu", (unsigned long)snapshot.relay.cycleCount);
    setLabelTextIfChanged(g_cycleCountLabel, buf);

    if (snapshot.relay.relayOn) {
        lv_obj_clear_flag(g_spinner, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_spinner, LV_OBJ_FLAG_HIDDEN);
    }
}

}  // namespace UiDashboard
