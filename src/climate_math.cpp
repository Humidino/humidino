#include "climate_math.h"

#include <cmath>

namespace ClimateMath {

namespace {
constexpr float kMagnusA = 17.62f;
constexpr float kMagnusB = 243.12f;  // °C

// Давление насыщенного пара в гПа (формула Магнуса).
float saturationVaporPressureHpa(float tempC) {
    return 6.112f * std::exp((kMagnusA * tempC) / (kMagnusB + tempC));
}
}  // namespace

float dewPointC(float tempC, float rhPercent) {
    if (rhPercent <= 0.0f) return NAN;
    float rh = rhPercent / 100.0f;
    float gamma = std::log(rh) + (kMagnusA * tempC) / (kMagnusB + tempC);
    return (kMagnusB * gamma) / (kMagnusA - gamma);
}

float absHumidityGm3(float tempC, float rhPercent) {
    float pvHpa = saturationVaporPressureHpa(tempC) * (rhPercent / 100.0f);
    float pvPa = pvHpa * 100.0f;
    float tempK = tempC + 273.15f;
    // Абсолютная влажность (г/м³) = Pv / (Rv * T), Rv = 461,5 Дж/(кг·К) для
    // водяного пара; множитель 1000 переводит кг/м³ -> г/м³.
    return (pvPa / (461.5f * tempK)) * 1000.0f;
}

}  // namespace ClimateMath
