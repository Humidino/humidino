#include "i2c_recovery.h"

#include <Arduino.h>

namespace I2CRecovery {

bool unstickPins(uint8_t sdaPin, uint8_t sclPin) {
    pinMode(sclPin, OUTPUT);
    pinMode(sdaPin, INPUT_PULLUP);
    digitalWrite(sclPin, HIGH);

    // Если ведомое устройство держит SDA в LOW посреди транзакции, до 9
    // тактовых импульсов на SCL (максимум бит в одном кадре I2C) позволяют
    // ему завершиться и отпустить линию.
    if (digitalRead(sdaPin) == LOW) {
        for (uint8_t i = 0; i < 9; i++) {
            digitalWrite(sclPin, LOW);
            delayMicroseconds(5);
            digitalWrite(sclPin, HIGH);
            delayMicroseconds(5);
            if (digitalRead(sdaPin) == HIGH) break;
        }
    }

    // Ручное условие STOP: SDA переходит в HIGH, пока SCL в HIGH.
    pinMode(sdaPin, OUTPUT);
    digitalWrite(sdaPin, LOW);
    delayMicroseconds(5);
    digitalWrite(sclPin, HIGH);
    delayMicroseconds(5);
    pinMode(sdaPin, INPUT_PULLUP);
    delayMicroseconds(5);

    return digitalRead(sdaPin) == HIGH;
}

bool recoverBus(TwoWire& bus, uint8_t sdaPin, uint8_t sclPin, uint32_t clockHz) {
    bus.end();
    bool freed = unstickPins(sdaPin, sclPin);
    bus.begin(sdaPin, sclPin, clockHz);
    return freed;
}

}  // namespace I2CRecovery
