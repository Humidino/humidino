#include "ui_stats_screen.h"

#include <Arduino.h>
#include <cmath>
#include <ctime>
#include <vector>

#include "config.h"
#include "fonts/fonts.h"
#include "run_log.h"

namespace {

// На экране показываем только последние kMaxRowsShown циклов — полный
// журнал (до RUN_LOG_CAPACITY записей) смотрят через веб-интерфейс
// (/api/history), тут только "что было недавно", без пагинации на
// маленьком тачскрине. Держим число небольшим и по другой причине: у LVGL
// всего 48 КБ (LV_MEM_SIZE в lv_conf.h) под все объекты сразу на всех 4
// вкладках (они не уничтожаются при переключении, только скрываются) — не
// проверено на реальном железе, поэтому лучше не приближаться к пределу.
constexpr size_t kMaxRowsShown = 10;

lv_obj_t* g_summaryLabel;
lv_obj_t* g_totalLabel;
lv_obj_t* g_timeWarningLabel;
lv_obj_t* g_emptyLabel;
lv_obj_t* g_rows;

std::vector<RunLog::RunRecord> g_cachedRecords;

void fmt1(char* buf, size_t n, float v) {
    if (isnan(v)) snprintf(buf, n, "-");
    else snprintf(buf, n, "%.1f", v);
}

void formatClock(char* out, size_t outSize, uint32_t epoch) {
    if (epoch == 0) {
        snprintf(out, outSize, "--:-- --.--");
        return;
    }
    time_t local = static_cast<time_t>(epoch) + LOCAL_TZ_OFFSET_SEC;
    struct tm tmv;
    gmtime_r(&local, &tmv);
    snprintf(out, outSize, "%02d:%02d %02d.%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_mday, tmv.tm_mon + 1);
}

void formatDuration(char* out, size_t outSize, uint32_t ms) {
    uint32_t minutes = ms / 60000;
    if (minutes < 60) {
        snprintf(out, outSize, "%lu мин", (unsigned long)minutes);
    } else {
        snprintf(out, outSize, "%luч %02luм", (unsigned long)(minutes / 60), (unsigned long)(minutes % 60));
    }
}

const char* reasonText(RunLog::StopReason reason) {
    switch (reason) {
        case RunLog::StopReason::HysteresisReached:
            return "по графику";
        case RunLog::StopReason::ManualOff:
            return "вручную";
        case RunLog::StopReason::LockedFreeze:
            return "мороз";
        case RunLog::StopReason::LockedCondensation:
            return "конденсат";
        case RunLog::StopReason::SensorFault:
            return "ERR";
        case RunLog::StopReason::Interrupted:
            return "перезагрузка";
        case RunLog::StopReason::Unknown:
        default:
            // На практике сюда не попадаем — в buildRow() для ещё не
            // закрытой записи стоит отдельный лейбл "РАБОТАЕТ"; это только
            // fallback на случай прямого вызова reasonText(Unknown).
            return "идёт";
    }
}

// "->" вместо юникодной стрелки "→" — кастомные шрифты font_ru_* собраны
// только с ASCII + ° + кириллицей (см. cmap-таблицу в src/fonts/font_ru_14.c),
// лишний символ иначе не отрисуется (пустой квадрат вместо глифа).
void buildZoneLine(char* out, size_t outSize, const char* label, float startRh, float endRh, float startT,
                    float endT, bool inProgress) {
    char sRh[8], eRh[8], sT[8], eT[8];
    fmt1(sRh, sizeof(sRh), startRh);
    fmt1(sT, sizeof(sT), startT);
    if (inProgress) {
        snprintf(out, outSize, "%s: %s%%  %s°C (сейчас)", label, sRh, sT);
    } else {
        fmt1(eRh, sizeof(eRh), endRh);
        fmt1(eT, sizeof(eT), endT);
        snprintf(out, outSize, "%s: %s->%s%%  %s->%s°C", label, sRh, eRh, sT, eT);
    }
}

void buildRow(size_t index) {
    const RunLog::RunRecord& r = g_cachedRecords[index];
    bool inProgress = (r.stopReason == RunLog::StopReason::Unknown);

    lv_obj_t* row = lv_obj_create(g_rows);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_set_style_pad_row(row, 1, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x2A323C), 0);

    lv_obj_t* topRow = lv_obj_create(row);
    lv_obj_set_width(topRow, LV_PCT(100));
    lv_obj_set_height(topRow, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(topRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(topRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(topRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(topRow, 0, 0);
    lv_obj_set_style_pad_all(topRow, 0, 0);

    char buf[40];
    formatClock(buf, sizeof(buf), r.startEpoch);
    lv_obj_t* timeLbl = lv_label_create(topRow);
    lv_obj_set_style_text_font(timeLbl, &font_ru_14, 0);
    lv_label_set_text(timeLbl, buf);

    if (inProgress) {
        lv_obj_t* durLbl = lv_label_create(topRow);
        lv_obj_set_style_text_font(durLbl, &font_ru_14, 0);
        lv_obj_set_style_text_color(durLbl, lv_color_hex(0x4CAF50), 0);
        lv_label_set_text(durLbl, "РАБОТАЕТ");
    } else {
        formatDuration(buf, sizeof(buf), r.durationMs);
        lv_obj_t* durLbl = lv_label_create(topRow);
        lv_obj_set_style_text_font(durLbl, &font_ru_14, 0);
        lv_label_set_text(durLbl, buf);

        lv_obj_t* reasonLbl = lv_label_create(topRow);
        lv_obj_set_style_text_font(reasonLbl, &font_ru_14, 0);
        lv_obj_set_style_text_color(reasonLbl, lv_color_hex(0x8AA0B8), 0);
        lv_label_set_text(reasonLbl, reasonText(r.stopReason));
    }

    buildZoneLine(buf, sizeof(buf), "Подпол", r.startCrawlRh, r.endCrawlRh, r.startCrawlTempC, r.endCrawlTempC,
                  inProgress);
    lv_obj_t* crawlLbl = lv_label_create(row);
    lv_obj_set_style_text_font(crawlLbl, &font_ru_14, 0);
    lv_obj_set_style_text_color(crawlLbl, lv_color_hex(0xC0C8D0), 0);
    lv_label_set_text(crawlLbl, buf);

    buildZoneLine(buf, sizeof(buf), "Улица", r.startOutsideRh, r.endOutsideRh, r.startOutsideTempC,
                  r.endOutsideTempC, inProgress);
    lv_obj_t* outsideLbl = lv_label_create(row);
    lv_obj_set_style_text_font(outsideLbl, &font_ru_14, 0);
    lv_obj_set_style_text_color(outsideLbl, lv_color_hex(0xC0C8D0), 0);
    lv_label_set_text(outsideLbl, buf);
}

void rebuildRows() {
    lv_obj_clean(g_rows);
    if (g_cachedRecords.empty()) {
        lv_obj_clear_flag(g_emptyLabel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_emptyLabel, LV_OBJ_FLAG_HIDDEN);
    }
    for (size_t i = 0; i < g_cachedRecords.size(); i++) buildRow(i);
}

}  // namespace

namespace UiStatsScreen {

void build(lv_obj_t* parent) {
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x101418), 0);
    lv_obj_set_style_pad_all(parent, 8, 0);
    lv_obj_set_style_pad_row(parent, 4, 0);

    lv_obj_t* title = lv_label_create(parent);
    lv_obj_set_style_text_font(title, &font_ru_20, 0);
    lv_label_set_text(title, "Статистика");

    g_summaryLabel = lv_label_create(parent);
    lv_obj_set_style_text_font(g_summaryLabel, &font_ru_14, 0);
    lv_label_set_text(g_summaryLabel, "--");

    g_totalLabel = lv_label_create(parent);
    lv_obj_set_style_text_font(g_totalLabel, &font_ru_14, 0);
    lv_obj_set_style_text_color(g_totalLabel, lv_color_hex(0x8AA0B8), 0);
    lv_label_set_text(g_totalLabel, "--");

    g_timeWarningLabel = lv_label_create(parent);
    lv_obj_set_style_text_font(g_timeWarningLabel, &font_ru_14, 0);
    lv_obj_set_style_text_color(g_timeWarningLabel, lv_color_hex(0xC97A1E), 0);
    lv_obj_set_width(g_timeWarningLabel, LV_PCT(100));
    lv_label_set_long_mode(g_timeWarningLabel, LV_LABEL_LONG_WRAP);
    lv_label_set_text(g_timeWarningLabel, "Время ещё не синхронизировано - метки времени недоступны");
    lv_obj_add_flag(g_timeWarningLabel, LV_OBJ_FLAG_HIDDEN);

    g_emptyLabel = lv_label_create(parent);
    lv_obj_set_style_text_font(g_emptyLabel, &font_ru_14, 0);
    lv_label_set_text(g_emptyLabel, "Пока нет ни одного запуска");
    lv_obj_add_flag(g_emptyLabel, LV_OBJ_FLAG_HIDDEN);

    g_rows = lv_obj_create(parent);
    lv_obj_set_width(g_rows, LV_PCT(100));
    lv_obj_set_flex_grow(g_rows, 1);
    lv_obj_set_flex_flow(g_rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(g_rows, 0, 0);
    lv_obj_set_style_pad_row(g_rows, 2, 0);
    lv_obj_set_style_border_width(g_rows, 1, 0);
    lv_obj_set_style_bg_opa(g_rows, LV_OPA_TRANSP, 0);

    refresh();
}

void refresh() {
    RunLog::Summary sum = RunLog::getSummary();

    char buf[64];
    if (sum.timeSynced) {
        char durBuf[24];
        formatDuration(durBuf, sizeof(durBuf), sum.runtimeTodayMs);
        snprintf(buf, sizeof(buf), "Сегодня: %lu запусков, %s", (unsigned long)sum.runsToday, durBuf);
        lv_obj_add_flag(g_timeWarningLabel, LV_OBJ_FLAG_HIDDEN);
    } else {
        snprintf(buf, sizeof(buf), "Сегодня: н/д");
        lv_obj_clear_flag(g_timeWarningLabel, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(g_summaryLabel, buf);

    snprintf(buf, sizeof(buf), "Всего за всё время: %lu", (unsigned long)sum.runsTotal);
    lv_label_set_text(g_totalLabel, buf);

    RunLog::RunRecord records[kMaxRowsShown];
    size_t n = RunLog::getRecent(records, kMaxRowsShown);
    g_cachedRecords.assign(records, records + n);
    rebuildRows();
}

}  // namespace UiStatsScreen
