#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "thermal_frame.hpp"

using CameraFingerprint = std::array<std::uint8_t, 32>;

inline constexpr std::uint32_t kFixedPatternProfileVersion = 1;
inline constexpr std::size_t kFixedPatternCalibrationFrameCount = 96;
inline constexpr std::size_t kFixedPatternMinimumShutterCount = 4;

struct FixedPatternCalibrationMetrics final {
  float referenceTemperatureCelsius = 0.0F;
  float biasRmsCelsius = 0.0F;
  float biasP01Celsius = 0.0F;
  float biasP99Celsius = 0.0F;
  float maximumAbsoluteBiasCelsius = 0.0F;
  float lowFrequencySpanCelsius = 0.0F;
  float globalMedianSpanCelsius = 0.0F;
  float temporalNoiseRmsCelsius = 0.0F;
};

struct FixedPatternProfile final {
  std::uint32_t formatVersion = kFixedPatternProfileVersion;
  CameraFingerprint cameraFingerprint{};
  std::size_t width = 0;
  std::size_t height = 0;
  std::uint32_t sampleCount = 0;
  std::uint32_t shutterCount = 0;
  FixedPatternCalibrationMetrics metrics;
  std::vector<float> biasCelsius;
};

struct FixedPatternCalibrationResult final {
  std::optional<FixedPatternProfile> profile;
  FixedPatternCalibrationMetrics metrics;
  std::string error;
};

class FixedPatternCalibrator final {
public:
  void noteShutterFrame() noexcept;
  void addFrame(const ThermalFrame &frame);

  std::size_t frameCount() const noexcept;
  std::size_t shutterCount() const noexcept;
  bool isComplete() const noexcept;

  FixedPatternCalibrationResult
  finalize(const CameraFingerprint &cameraFingerprint) const;

private:
  std::size_t width_ = 0;
  std::size_t height_ = 0;
  std::size_t shutterCount_ = 0;
  std::vector<float> frameMedians_;
  std::vector<float> deviations_;
  std::vector<float> frameScratch_;
};

bool isValidFixedPatternProfile(const FixedPatternProfile &profile,
                                std::string *error = nullptr);

void applyFixedPatternCorrection(const ThermalFrame &cameraFrame,
                                 const FixedPatternProfile &profile,
                                 ThermalFrame &correctedFrame);
