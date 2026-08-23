#include "radiometric_correction.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>

namespace {
constexpr double kKelvinOffset = 273.15;

bool hasValidDimensions(const ThermalFrame& frame) noexcept {
  if (frame.width == 0 || frame.height == 0 ||
      frame.width > std::numeric_limits<std::size_t>::max() / frame.height) {
    return false;
  }
  return frame.celsius.size() == frame.width * frame.height;
}

double fourthPower(double value) noexcept {
  const double square = value * value;
  return square * square;
}

float applyRadiometricModel(float apparentTemperatureCelsius,
                            double inverseEmissivity,
                            double reflectedContribution) noexcept {
  if (!std::isfinite(apparentTemperatureCelsius) ||
      apparentTemperatureCelsius < kAbsoluteZeroCelsius) {
    return std::numeric_limits<float>::quiet_NaN();
  }

  const double apparentKelvin =
      static_cast<double>(apparentTemperatureCelsius) + kKelvinOffset;
  const double objectRadiance =
      (fourthPower(apparentKelvin) - reflectedContribution) *
      inverseEmissivity;
  const double correctedKelvin =
      std::sqrt(std::sqrt(std::max(0.0, objectRadiance)));
  const double correctedCelsius = correctedKelvin - kKelvinOffset;
  if (!std::isfinite(correctedCelsius) ||
      correctedCelsius >
          static_cast<double>(std::numeric_limits<float>::max())) {
    return std::numeric_limits<float>::quiet_NaN();
  }
  return static_cast<float>(correctedCelsius);
}
}  // namespace

bool isValidRadiometricSettings(
    const RadiometricSettings& settings) noexcept {
  return std::isfinite(settings.emissivity) && settings.emissivity > 0.0F &&
         settings.emissivity <= 1.0F &&
         std::isfinite(settings.reflectedTemperatureCelsius) &&
         settings.reflectedTemperatureCelsius >= kAbsoluteZeroCelsius;
}

bool isIdentityRadiometricCorrection(
    const RadiometricSettings& settings) noexcept {
  return isValidRadiometricSettings(settings) && settings.emissivity == 1.0F;
}

float correctApparentTemperature(
    float apparentTemperatureCelsius,
    const RadiometricSettings& settings) noexcept {
  if (!isValidRadiometricSettings(settings)) {
    return std::numeric_limits<float>::quiet_NaN();
  }
  if (settings.emissivity == 1.0F) {
    return std::isfinite(apparentTemperatureCelsius) &&
                   apparentTemperatureCelsius >= kAbsoluteZeroCelsius
               ? apparentTemperatureCelsius
               : std::numeric_limits<float>::quiet_NaN();
  }

  const double emissivity = static_cast<double>(settings.emissivity);
  const double reflectedKelvin =
      static_cast<double>(settings.reflectedTemperatureCelsius) +
      kKelvinOffset;
  return applyRadiometricModel(
      apparentTemperatureCelsius, 1.0 / emissivity,
      (1.0 - emissivity) * fourthPower(reflectedKelvin));
}

void correctThermalFrame(const ThermalFrame& apparentFrame,
                         const RadiometricSettings& settings,
                         ThermalFrame& correctedFrame) {
  if (&apparentFrame == &correctedFrame) {
    throw std::invalid_argument(
        "Radiometric correction requires distinct input and output frames");
  }
  if (!isValidRadiometricSettings(settings)) {
    throw std::invalid_argument("Radiometric settings are invalid");
  }
  if (!hasValidDimensions(apparentFrame)) {
    throw std::invalid_argument("Apparent thermal frame dimensions are invalid");
  }
  if (settings.emissivity == 1.0F) {
    correctedFrame = apparentFrame;
    return;
  }

  correctedFrame.width = apparentFrame.width;
  correctedFrame.height = apparentFrame.height;
  correctedFrame.celsius.resize(apparentFrame.celsius.size());

  const double emissivity = static_cast<double>(settings.emissivity);
  const double reflectedKelvin =
      static_cast<double>(settings.reflectedTemperatureCelsius) +
      kKelvinOffset;
  const double inverseEmissivity = 1.0 / emissivity;
  const double reflectedContribution =
      (1.0 - emissivity) * fourthPower(reflectedKelvin);

  float minimum = std::numeric_limits<float>::infinity();
  float maximum = -std::numeric_limits<float>::infinity();
  std::size_t minimumIndex = 0;
  std::size_t maximumIndex = 0;
  for (std::size_t index = 0; index < apparentFrame.celsius.size(); ++index) {
    const float corrected =
        applyRadiometricModel(apparentFrame.celsius[index],
                              inverseEmissivity, reflectedContribution);
    if (!std::isfinite(corrected)) {
      throw std::invalid_argument(
          "Apparent thermal frame contains an invalid temperature");
    }
    correctedFrame.celsius[index] = corrected;
    if (corrected < minimum) {
      minimum = corrected;
      minimumIndex = index;
    }
    if (corrected > maximum) {
      maximum = corrected;
      maximumIndex = index;
    }
  }

  correctedFrame.minimumCelsius = minimum;
  correctedFrame.maximumCelsius = maximum;
  correctedFrame.minimumX = minimumIndex % correctedFrame.width;
  correctedFrame.minimumY = minimumIndex / correctedFrame.width;
  correctedFrame.maximumX = maximumIndex % correctedFrame.width;
  correctedFrame.maximumY = maximumIndex / correctedFrame.width;
  const std::size_t centerIndex =
      (correctedFrame.height / 2) * correctedFrame.width +
      correctedFrame.width / 2;
  correctedFrame.centerCelsius = correctedFrame.celsius[centerIndex];
}
