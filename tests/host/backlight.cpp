#include <Arduino.h>
#include <cstdlib>
#include <iostream>
#include "backlight.h"
#include "config.h"

void check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
    std::cout << "PASS: " << message << '\n';
}
int main() {
    Backlight::begin();
    testNow = BACKLIGHT_DIM_TIMEOUT_MS;
    Backlight::update(testNow);
    testNow += BACKLIGHT_FADE_MS;
    Backlight::update(testNow);
    check(testBacklightDuty == BACKLIGHT_DIM_PCT * 255 / 100, "idle screen dims");
    Backlight::noteActivity();
    Backlight::update(testNow);
    check(testBacklightDuty == 255, "first touch wakes immediately");
    for (int i = 0; i < 200; ++i) {
        testNow += 10;
        Backlight::noteActivity();
        Backlight::update(testNow);
        check(testBacklightDuty == 255, "held touch stays bright");
    }
    testNow += BACKLIGHT_DIM_TIMEOUT_MS;
    Backlight::update(testNow);
    testNow += BACKLIGHT_FADE_MS / 2;
    Backlight::update(testNow);
    check(testBacklightDuty < 255, "dimming resumes after inactivity");
    Backlight::noteActivity(); Backlight::update(testNow);
    check(testBacklightDuty == 255, "activity cancels dimming");
    testNow = UINT32_MAX - 100;
    Backlight::noteActivity(); Backlight::update(testNow);
    testNow += 200;
    Backlight::update(testNow);
    check(testBacklightDuty == 255, "clock wrap does not dim active screen");
}
