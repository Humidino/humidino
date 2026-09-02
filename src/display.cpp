#include "display.h"

#include <TFT_eSPI.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include "backlight.h"
#include "config.h"
#include "settings_store.h"
#include "ui_root.h"
#include "watchdog.h"

namespace {

constexpr uint16_t kScreenWidth = 480;
constexpr uint16_t kScreenHeight = 320;
constexpr uint32_t kDrawBufLines = 40;  // 480*40*2 байта = ~38 КБ на буфер — мелочь для 8 МБ PSRAM

TFT_eSPI g_tft;
lv_display_t* g_lvDisplay = nullptr;
uint8_t* g_buf1 = nullptr;
uint8_t* g_buf2 = nullptr;

uint32_t lvTickGetCb() { return millis(); }

// Загружает калибровку тачскрина из NVS, если она уже есть; иначе запускает
// интерактивную калибровку TFT_eSPI (просит коснуться меток по очереди) и
// сохраняет результат — так это делается только один раз за всё время
// жизни устройства (или до сброса NVS). Вызывается до lv_init(), пока
// экран ещё рисуется напрямую через TFT_eSPI, а не через LVGL.
void setupTouch() {
    uint16_t calData[Settings::kTouchCalibrationValues];
    if (Settings::loadTouchCalibration(calData)) {
        g_tft.setTouch(calData);
        return;
    }

    // calibrateTouch() блокирует до тех пор, пока пользователь не коснётся
    // всех меток — это может быть дольше WDT_TIMEOUT_S, если устройство
    // включили без присмотра. Снимаем текущую задачу с watchdog на время
    // калибровки, иначе получим перезагрузку прямо посреди неё.
    Watchdog::unregisterCurrentTask();

    g_tft.fillScreen(TFT_BLACK);
    g_tft.setTextColor(TFT_WHITE, TFT_BLACK);
    g_tft.setTextDatum(MC_DATUM);
    g_tft.drawString("Touch each cross to calibrate", g_tft.width() / 2, 20, 2);
    g_tft.calibrateTouch(calData, TFT_MAGENTA, TFT_BLACK, 15);

    Watchdog::registerCurrentTask();

    Settings::saveTouchCalibration(calData);
}

void touchReadCb(lv_indev_t*, lv_indev_data_t* data) {
    uint16_t x, y;
    if (g_tft.getTouch(&x, &y, 600)) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = x;
        data->point.y = y;
        // Раньше подсветка сбрасывала таймер гашения только при смене
        // состояния реле (см. relay.cpp) — экран мог потускнеть прямо
        // во время работы с тач-интерфейсом. Любое касание тоже должно
        // считаться активностью.
        Backlight::noteActivity();
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

void flushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* pxMap) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;

    g_tft.startWrite();
    g_tft.setAddrWindow(area->x1, area->y1, w, h);
    g_tft.pushPixels(reinterpret_cast<uint16_t*>(pxMap), w * h);
    g_tft.endWrite();

    lv_display_flush_ready(disp);
}

bool initLvglBuffers() {
    size_t bufSizePx = static_cast<size_t>(kScreenWidth) * kDrawBufLines;
    size_t bufSizeBytes = bufSizePx * LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565);

    g_buf1 = static_cast<uint8_t*>(heap_caps_malloc(bufSizeBytes, MALLOC_CAP_SPIRAM));
    g_buf2 = static_cast<uint8_t*>(heap_caps_malloc(bufSizeBytes, MALLOC_CAP_SPIRAM));
    if (g_buf1 == nullptr || g_buf2 == nullptr) {
        heap_caps_free(g_buf1);
        heap_caps_free(g_buf2);
        g_buf1 = static_cast<uint8_t*>(
            heap_caps_malloc(bufSizeBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        g_buf2 = static_cast<uint8_t*>(
            heap_caps_malloc(bufSizeBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }

    if (g_buf1 == nullptr || g_buf2 == nullptr) {
        heap_caps_free(g_buf1);
        heap_caps_free(g_buf2);
        g_buf1 = nullptr;
        g_buf2 = nullptr;
        return false;
    }

    lv_display_set_buffers(g_lvDisplay, g_buf1, g_buf2, bufSizeBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    return true;
}

void lvglTask(void*) {
    Watchdog::registerCurrentTask();

    g_tft.init();
    g_tft.setSwapBytes(true);
    g_tft.setRotation(1);  // альбомная ориентация; подправить на стенде, если панель выйдет зеркальной/повёрнутой
    g_tft.fillScreen(TFT_BLACK);

    // Подсветку нужно включить ДО калибровки тача — иначе экран может
    // оставаться тёмным (пин GPIO2 без ledc не гарантированно светится) как
    // раз тогда, когда пользователю нужно увидеть метки и коснуться их.
    Backlight::begin();

    setupTouch();

    lv_init();
    lv_tick_set_cb(lvTickGetCb);
    g_lvDisplay = lv_display_create(kScreenWidth, kScreenHeight);
    lv_display_set_flush_cb(g_lvDisplay, flushCb);
    lv_display_set_color_format(g_lvDisplay, LV_COLOR_FORMAT_RGB565);
    if (!initLvglBuffers()) {
        Serial.println("LVGL: unable to allocate display buffers");
        for (;;) {
            Watchdog::feed();
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    lv_indev_t* touchIndev = lv_indev_create();
    lv_indev_set_type(touchIndev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touchIndev, touchReadCb);  // читает g_tft.getTouch() каждый цикл lv_timer_handler()

    UiRoot::build();
    lv_timer_create([](lv_timer_t*) { UiRoot::update(); }, 500, nullptr);

    Backlight::setLevel(BACKLIGHT_FULL_PCT);

    for (;;) {
        uint32_t now = millis();
        lv_timer_handler();
        Backlight::update(now);
        Watchdog::feed();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

}  // namespace

namespace Display {

void begin() {
    // Стек увеличен с 8192 после добавления тач-интерфейса (4 экрана вместо
    // одного дашборда, экранная клавиатура) — запас на случай более глубоких
    // цепочек вызовов при построении/событиях LVGL-виджетов.
    xTaskCreatePinnedToCore(lvglTask, "lvglTask", 16384, nullptr, 2, nullptr, 1);
}

}  // namespace Display
