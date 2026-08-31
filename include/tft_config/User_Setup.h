#define USER_SETUP_INFO "Humidino_ILI9488"

#define ILI9488_DRIVER
#define TFT_WIDTH  320
#define TFT_HEIGHT 480

// ---- ILI9488 3.5" 480x320 SPI, ESP32-S3 ----
// ВАЖНО: SDO дисплея ILI9488 физически НЕ подключаем к этой линии — у этой
// матрицы SDO не тристейтится при неактивном CS (см. предупреждение самой
// TFT_eSPI рядом с ILI9488_DRIVER), поэтому она забивает ответ XPT2046 на
// общей шине. На GPIO13 подключён только DOUT тачскрина; TFT_MISO здесь
// нужен библиотеке лишь как символическое значение для компиляции.
#define TFT_MISO 13
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_CS   10
#define TFT_DC    9
#define TFT_RST  14

// Тачскрин XPT2046 (резистивный), делит SPI-шину с дисплеем (SCLK/MOSI/MISO
// выше) — отдельный провод нужен только на его собственный chip-select.
// Пин продублирован в config.h как PIN_TOUCH_CS для использования вне
// TFT_eSPI (там, где нужен только номер, а не сама библиотека).
#define TOUCH_CS 16

// Подсветкой (GPIO2) НЕ управляет встроенная поддержка TFT_BL/ШИМ в TFT_eSPI —
// GPIO2 напрямую владеет backlight.cpp через ledc для плавного гашения.
// Определение TFT_BL здесь конфликтовало бы с этим каналом ledc.

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

#define SPI_FREQUENCY       40000000
#define SPI_READ_FREQUENCY  20000000

// На ESP32-S3 реальный REG_SPI_BASE(i) в soc.h (esp32s3/include/soc/soc.h)
// определён как ((i)>=2) ? (DR_REG_SPI2_BASE + (i-2)*0x1000) : 0 — то есть
// валиден только при i>=2. Макрос FSPI в esp32-hal-spi.h для не-classic-ESP32
// чипов равен 1, поэтому и режим по умолчанию (SPI_PORT=FSPI=1), и
// SPI_PORT=2 через SPIClass(FSPI) дают в итоге нулевой/несовместимый адрес →
// SPI_USER_REG уезжает на 0x10 → StoreProhibited в begin_tft_write().
// USE_FSPI_PORT задаёт SPI_PORT=2 (TFT_eSPI_ESP32_S3.h), для которого
// REG_SPI_BASE(2) = DR_REG_SPI2_BASE + 0 — валидный ненулевой адрес
// (в отличие от значения по умолчанию FSPI=1, для которого REG_SPI_BASE
// на этом чипе возвращает 0).
#define USE_FSPI_PORT
