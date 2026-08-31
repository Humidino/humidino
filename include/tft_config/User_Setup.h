#define USER_SETUP_INFO "Humidino_ILI9488"

#define ILI9488_DRIVER
#define TFT_WIDTH  320
#define TFT_HEIGHT 480

// ---- ILI9488 3.5" 480x320 SPI, ESP32-S3 ----
#define TFT_MISO 13   // теперь физически подключён — общая линия SPI, нужна тачскрину XPT2046 для ответа
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

// USE_HSPI_PORT не используем. По умолчанию (без флага) TFT_eSPI на ESP32-S3
// берёт SPI_PORT = FSPI, а макрос FSPI в esp32-hal-spi.h для не-classic-ESP32
// чипов равен 1 — но REG_SPI_BASE(i) в soc.h валиден только при i>=2, иначе
// возвращает 0, и SPI_USER_REG(1) == 0x10 → StoreProhibited при первой же
// записи в "регистр" в begin_tft_write(). USE_FSPI_PORT задаёт SPI_PORT
// напрямую как физический индекс 2 (реальный SPI2), это валидный адрес и
// не конфликтует с периферией I2C (у неё свой блок).
#define USE_FSPI_PORT
