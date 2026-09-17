#pragma once

#include "thermal_frame.hpp"

struct RadiometricSettings final {
  float emissivity = 1.0F;
  float reflectedTemperatureCelsius = 20.0F;
};

constexpr float kAbsoluteZeroCelsius = -273.15F;

bool isValidRadiometricSettings(
    const RadiometricSettings& settings) noexcept;
bool isIdentityRadiometricCorrection(
    const RadiometricSettings& settings) noexcept;

float correctApparentTemperature(
    float apparentTemperatureCelsius,
    const RadiometricSettings& settings) noexcept;

void correctThermalFrame(const ThermalFrame& apparentFrame,
                         const RadiometricSettings& settings,
                         ThermalFrame& correctedFrame);
