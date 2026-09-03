#include "soft_i2c.h"

#include <Arduino.h>

namespace {
// Максимум ждём растяжку такта ведомым на одном фронте SCL, дальше считаем
// шину зависшей и прерываем транзакцию — без этого отключённый/неисправный
// датчик мог бы заблокировать sensorTask на неопределённое время (см. тот же
// мотив у I2C_TRANSACTION_TIMEOUT_MS для аппаратных шин в config.h).
constexpr uint32_t kClockStretchTimeoutUs = 2000;
}  // namespace

void SoftI2C::begin(uint8_t sdaPin, uint8_t sclPin, uint32_t clockHz) {
    sda_ = sdaPin;
    scl_ = sclPin;
    halfPeriodUs_ = (clockHz == 0) ? 50 : static_cast<uint32_t>(1000000UL / clockHz / 2);
    if (halfPeriodUs_ == 0) halfPeriodUs_ = 1;
    sdaRelease();
    sclRelease();
}

void SoftI2C::sclRelease() { pinMode(scl_, INPUT_PULLUP); }
void SoftI2C::sclLow() {
    pinMode(scl_, OUTPUT);
    digitalWrite(scl_, LOW);
}
void SoftI2C::sdaRelease() { pinMode(sda_, INPUT_PULLUP); }
void SoftI2C::sdaLow() {
    pinMode(sda_, OUTPUT);
    digitalWrite(sda_, LOW);
}

bool SoftI2C::sclWaitHigh() {
    sclRelease();
    uint32_t startUs = micros();
    while (digitalRead(scl_) == LOW) {
        if (static_cast<uint32_t>(micros() - startUs) > kClockStretchTimeoutUs) return false;
    }
    return true;
}

void SoftI2C::start() {
    sdaRelease();
    sclRelease();
    delayMicroseconds(halfPeriodUs_);
    sdaLow();
    delayMicroseconds(halfPeriodUs_);
    sclLow();
}

void SoftI2C::stop() {
    sdaLow();
    delayMicroseconds(halfPeriodUs_);
    sclWaitHigh();
    delayMicroseconds(halfPeriodUs_);
    sdaRelease();
    delayMicroseconds(halfPeriodUs_);
}

bool SoftI2C::writeByte(uint8_t b) {
    for (uint8_t i = 0; i < 8; i++) {
        if (b & 0x80) sdaRelease(); else sdaLow();
        b = static_cast<uint8_t>(b << 1);
        delayMicroseconds(halfPeriodUs_);
        if (!sclWaitHigh()) return false;
        delayMicroseconds(halfPeriodUs_);
        sclLow();
    }
    // 9-й такт — ACK от ведомого: отпускаем SDA, ведомый прижимает её к LOW.
    sdaRelease();
    delayMicroseconds(halfPeriodUs_);
    if (!sclWaitHigh()) return false;
    bool ack = (digitalRead(sda_) == LOW);
    delayMicroseconds(halfPeriodUs_);
    sclLow();
    return ack;
}

uint8_t SoftI2C::readByte(bool ack) {
    uint8_t b = 0;
    sdaRelease();
    for (uint8_t i = 0; i < 8; i++) {
        delayMicroseconds(halfPeriodUs_);
        sclWaitHigh();
        b = static_cast<uint8_t>((b << 1) | (digitalRead(sda_) == HIGH ? 1 : 0));
        delayMicroseconds(halfPeriodUs_);
        sclLow();
    }
    // 9-й такт — ACK/NACK от нас как мастера (NACK на последнем байте кадра
    // сообщает ведомому, что это конец чтения).
    if (ack) sdaLow(); else sdaRelease();
    delayMicroseconds(halfPeriodUs_);
    sclWaitHigh();
    delayMicroseconds(halfPeriodUs_);
    sclLow();
    sdaRelease();
    return b;
}

bool SoftI2C::writeBytes(uint8_t addr, const uint8_t* data, size_t len) {
    start();
    bool ok = writeByte(static_cast<uint8_t>(addr << 1));
    for (size_t i = 0; ok && i < len; i++) ok = writeByte(data[i]);
    stop();
    return ok;
}

bool SoftI2C::readBytes(uint8_t addr, uint8_t* out, size_t len) {
    start();
    bool ok = writeByte(static_cast<uint8_t>((addr << 1) | 1));
    if (ok) {
        for (size_t i = 0; i < len; i++) out[i] = readByte(i + 1 < len);
    }
    stop();
    return ok;
}
