#pragma once
#include <cstdint>

// ============================================================================
// Дисплей (ILI9488 480x320, SPI) — физические пины заданы в
// include/tft_config/User_Setup.h. Здесь пин продублирован ТОЛЬКО для
// подсветки, которой TFT_eSPI не управляет (ей отдельно рулит ledc в
// backlight.cpp).
// ============================================================================
constexpr uint8_t PIN_TFT_BACKLIGHT = 2;

// ============================================================================
// Шина I2C 0: датчики подпола (Wire)
// ============================================================================
constexpr uint8_t PIN_I2C0_SDA = 21;
constexpr uint8_t PIN_I2C0_SCL = 22;

// ============================================================================
// Шина I2C 1: датчики улицы и дома (Wire1)
// ============================================================================
constexpr uint8_t PIN_I2C1_SDA = 32;
constexpr uint8_t PIN_I2C1_SCL = 33;

constexpr uint8_t SHT31_ADDR_A = 0x44;
constexpr uint8_t SHT31_ADDR_B = 0x45;
constexpr uint32_t I2C_CLOCK_HZ = 100000;

// ============================================================================
// Реле (SSR)
// ============================================================================
constexpr uint8_t PIN_RELAY_SSR = 25;
constexpr bool RELAY_ACTIVE_HIGH = true;

// ============================================================================
// Идентификаторы датчиков
// ============================================================================
enum class SensorId : uint8_t {
    CrawlspaceIntake = 0,  // шина0, адрес 0x44 — Зона 1: Приточка подпола
    CrawlspaceCorner = 1,  // шина0, адрес 0x45 — Зона 2: Дальний угол подпола
    Outside          = 2,  // шина1, адрес 0x44 — Зона 3: Улица
    House            = 3,  // шина1, адрес 0x45 — Зона 4: Дом
    Count            = 4
};

// ============================================================================
// Тайминги
// ============================================================================
constexpr uint32_t SENSOR_POLL_INTERVAL_MS  = 3000;  // 3000 мс = 3 с — период опроса датчиков
constexpr uint8_t  MOVING_AVG_WINDOW        = 5;      // окно скользящего среднего, отсчётов
constexpr uint8_t  I2C_FAILURE_RECOVERY_THRESHOLD = 3;  // подряд неудач до восстановления шины

constexpr uint32_t CONTROL_EVAL_INTERVAL_MS = 1000;   // 1000 мс = 1 с — период оценки состояния реле
constexpr uint32_t MIN_RUNTIME_MS           = 10UL * 60 * 1000;  // 10 мин минимальной работы
constexpr uint32_t MIN_PAUSE_MS             = 15UL * 60 * 1000;  // 15 мин минимальной паузы

constexpr float DEFAULT_RH_TARGET_PERCENT   = 70.0f;  // % — порог влажности подпола
constexpr float DEFAULT_HYSTERESIS_PERCENT  = 5.0f;   // % — гистерезис по влажности
constexpr float FREEZE_PROTECT_TEMP_C       = 2.0f;   // °C — защита от замерзания

constexpr uint32_t WDT_TIMEOUT_S            = 8;  // с — таймаут аппаратного watchdog

constexpr uint32_t BACKLIGHT_DIM_TIMEOUT_MS = 5UL * 60 * 1000;  // 5 мин бездействия до гашения
constexpr uint32_t BACKLIGHT_FADE_MS        = 1500;  // 1500 мс = 1,5 с — длительность плавного гашения
constexpr uint8_t  BACKLIGHT_FULL_PCT       = 100;    // % яркости в рабочем режиме
constexpr uint8_t  BACKLIGHT_DIM_PCT        = 15;     // % яркости в приглушённом режиме

// ============================================================================
// Сеть / MQTT по умолчанию
// ============================================================================
constexpr uint16_t WEB_SERVER_PORT          = 80;
constexpr uint16_t DEFAULT_MQTT_PORT        = 1883;
constexpr const char* DEFAULT_MQTT_BASE_TOPIC = "humidino";
constexpr const char* DEVICE_HOSTNAME       = "humidino";
