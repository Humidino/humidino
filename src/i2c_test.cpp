// Изолированный тест ОДНОГО датчика по I2C0 (GPIO21=SDA, GPIO18=SCL).
// Собирается и заливается отдельно, без основной прошивки:
//
//   pio run -e i2c_test -t upload --upload-port COM4
//   pio device monitor -e i2c_test -p COM4
//
// Ничего не зависит от остального проекта (config.h/shared_state и т.д.) —
// только Arduino + Wire, чтобы тестировать саму физическую линию в отрыве
// от логики основной прошивки (watchdog, восстановление шины, LVGL и т.д.).
//
// Что проверяет:
//  1. Сырой уровень SDA/SCL ДО Wire.begin() — если линия реально доведена
//     проводом до платы, при INPUT_PULLUP она должна читаться HIGH в покое.
//     Если сразу LOW — либо провод не туда подключён (например, перепутан с
//     GND), либо на линии внешний "тяни к земле" (залипшее ведомое, шорт).
//  2. Время каждой I2C-транзакции (micros() до/после) — если зависание, это
//     будет сразу видно как транзакция на много миллисекунд дольше нормы,
//     а не просто "не отвечает".
//  3. Прямой опрос SHT31 без библиотеки Adafruit (сырые команды по датчику),
//     чтобы исключить любые побочные эффекты остальной прошивки.

#include <Arduino.h>
#include <Wire.h>

namespace {

constexpr uint8_t PIN_SDA = 47;  // временно I2C1 (улица) вместо 21 — см. диагностику уличного датчика
constexpr uint8_t PIN_SCL = 8;  // временно I2C1 (улица) вместо 48
constexpr uint32_t I2C_CLOCK_HZ = 10000;
constexpr uint16_t I2C_TIMEOUT_MS = 50;

constexpr uint8_t SHT31_ADDR_A = 0x44;
constexpr uint8_t SHT31_ADDR_B = 0x45;

// Команда SHT31 "single shot, high repeatability, no clock stretching".
constexpr uint8_t SHT31_CMD_MSB = 0x24;
constexpr uint8_t SHT31_CMD_LSB = 0x00;

uint8_t crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31) : static_cast<uint8_t>(crc << 1);
        }
    }
    return crc;
}

void printPinLevels(const char* when) {
    pinMode(PIN_SDA, INPUT_PULLUP);
    pinMode(PIN_SCL, INPUT_PULLUP);
    delay(2);
    int sda = digitalRead(PIN_SDA);
    int scl = digitalRead(PIN_SCL);
    Serial.printf("[%s] SDA(GPIO%u)=%s  SCL(GPIO%u)=%s  %s\n", when, PIN_SDA, sda ? "HIGH" : "LOW", PIN_SCL,
                  scl ? "HIGH" : "LOW",
                  (sda && scl) ? "(норма, обе в покое HIGH)"
                               : "(!) хотя бы одна линия LOW — обрыв/шорт/не туда подключено");
}

// Возвращает 0 при ACK (endTransmission), иначе код ошибки Wire, и печатает
// сколько микросекунд заняла ТОЛЬКО эта транзакция — так видно зависание,
// а не просто "нет ответа".
uint8_t probeAddr(uint8_t addr) {
    uint32_t t0 = micros();
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    uint32_t dt = micros() - t0;
    Serial.printf("  probe 0x%02X: %s, %lu мкс%s\n", addr, err == 0 ? "ACK" : "NACK/ошибка", (unsigned long)dt,
                  dt > (I2C_TIMEOUT_MS * 1000UL / 2) ? "  (!) подозрительно долго, похоже на зависание шины" : "");
    return err;
}

// Полный цикл измерения SHT31 напрямую по протоколу, без библиотеки —
// исключает любые сторонние причины сбоя, кроме самой I2C-линии и датчика.
bool readSht31Raw(uint8_t addr, float& tempC, float& rh) {
    Wire.beginTransmission(addr);
    Wire.write(SHT31_CMD_MSB);
    Wire.write(SHT31_CMD_LSB);
    if (Wire.endTransmission() != 0) {
        Serial.printf("  0x%02X: не подтвердил команду измерения\n", addr);
        return false;
    }

    delay(20);  // датчику нужно время на конверсию (по даташиту макс. ~15 мс)

    uint8_t n = Wire.requestFrom(static_cast<int>(addr), 6);
    if (n != 6) {
        Serial.printf("  0x%02X: получено %u байт вместо 6\n", addr, n);
        return false;
    }

    uint8_t buf[6];
    for (uint8_t i = 0; i < 6; i++) buf[i] = Wire.read();

    if (crc8(buf, 2) != buf[2] || crc8(buf + 3, 2) != buf[5]) {
        Serial.printf("  0x%02X: ошибка CRC (данные на линии повреждены/наводки)\n", addr);
        return false;
    }

    uint16_t rawT = (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
    uint16_t rawRh = (static_cast<uint16_t>(buf[3]) << 8) | buf[4];
    tempC = -45.0f + 175.0f * (static_cast<float>(rawT) / 65535.0f);
    rh = 100.0f * (static_cast<float>(rawRh) / 65535.0f);
    return true;
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.printf("=== i2c_test: изолированная проверка датчика на SDA=GPIO%u SCL=GPIO%u ===\n", PIN_SDA, PIN_SCL);

    // Уровни проверяем ТОЛЬКО до Wire.begin(): pinMode() внутри
    // printPinLevels() физически переключает матрицу GPIO обратно на простой
    // цифровой вход, снимая пины с аппаратной I2C-периферии. Если вызвать
    // это после Wire.begin(), любая последующая транзакция будет стучаться
    // в пины, отключённые от контроллера I2C, и виснуть на таймауте шины —
    // именно так тест раньше сам себе имитировал "датчик не отвечает".
    printPinLevels("до Wire.begin()");

    Wire.begin(PIN_SDA, PIN_SCL, I2C_CLOCK_HZ);
    Wire.setTimeOut(I2C_TIMEOUT_MS);
}

void loop() {
    Serial.println("----------------------------------------------------------");

    uint8_t errA = probeAddr(SHT31_ADDR_A);
    uint8_t errB = probeAddr(SHT31_ADDR_B);

    if (errA == 0) {
        float t, rh;
        if (readSht31Raw(SHT31_ADDR_A, t, rh)) {
            Serial.printf("  0x%02X: %.2f °C, %.2f %%RH — датчик рабочий\n", SHT31_ADDR_A, t, rh);
        }
    }
    if (errB == 0) {
        float t, rh;
        if (readSht31Raw(SHT31_ADDR_B, t, rh)) {
            Serial.printf("  0x%02X: %.2f °C, %.2f %%RH — датчик рабочий\n", SHT31_ADDR_B, t, rh);
        }
    }

    if (errA != 0 && errB != 0) {
        Serial.println("  Ни один адрес не отвечает. Если SDA/SCL выше показывали HIGH и"
                        " до, и после Wire.begin() — линии физически не доведены до датчика"
                        " (обрыв в кабеле/разъёме) или перепутаны местами с VCC/GND.");
    }

    delay(1000);
}
