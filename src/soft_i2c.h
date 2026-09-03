#pragma once
#include <cstddef>
#include <cstdint>

// Программная (bit-bang) реализация ведущего I2C на двух произвольных GPIO.
// Нужна, потому что у ESP32-S3 всего два аппаратных контроллера I2C (Wire,
// Wire1, уже заняты шинами I2C0/I2C1 — см. config.h), а датчиков на
// отдельных шинах в проекте четыре. Работает как обычный open-drain I2C:
// обе линии подтянуты внешними резисторами к 3.3В (см. docs/WIRING.md §3),
// сама шина только прижимает их к GND или отпускает в Hi-Z (INPUT_PULLUP) —
// как и настоящий контроллер I2C, никогда не выдаёт HIGH активно.
class SoftI2C {
public:
    void begin(uint8_t sdaPin, uint8_t sclPin, uint32_t clockHz);

    // Полная транзакция записи: START, адрес+W, len байт данных, STOP.
    // false — адрес или любой байт не подтверждён (NACK), т.е. на шине никто
    // не отвечает (обрыв провода, не запитан датчик, обрыв во время
    // растяжки такта дольше таймаута).
    bool writeBytes(uint8_t addr, const uint8_t* data, size_t len);

    // Полная транзакция чтения: START, адрес+R, len байт (ACK на все, кроме
    // последнего — NACK), STOP.
    bool readBytes(uint8_t addr, uint8_t* out, size_t len);

private:
    uint8_t sda_ = 0;
    uint8_t scl_ = 0;
    uint32_t halfPeriodUs_ = 50;

    void sclRelease();
    void sclLow();
    void sdaRelease();
    void sdaLow();
    // Отпускает SCL и ждёт, пока линия реально не станет HIGH — учитывает
    // растяжку такта ведомым (slave clock stretching). false = не дождались
    // за отведённый таймаут, шина считается зависшей на этом такте.
    bool sclWaitHigh();

    void start();
    void stop();
    bool writeByte(uint8_t b);  // true, если ведомый ответил ACK
    uint8_t readByte(bool ack);
};
