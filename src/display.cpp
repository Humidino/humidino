#include "display.h"

#include <TFT_eSPI.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include "backlight.h"
#include "config.h"
#include "ui_dashboard.h"
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

    Backlight::begin();

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

    UiDashboard::build();
    lv_timer_create([](lv_timer_t*) { UiDashboard::update(); }, 500, nullptr);

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
    xTaskCreatePinnedToCore(lvglTask, "lvglTask", 16384, nullptr, 2, nullptr, 1);
}

}  // namespace Display
