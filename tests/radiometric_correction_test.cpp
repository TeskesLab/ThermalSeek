#include "radiometric_correction.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
int failures = 0;

void expect(bool condition, const std::string& message) {
  if (condition) {
    return;
  }
  std::cerr << "FAIL: " << message << '\n';
  ++failures;
}

bool nearlyEqual(float actual, float expected, float tolerance = 0.0001F) {
  return std::abs(actual - expected) <= tolerance;
}

void testIdentityCorrection() {
  const RadiometricSettings settings{1.0F, 125.0F};
  expect(isValidRadiometricSettings(settings),
         "identity settings are valid");
  expect(isIdentityRadiometricCorrection(settings),
         "emissivity one is recognized as the identity correction");
  expect(correctApparentTemperature(37.25F, settings) == 37.25F,
         "identity correction preserves the apparent temperature exactly");
}

void testKnownCompensationValues() {
  expect(nearlyEqual(correctApparentTemperature(
                         30.0F, RadiometricSettings{0.95F, 20.0F}),
                     30.499605F),
         "warm target compensation matches the verified radiance equation");
  expect(nearlyEqual(correctApparentTemperature(
                         20.0F, RadiometricSettings{0.5F, 30.0F}),
                     8.857212F),
         "target cooler than its reflected background is corrected downward");
}

void testInvalidSettingsAreRejected() {
  expect(!isValidRadiometricSettings(RadiometricSettings{0.0F, 20.0F}),
         "zero emissivity is rejected");
  expect(!isValidRadiometricSettings(RadiometricSettings{1.01F, 20.0F}),
         "emissivity above one is rejected");
  expect(!isValidRadiometricSettings(
             RadiometricSettings{0.95F, -274.0F}),
         "reflected temperature below absolute zero is rejected");
  expect(!isValidRadiometricSettings(RadiometricSettings{
             std::numeric_limits<float>::quiet_NaN(), 20.0F}),
         "non-finite emissivity is rejected");
  expect(std::isnan(correctApparentTemperature(
             20.0F, RadiometricSettings{0.0F, 20.0F})),
         "scalar correction does not produce a value for invalid settings");

  ThermalFrame apparent;
  apparent.width = 1;
  apparent.height = 1;
  apparent.celsius = {20.0F};
  ThermalFrame corrected;
  bool rejected = false;
  try {
    correctThermalFrame(apparent, RadiometricSettings{0.0F, 20.0F},
                        corrected);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  expect(rejected, "frame correction rejects invalid settings");
}

void testCorrectedFrameStatistics() {
  ThermalFrame apparent;
  apparent.width = 2;
  apparent.height = 2;
  apparent.celsius = {30.0F, 50.0F, 20.0F, 40.0F};

  const RadiometricSettings settings{0.8F, 20.0F};
  ThermalFrame corrected;
  correctThermalFrame(apparent, settings, corrected);

  expect(corrected.width == 2 && corrected.height == 2 &&
             corrected.celsius.size() == 4,
         "corrected frame preserves detector dimensions");
  expect(nearlyEqual(corrected.minimumCelsius, 20.0F) &&
             corrected.minimumX == 0 && corrected.minimumY == 1,
         "corrected frame recalculates its minimum and location");
  expect(nearlyEqual(corrected.maximumCelsius, 56.330282F) &&
             corrected.maximumX == 1 && corrected.maximumY == 0,
         "corrected frame recalculates its maximum and location");
  expect(nearlyEqual(
             corrected.centerCelsius,
             correctApparentTemperature(40.0F, settings)),
         "corrected frame recalculates its center measurement");
}
}  // namespace

int main() {
  testIdentityCorrection();
  testKnownCompensationValues();
  testInvalidSettingsAreRejected();
  testCorrectedFrameStatistics();

  if (failures != 0) {
    std::cerr << failures << " radiometric correction test(s) failed\n";
    return 1;
  }
  std::cout << "All radiometric correction tests passed\n";
  return 0;
}
