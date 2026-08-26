#define USER_SETUP_INFO "Humidino_ILI9488"

#define ILI9488_DRIVER
#define TFT_WIDTH  320
#define TFT_HEIGHT 480

// ---- ILI9488 3.5" 480x320 SPI, ESP32-S3 ----
#define TFT_MISO 13   // физически не подключён для этого дисплея; TFT_eSPI требует какое-то значение
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_CS   10
#define TFT_DC    9
#define TFT_RST  14
// В этой конструкции нет сенсорного контроллера — TOUCH_CS намеренно не определён.

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

// Явный выбор порта SPI, чтобы SPI дисплея не конфликтовал с периферией I2C.
#define USE_HSPI_PORT
