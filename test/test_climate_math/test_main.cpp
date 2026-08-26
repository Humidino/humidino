#include <unity.h>

#include <cmath>

#include "climate_math.h"

void setUp(void) {}
void tearDown(void) {}

void test_dew_point_known_reference(void) {
    // 20°C / 50% отн. влажности -> точка росы ~9,27°C (эталонное значение).
    float dp = ClimateMath::dewPointC(20.0f, 50.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.3f, 9.27f, dp);
}

void test_dew_point_saturation_equals_temp(void) {
    // При 100% отн. влажности точка росы равна температуре воздуха.
    float dp = ClimateMath::dewPointC(15.0f, 100.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 15.0f, dp);
}

void test_dew_point_monotonic_with_humidity(void) {
    float dpLow = ClimateMath::dewPointC(20.0f, 30.0f);
    float dpHigh = ClimateMath::dewPointC(20.0f, 80.0f);
    TEST_ASSERT_TRUE(dpHigh > dpLow);
}

void test_abs_humidity_known_reference(void) {
    // 20°C / 50% отн. влажности -> ~8,65 г/м³ (эталонное значение).
    float ah = ClimateMath::absHumidityGm3(20.0f, 50.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.3f, 8.65f, ah);
}

void test_abs_humidity_zero_rh_is_zero(void) {
    float ah = ClimateMath::absHumidityGm3(20.0f, 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, ah);
}

void test_abs_humidity_condensation_scenario(void) {
    // Холодный уличный воздух содержит меньше абсолютной влаги, чем более
    // тёплый и влажный воздух подпола при той же отн. влажности — это
    // физическая основа защиты от конденсата в relay.cpp.
    float outsideAh = ClimateMath::absHumidityGm3(2.0f, 80.0f);
    float crawlspaceAh = ClimateMath::absHumidityGm3(18.0f, 75.0f);
    TEST_ASSERT_TRUE(outsideAh < crawlspaceAh);
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_dew_point_known_reference);
    RUN_TEST(test_dew_point_saturation_equals_temp);
    RUN_TEST(test_dew_point_monotonic_with_humidity);
    RUN_TEST(test_abs_humidity_known_reference);
    RUN_TEST(test_abs_humidity_zero_rh_is_zero);
    RUN_TEST(test_abs_humidity_condensation_scenario);
    return UNITY_END();
}
