#include "ui_dashboard.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

#include "config.h"
#include "fonts/fonts.h"
#include "relay.h"
#include "season.h"
#include "settings_actions.h"
#include "shared_state.h"

namespace {

// Смайлик-индикатор комфорта — нарисован примитивами LVGL (круг + глаза +
// дуга-рот), а не символом Unicode: шрифты font_ru_* сгенерированы под узкий
// диапазон глифов (см. fonts.h) и эмодзи не содержат.
// 32, а не прежние 28 — вместе с укрупнёнными глазами (5px) и ртом (дуга
// 20px/3px) даёт больше внутреннего пространства, иначе детали лица снова
// упираются друг в друга на маленьком круге.
constexpr int32_t kFaceSize = 32;

struct FaceWidgets {
    lv_obj_t* face = nullptr;
    lv_obj_t* eyeL = nullptr;
    lv_obj_t* eyeR = nullptr;
    lv_obj_t* mouth = nullptr;  // lv_arc, форма рта = диапазон bg-углов
};

struct ZonePanelWidgets {
    lv_obj_t* value;    // "23.4 °C   61.2 %" одной строкой
    lv_obj_t* dew;      // всегда существует, но не заполняется для зон без точки росы (Улица)
    lv_obj_t* errBadge;
    lv_obj_t* faceSlot;  // всегда существует (выравнивание колонок между зонами), лицо внутри — только если hasFace
    FaceWidgets face;
    bool hasDew;
    bool hasFace;  // лицо комфорта имеет смысл только там, где есть целевая влажность (Подпол)
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
    "Приточка",
    "Середина",
    "Дальний угол",
    "Улица",
};

bool zoneHasDewPoint(SensorId id) {
    return isCrawlspaceSensor(id);
}

// Лицо комфорта показывает, насколько влажность зоны близка к rhTargetPercent
// — эта величина осмысленна только для датчика, которым реально управляет
// реле (см. relay.cpp: crawlRh сравнивается с cfg.rhTargetPercent). У "Улицы"
// целевой влажности нет вовсе, поэтому там лицо не рисуется.
bool zoneHasComfortFace(SensorId id) {
    return id == SensorId::CrawlspaceIntake;
}

// Настроение лица по влажности относительно порога/гистерезиса реле —
// специально не "полоса комфорта" (как на бытовых гигрометрах), а привязка
// к реальной логике управления (relay.cpp): реле включается выше
// rhTargetPercent и выключается ниже rhTargetPercent-hysteresisPercent, ниже
// порога "слишком сухо" не бывает в принципе. Поэтому лицо веселее там, где
// вентилятор точно не нужен, и мрачнее по мере приближения и превышения
// порога, а не по расстоянию от какого-то "идеального" числа.
enum class ComfortMood : uint8_t { Ideal, Good, High, VeryHigh };

ComfortMood comfortMoodFor(float humidityPct, float targetPercent, float hysteresisPercent) {
    // Строго "<", а не "<=" — relay.cpp останавливает реле по тому же
    // сравнению (belowHysteresis = crawlRh < target-hysteresis), так что на
    // самой границе реле ещё работает и лицо не должно уже улыбаться.
    if (humidityPct < targetPercent - hysteresisPercent) return ComfortMood::Ideal;
    if (humidityPct <= targetPercent) return ComfortMood::Good;
    if (humidityPct <= targetPercent + hysteresisPercent) return ComfortMood::High;
    return ComfortMood::VeryHigh;
}

FaceWidgets buildFace(lv_obj_t* slot) {
    FaceWidgets f{};

    f.face = lv_obj_create(slot);
    lv_obj_remove_style_all(f.face);
    lv_obj_set_size(f.face, kFaceSize, kFaceSize);
    lv_obj_set_style_radius(f.face, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(f.face, LV_OPA_COVER, 0);
    lv_obj_center(f.face);

    // Глаза и рот — 5px и 3px дуга вместо прежних 3px/2px: на реальном
    // экране (не в симуляторе) такие тонкие детали внутри 28px лица
    // фактически сливались с фоном и были неразличимы вблизи.
    f.eyeL = lv_obj_create(f.face);
    lv_obj_remove_style_all(f.eyeL);
    lv_obj_set_size(f.eyeL, 5, 5);
    lv_obj_set_style_radius(f.eyeL, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(f.eyeL, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(f.eyeL, lv_color_hex(0x1A1A1A), 0);
    lv_obj_align(f.eyeL, LV_ALIGN_TOP_LEFT, 6, 7);

    f.eyeR = lv_obj_create(f.face);
    lv_obj_remove_style_all(f.eyeR);
    lv_obj_set_size(f.eyeR, 5, 5);
    lv_obj_set_style_radius(f.eyeR, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(f.eyeR, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(f.eyeR, lv_color_hex(0x1A1A1A), 0);
    lv_obj_align(f.eyeR, LV_ALIGN_TOP_RIGHT, -6, 7);

    // Рот — индикаторная дуга lv_arc без фона и без "ручки": lv_arc_set_angles
    // напрямую задаёт углы индикатора (широкая нижняя дуга = улыбка, верхняя
    // = недовольство), в обход обычной логики value/range — рту не нужно
    // "значение", только форма.
    f.mouth = lv_arc_create(f.face);
    lv_obj_remove_style_all(f.mouth);
    lv_obj_set_size(f.mouth, 20, 20);
    lv_obj_set_style_arc_opa(f.mouth, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(f.mouth, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(f.mouth, lv_color_hex(0x1A1A1A), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(f.mouth, true, LV_PART_INDICATOR);
    lv_obj_clear_flag(f.mouth, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(f.mouth, LV_ALIGN_BOTTOM_MID, 0, -3);

    // Скрыто до первого update() с валидными данными — иначе один кадр
    // после старта виден недокрашенный кружок без настроения.
    lv_obj_add_flag(f.face, LV_OBJ_FLAG_HIDDEN);

    return f;
}

void applyComfortMood(const FaceWidgets& f, ComfortMood mood) {
    lv_color_t faceColor;
    int32_t angleStart;
    int32_t angleEnd;

    switch (mood) {
        case ComfortMood::Ideal:
            faceColor = lv_color_hex(0xFFD54A);  // жёлтый — сухо, реле точно не нужно
            angleStart = 20;
            angleEnd = 160;  // широкая улыбка
            break;
        case ComfortMood::Good:
            faceColor = lv_color_hex(0xC9D24A);  // жёлто-зелёный — ещё в порядке
            angleStart = 35;
            angleEnd = 145;  // лёгкая улыбка
            break;
        case ComfortMood::High:
            faceColor = lv_color_hex(0xF0A050);  // оранжевый — выше порога, реле включается
            angleStart = 215;
            angleEnd = 325;  // лёгкое недовольство
            break;
        case ComfortMood::VeryHigh:
        default:
            faceColor = lv_color_hex(0xE05A5A);  // красный — заметно выше порога
            angleStart = 200;
            angleEnd = 340;  // явное недовольство
            break;
    }

    lv_obj_set_style_bg_color(f.face, faceColor, 0);
    lv_arc_set_angles(f.mouth, angleStart, angleEnd);
}

// Одна зона — одна горизонтальная строка на всю ширину экрана: title | value
// | точка росы | ERR. При 4 зонах на книжной сетке 2x2 в альбомной
// ориентации 480x320 на ячейку остаётся ~67px высоты — этого не хватает даже
// на 3 строки текста, отсюда обрезанные подписи и невидимый бейдж ERR на
// фото с платы. Список строк вместо карточек использует свободную ширину
// экрана вместо тесной высоты.
ZonePanelWidgets buildZoneRow(lv_obj_t* parent, const char* title, bool hasDew, bool hasFace) {
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
    lv_label_set_text(titleLabel, title);
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
    lv_label_set_text(w.value, "Нет данных");
    lv_obj_set_width(w.value, LV_PCT(38));
    lv_label_set_long_mode(w.value, LV_LABEL_LONG_DOT);

    w.dew = lv_label_create(row);
    lv_obj_set_style_text_font(w.dew, &font_ru_14, 0);
    lv_label_set_text(w.dew, "");
    lv_obj_set_width(w.dew, LV_PCT(24));
    lv_label_set_long_mode(w.dew, LV_LABEL_LONG_DOT);
    w.hasDew = hasDew;

    // Слот фиксированной ширины создаётся в обеих строках (даже без лица)
    // ради выравнивания колонок между зонами — тот же приём, что и с dew
    // выше (виджет есть всегда, содержимое — не всегда).
    w.faceSlot = lv_obj_create(row);
    lv_obj_remove_style_all(w.faceSlot);
    lv_obj_set_size(w.faceSlot, kFaceSize, kFaceSize);
    w.hasFace = hasFace;
    if (hasFace) {
        w.face = buildFace(w.faceSlot);
    }

    w.errBadge = lv_label_create(row);
    lv_obj_set_style_text_font(w.errBadge, &font_ru_14, 0);
    lv_obj_set_style_text_color(w.errBadge, lv_color_hex(0xE04040), 0);
    lv_label_set_text(w.errBadge, "");
    lv_obj_set_flex_grow(w.errBadge, 1);
    lv_obj_set_style_text_align(w.errBadge, LV_TEXT_ALIGN_RIGHT, 0);

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

void updateZonePanel(const ZonePanelWidgets& w, const SensorReading& r, const RuntimeSettings& settings,
                      bool relayOn) {
    char buf[32];

    if (r.valid) {
        snprintf(buf, sizeof(buf), "%.1f °C %.1f%%", r.temperatureC, r.humidityPct);
        lv_label_set_text(w.value, buf);

        if (w.hasDew) {
            snprintf(buf, sizeof(buf), "т.р. %.1f °C", r.dewPointC);
            lv_label_set_text(w.dew, buf);
        }
    } else {
        // Единая явная надпись вместо прочерков — иначе строка отключённого
        // датчика выглядит как визуально пустая, а не как сообщение об
        // отсутствии данных.
        lv_label_set_text(w.value, "Нет данных");
        if (w.hasDew) lv_label_set_text(w.dew, "");
    }

    lv_label_set_text(w.errBadge, r.error ? "ERR" : "");

    if (w.hasFace) {
        // Пока нет валидных данных (первые секунды после старта) или датчик
        // ошибается — лицо прячем целиком, а не рисуем "нейтральное":
        // настроение без опоры на реальное число только вводило бы в
        // заблуждение.
        if (r.valid && !r.error) {
            lv_obj_clear_flag(w.face.face, LV_OBJ_FLAG_HIDDEN);
            ComfortMood mood =
                comfortMoodFor(r.humidityPct, settings.rhTargetPercent, settings.hysteresisPercent);
            // Реле умеет продолжать работать некоторое время уже после того,
            // как влажность опустилась ниже порога (minRuntimeMs в
            // relay.cpp защищает его от износа при частых включениях) — всё
            // это время лицо не должно радоваться раньше вентилятора.
            if (relayOn && static_cast<uint8_t>(mood) < static_cast<uint8_t>(ComfortMood::High)) {
                mood = ComfortMood::High;
            }
            applyComfortMood(w.face, mood);
        } else {
            lv_obj_add_flag(w.face.face, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// Баннер намеренно не показывает причину простоя (мороз/конденсат/сбой
// датчика) — только сам факт, крутится вентилятор сейчас или нет. Причину
// при необходимости видно в веб-интерфейсе (toString(RelayControlState) в
// JSON-API), а главный экран платы держим простым и однозначным.
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

    g_seasonLabel = lv_label_create(statusBar);
    lv_obj_set_style_text_font(g_seasonLabel, &font_ru_14, 0);
    lv_obj_set_style_text_color(g_seasonLabel, lv_color_hex(0x8AA0B8), 0);
    lv_label_set_text(g_seasonLabel, "");

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
        g_panels[i] = buildZoneRow(list, kZoneTitles[i], zoneHasDewPoint(id), zoneHasComfortFace(id));
    }

    // --- Баннер статуса: ВКЛ/ВЫКЛ сверху, счётчик запусков снизу ---
    g_banner = lv_obj_create(scr);
    lv_obj_set_size(g_banner, LV_PCT(100), 64);
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
    lv_label_set_text(g_bannerLabel, "ВЕНТИЛЯТОР: ВЫКЛ");

    g_cycleCountLabel = lv_label_create(g_banner);
    lv_obj_set_style_text_font(g_cycleCountLabel, &font_ru_14, 0);
    lv_obj_set_style_text_color(g_cycleCountLabel, lv_color_hex(0xC0C8D0), 0);
    lv_label_set_text(g_cycleCountLabel, "Запусков всего: --");
}

void update() {
    SystemState snapshot;
    if (!ShaState::getSnapshot(snapshot)) return;

    for (size_t i = 0; i < static_cast<size_t>(SensorId::Count); i++) {
        updateZonePanel(g_panels[i], snapshot.readings[i], snapshot.settings, snapshot.relay.relayOn);
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
    if (snapshot.settings.seasonAutoEnabled) {
        lv_label_set_text(g_seasonLabel, seasonRuLabel(Season::current()));
        lv_obj_clear_flag(g_seasonLabel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_seasonLabel, LV_OBJ_FLAG_HIDDEN);
    }

    for (size_t i = 0; i < 3; i++) {
        bool active = (i == static_cast<size_t>(snapshot.settings.mode));
        lv_obj_set_style_bg_color(g_modeButtons[i],
                                   active ? lv_color_hex(0x2E6DA4) : lv_color_hex(0x3A4048), 0);
    }

    lv_label_set_text(g_bannerLabel, snapshot.relay.relayOn ? "ВЕНТИЛЯТОР: ВКЛ" : "ВЕНТИЛЯТОР: ВЫКЛ");
    lv_obj_set_style_bg_color(g_banner, bannerColorFor(snapshot.relay.relayOn), 0);

    snprintf(buf, sizeof(buf), "Запусков всего: %lu", (unsigned long)snapshot.relay.cycleCount);
    lv_label_set_text(g_cycleCountLabel, buf);

    if (snapshot.relay.relayOn) {
        lv_obj_clear_flag(g_spinner, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_spinner, LV_OBJ_FLAG_HIDDEN);
    }
}

}  // namespace UiDashboard
