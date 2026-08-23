#include "fixed_pattern_correction.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr float kMaximumGlobalMedianSpanCelsius = 2.0F;
constexpr float kMaximumLowFrequencySpanCelsius = 3.0F;
constexpr float kMaximumTemporalNoiseRmsCelsius = 0.5F;
constexpr float kMaximumBiasRmsCelsius = 1.5F;
constexpr float kMaximumAbsoluteBiasCelsius = 5.0F;

bool hasValidDimensions(const ThermalFrame &frame) noexcept {
  if (frame.width == 0 || frame.height == 0 ||
      frame.width > std::numeric_limits<std::size_t>::max() / frame.height) {
    return false;
  }
  return frame.celsius.size() == frame.width * frame.height;
}

void setError(std::string *error, const std::string &message) {
  if (error != nullptr) {
    *error = message;
  }
}

float medianInPlace(std::vector<float> &values) {
  if (values.empty()) {
    throw std::invalid_argument("Cannot calculate the median of no values");
  }

  const std::size_t middle = values.size() / 2;
  std::nth_element(values.begin(), values.begin() + middle, values.end());
  const float upper = values[middle];
  if ((values.size() & 1U) != 0U) {
    return upper;
  }
  const float lower =
      *std::max_element(values.begin(), values.begin() + middle);
  return (lower + upper) * 0.5F;
}

float medianOfSmallWindow(std::array<float, 25> &values, std::size_t count) {
  const std::size_t middle = count / 2;
  std::nth_element(values.begin(), values.begin() + middle,
                   values.begin() + count);
  const float upper = values[middle];
  if ((count & 1U) != 0U) {
    return upper;
  }
  const float lower =
      *std::max_element(values.begin(), values.begin() + middle);
  return (lower + upper) * 0.5F;
}

float sortedQuantile(const std::vector<float> &sortedValues, double quantile) {
  if (sortedValues.empty()) {
    throw std::invalid_argument("Cannot calculate a quantile of no values");
  }
  const double position =
      quantile * static_cast<double>(sortedValues.size() - 1);
  const std::size_t lowerIndex = static_cast<std::size_t>(position);
  const std::size_t upperIndex =
      std::min(lowerIndex + 1, sortedValues.size() - 1);
  const double fraction = position - static_cast<double>(lowerIndex);
  return static_cast<float>(
      static_cast<double>(sortedValues[lowerIndex]) * (1.0 - fraction) +
      static_cast<double>(sortedValues[upperIndex]) * fraction);
}

std::string metricFailure(const char *prefix, float actual, float limit) {
  std::ostringstream message;
  message << prefix << ' ' << std::fixed << std::setprecision(2) << actual
          << " °C exceeds the " << limit << " °C limit.";
  return message.str();
}

bool hasValidProfileMetadata(const FixedPatternProfile &profile,
                             std::string *error) {
  if (profile.formatVersion != kFixedPatternProfileVersion) {
    setError(error, "Fixed-pattern profile version is unsupported");
    return false;
  }
  if (profile.width == 0 || profile.height == 0 ||
      profile.width >
          std::numeric_limits<std::size_t>::max() / profile.height) {
    setError(error, "Fixed-pattern profile dimensions are invalid");
    return false;
  }
  const std::size_t pixelCount = profile.width * profile.height;
  if (profile.biasCelsius.size() != pixelCount) {
    setError(error, "Fixed-pattern profile pixel count is invalid");
    return false;
  }
  if (profile.sampleCount < kFixedPatternCalibrationFrameCount) {
    setError(error, "Fixed-pattern profile has too few calibration frames");
    return false;
  }
  if (profile.shutterCount < kFixedPatternMinimumShutterCount) {
    setError(error, "Fixed-pattern profile has too few shutter refreshes");
    return false;
  }

  const FixedPatternCalibrationMetrics &metrics = profile.metrics;
  const std::array<float, 8> metricValues{
      metrics.referenceTemperatureCelsius,
      metrics.biasRmsCelsius,
      metrics.biasP01Celsius,
      metrics.biasP99Celsius,
      metrics.maximumAbsoluteBiasCelsius,
      metrics.lowFrequencySpanCelsius,
      metrics.globalMedianSpanCelsius,
      metrics.temporalNoiseRmsCelsius,
  };
  if (!std::all_of(metricValues.begin(), metricValues.end(),
                   [](float value) { return std::isfinite(value); })) {
    setError(error, "Fixed-pattern profile metrics are not finite");
    return false;
  }
  if (metrics.biasRmsCelsius < 0.0F ||
      metrics.maximumAbsoluteBiasCelsius < 0.0F ||
      metrics.lowFrequencySpanCelsius < 0.0F ||
      metrics.globalMedianSpanCelsius < 0.0F ||
      metrics.temporalNoiseRmsCelsius < 0.0F ||
      metrics.biasP01Celsius > metrics.biasP99Celsius) {
    setError(error, "Fixed-pattern profile metrics are inconsistent");
    return false;
  }
  if (metrics.globalMedianSpanCelsius > kMaximumGlobalMedianSpanCelsius ||
      metrics.lowFrequencySpanCelsius > kMaximumLowFrequencySpanCelsius ||
      metrics.temporalNoiseRmsCelsius > kMaximumTemporalNoiseRmsCelsius ||
      metrics.biasRmsCelsius > kMaximumBiasRmsCelsius ||
      metrics.maximumAbsoluteBiasCelsius > kMaximumAbsoluteBiasCelsius) {
    setError(error, "Fixed-pattern profile exceeds calibration safety limits");
    return false;
  }
  return true;
}
} // namespace

void FixedPatternCalibrator::noteShutterFrame() noexcept {
  if (shutterCount_ < std::numeric_limits<std::uint32_t>::max()) {
    ++shutterCount_;
  }
}

void FixedPatternCalibrator::addFrame(const ThermalFrame &frame) {
  if (frameCount() >= kFixedPatternCalibrationFrameCount) {
    throw std::logic_error(
        "Fixed-pattern calibration already has enough frames");
  }
  if (!hasValidDimensions(frame)) {
    throw std::invalid_argument(
        "Fixed-pattern calibration frame dimensions are invalid");
  }
  if (!std::all_of(frame.celsius.begin(), frame.celsius.end(),
                   [](float value) { return std::isfinite(value); })) {
    throw std::invalid_argument(
        "Fixed-pattern calibration frame contains an invalid temperature");
  }

  if (frameMedians_.empty()) {
    width_ = frame.width;
    height_ = frame.height;
    const std::size_t pixelCount = width_ * height_;
    if (pixelCount > std::numeric_limits<std::size_t>::max() /
                         kFixedPatternCalibrationFrameCount) {
      throw std::length_error("Fixed-pattern calibration is too large");
    }
    frameMedians_.reserve(kFixedPatternCalibrationFrameCount);
    deviations_.reserve(pixelCount * kFixedPatternCalibrationFrameCount);
    frameScratch_.reserve(pixelCount);
  } else if (frame.width != width_ || frame.height != height_) {
    throw std::invalid_argument(
        "Fixed-pattern calibration frame dimensions changed");
  }

  frameScratch_ = frame.celsius;
  const float frameMedian = medianInPlace(frameScratch_);
  frameMedians_.push_back(frameMedian);
  for (float temperature : frame.celsius) {
    deviations_.push_back(temperature - frameMedian);
  }
}

std::size_t FixedPatternCalibrator::frameCount() const noexcept {
  return frameMedians_.size();
}

std::size_t FixedPatternCalibrator::shutterCount() const noexcept {
  return shutterCount_;
}

bool FixedPatternCalibrator::isComplete() const noexcept {
  return frameCount() >= kFixedPatternCalibrationFrameCount &&
         shutterCount_ >= kFixedPatternMinimumShutterCount;
}

FixedPatternCalibrationResult FixedPatternCalibrator::finalize(
    const CameraFingerprint &cameraFingerprint) const {
  FixedPatternCalibrationResult result;
  if (frameCount() < kFixedPatternCalibrationFrameCount) {
    result.error = "Calibration did not collect 96 usable thermal frames.";
    return result;
  }
  if (shutterCount_ < kFixedPatternMinimumShutterCount) {
    std::ostringstream message;
    message << "Calibration observed only " << shutterCount_
            << " shutter refreshes; at least "
            << kFixedPatternMinimumShutterCount
            << " are required. Keep the camera covered and retry.";
    result.error = message.str();
    return result;
  }

  const std::size_t pixelCount = width_ * height_;
  const std::size_t sampleCount = frameCount();
  std::vector<float> stableDeviation(pixelCount);
  std::vector<float> pixelSamples(sampleCount);
  for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
    for (std::size_t sample = 0; sample < sampleCount; ++sample) {
      pixelSamples[sample] = deviations_[sample * pixelCount + pixel];
    }
    stableDeviation[pixel] = medianInPlace(pixelSamples);
  }

  std::vector<float> lowFrequency(pixelCount);
  std::array<float, 25> window{};
  for (std::size_t y = 0; y < height_; ++y) {
    const std::size_t yBegin = y > 2 ? y - 2 : 0;
    const std::size_t yEnd = std::min(height_, y + 3);
    for (std::size_t x = 0; x < width_; ++x) {
      const std::size_t xBegin = x > 2 ? x - 2 : 0;
      const std::size_t xEnd = std::min(width_, x + 3);
      std::size_t count = 0;
      for (std::size_t neighborY = yBegin; neighborY < yEnd; ++neighborY) {
        for (std::size_t neighborX = xBegin; neighborX < xEnd; ++neighborX) {
          window[count++] = stableDeviation[neighborY * width_ + neighborX];
        }
      }
      lowFrequency[y * width_ + x] = medianOfSmallWindow(window, count);
    }
  }

  std::vector<float> bias(pixelCount);
  for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
    bias[pixel] = stableDeviation[pixel] - lowFrequency[pixel];
  }
  std::vector<float> biasForMedian = bias;
  const float biasMedian = medianInPlace(biasForMedian);
  for (float &value : bias) {
    value -= biasMedian;
  }

  std::vector<float> sortedFrameMedians = frameMedians_;
  std::sort(sortedFrameMedians.begin(), sortedFrameMedians.end());
  result.metrics.referenceTemperatureCelsius =
      sortedQuantile(sortedFrameMedians, 0.5);
  result.metrics.globalMedianSpanCelsius =
      sortedFrameMedians.back() - sortedFrameMedians.front();

  std::vector<float> sortedLowFrequency = lowFrequency;
  std::sort(sortedLowFrequency.begin(), sortedLowFrequency.end());
  result.metrics.lowFrequencySpanCelsius =
      sortedQuantile(sortedLowFrequency, 0.95) -
      sortedQuantile(sortedLowFrequency, 0.05);

  std::vector<float> sortedBias = bias;
  std::sort(sortedBias.begin(), sortedBias.end());
  result.metrics.biasP01Celsius = sortedQuantile(sortedBias, 0.01);
  result.metrics.biasP99Celsius = sortedQuantile(sortedBias, 0.99);

  double biasSquares = 0.0;
  float maximumAbsoluteBias = 0.0F;
  for (float value : bias) {
    const double asDouble = static_cast<double>(value);
    biasSquares += asDouble * asDouble;
    maximumAbsoluteBias = std::max(maximumAbsoluteBias, std::abs(value));
  }
  result.metrics.biasRmsCelsius = static_cast<float>(
      std::sqrt(biasSquares / static_cast<double>(pixelCount)));
  result.metrics.maximumAbsoluteBiasCelsius = maximumAbsoluteBias;

  double temporalSquares = 0.0;
  for (std::size_t sample = 0; sample < sampleCount; ++sample) {
    for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
      const double residual = static_cast<double>(
          deviations_[sample * pixelCount + pixel] - stableDeviation[pixel]);
      temporalSquares += residual * residual;
    }
  }
  result.metrics.temporalNoiseRmsCelsius = static_cast<float>(std::sqrt(
      temporalSquares / static_cast<double>(sampleCount * pixelCount)));

  if (result.metrics.globalMedianSpanCelsius >
      kMaximumGlobalMedianSpanCelsius) {
    result.error = metricFailure("Covered-target temperature drift",
                                 result.metrics.globalMedianSpanCelsius,
                                 kMaximumGlobalMedianSpanCelsius);
    return result;
  }
  if (result.metrics.lowFrequencySpanCelsius >
      kMaximumLowFrequencySpanCelsius) {
    result.error = metricFailure("Covered target non-uniformity",
                                 result.metrics.lowFrequencySpanCelsius,
                                 kMaximumLowFrequencySpanCelsius);
    return result;
  }
  if (result.metrics.temporalNoiseRmsCelsius >
      kMaximumTemporalNoiseRmsCelsius) {
    result.error = metricFailure("Calibration temporal noise",
                                 result.metrics.temporalNoiseRmsCelsius,
                                 kMaximumTemporalNoiseRmsCelsius);
    return result;
  }
  if (result.metrics.biasRmsCelsius > kMaximumBiasRmsCelsius) {
    result.error =
        metricFailure("Detector bias RMS", result.metrics.biasRmsCelsius,
                      kMaximumBiasRmsCelsius);
    return result;
  }
  if (result.metrics.maximumAbsoluteBiasCelsius > kMaximumAbsoluteBiasCelsius) {
    result.error = metricFailure("Maximum detector bias",
                                 result.metrics.maximumAbsoluteBiasCelsius,
                                 kMaximumAbsoluteBiasCelsius);
    return result;
  }

  FixedPatternProfile profile;
  profile.cameraFingerprint = cameraFingerprint;
  profile.width = width_;
  profile.height = height_;
  profile.sampleCount = static_cast<std::uint32_t>(sampleCount);
  profile.shutterCount = static_cast<std::uint32_t>(shutterCount_);
  profile.metrics = result.metrics;
  profile.biasCelsius = std::move(bias);

  std::string validationError;
  if (!isValidFixedPatternProfile(profile, &validationError)) {
    result.error =
        "Generated fixed-pattern profile is invalid: " + validationError;
    return result;
  }
  result.profile = std::move(profile);
  return result;
}

bool isValidFixedPatternProfile(const FixedPatternProfile &profile,
                                std::string *error) {
  if (!hasValidProfileMetadata(profile, error)) {
    return false;
  }

  double biasSquares = 0.0;
  float maximumAbsoluteBias = 0.0F;
  for (float bias : profile.biasCelsius) {
    if (!std::isfinite(bias)) {
      setError(error, "Fixed-pattern profile contains a non-finite bias");
      return false;
    }
    const double asDouble = static_cast<double>(bias);
    biasSquares += asDouble * asDouble;
    maximumAbsoluteBias = std::max(maximumAbsoluteBias, std::abs(bias));
  }
  const float calculatedRms = static_cast<float>(
      std::sqrt(biasSquares / static_cast<double>(profile.biasCelsius.size())));
  if (calculatedRms > kMaximumBiasRmsCelsius ||
      maximumAbsoluteBias > kMaximumAbsoluteBiasCelsius) {
    setError(error, "Fixed-pattern profile bias exceeds safety limits");
    return false;
  }

  const float rmsTolerance = std::max(0.001F, calculatedRms * 0.01F);
  const float maximumTolerance = std::max(0.001F, maximumAbsoluteBias * 0.01F);
  if (std::abs(calculatedRms - profile.metrics.biasRmsCelsius) > rmsTolerance ||
      std::abs(maximumAbsoluteBias -
               profile.metrics.maximumAbsoluteBiasCelsius) > maximumTolerance) {
    setError(error, "Fixed-pattern profile bias metrics do not match its data");
    return false;
  }

  if (error != nullptr) {
    error->clear();
  }
  return true;
}

void applyFixedPatternCorrection(const ThermalFrame &cameraFrame,
                                 const FixedPatternProfile &profile,
                                 ThermalFrame &correctedFrame) {
  if (&cameraFrame == &correctedFrame) {
    throw std::invalid_argument(
        "Fixed-pattern correction requires distinct input and output frames");
  }
  std::string profileError;
  if (!hasValidProfileMetadata(profile, &profileError)) {
    throw std::invalid_argument("Fixed-pattern profile is invalid: " +
                                profileError);
  }
  if (!hasValidDimensions(cameraFrame)) {
    throw std::invalid_argument("Camera thermal frame dimensions are invalid");
  }
  if (cameraFrame.width != profile.width ||
      cameraFrame.height != profile.height) {
    throw std::invalid_argument(
        "Fixed-pattern profile does not match the thermal frame dimensions");
  }

  correctedFrame.width = cameraFrame.width;
  correctedFrame.height = cameraFrame.height;
  correctedFrame.celsius.resize(cameraFrame.celsius.size());

  float minimum = std::numeric_limits<float>::infinity();
  float maximum = -std::numeric_limits<float>::infinity();
  std::size_t minimumIndex = 0;
  std::size_t maximumIndex = 0;
  for (std::size_t index = 0; index < cameraFrame.celsius.size(); ++index) {
    const float input = cameraFrame.celsius[index];
    const float bias = profile.biasCelsius[index];
    const float corrected = input - bias;
    if (!std::isfinite(input) || !std::isfinite(bias) ||
        !std::isfinite(corrected)) {
      throw std::invalid_argument(
          "Fixed-pattern correction input contains an invalid value");
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
