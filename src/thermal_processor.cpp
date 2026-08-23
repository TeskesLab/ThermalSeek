#include "thermal_processor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {
constexpr std::size_t kFactoryMarkerOffset = 0x02;
constexpr std::size_t kGainThresholdOffset = 0x3e;
constexpr std::size_t kThermographyConfigOffset = 0x60;
constexpr std::size_t kFpaReferenceOffset = kThermographyConfigOffset + 0x04;
constexpr std::size_t kAlternateTemperatureOffset =
    kThermographyConfigOffset + 0x48;
constexpr std::size_t kTemperatureModeOffset = 0x440;
constexpr std::size_t kTemperatureFlagsOffset = 0x850;
constexpr std::uint16_t kFactoryMarker = 0xf731;
constexpr std::uint32_t kThermographyV4 = 4;
constexpr std::uint32_t kOldLsmMode = 1;
constexpr std::uint32_t kTlutV4Marker = 0x1a452121;
constexpr std::size_t kTlutMarkerOffset = 0x80;
constexpr std::size_t kTlutAxisCountOffset = 0xe8;
constexpr std::size_t kTlutKnotCountOffset = 0xec;
constexpr std::size_t kTlutFlagsOffset = 0xf0;
constexpr std::size_t kTlutAxisOffset = 0x1c0;
constexpr std::size_t kTlutInputTableOffset = 0x200;
constexpr std::size_t kTlutOutputTableOffset = 0x1200;
constexpr std::size_t kTlutRowStride = 0x40;
constexpr std::size_t kMaximumTlutEntries = 32;
constexpr float kSlopeDivisor = 524288.0F;
constexpr float kInterceptDivisor = 32.0F;
constexpr int kSignalBaseline = 3000;

void requireRange(const std::vector<unsigned char>& data, std::size_t offset,
                  std::size_t size, const char* field) {
  if (offset > data.size() || size > data.size() - offset) {
    throw std::runtime_error(std::string("Camera calibration is missing ") +
                             field);
  }
}

std::uint16_t readU16(const std::vector<unsigned char>& data,
                      std::size_t offset, const char* field) {
  requireRange(data, offset, sizeof(std::uint16_t), field);
  return static_cast<std::uint16_t>(data[offset]) |
         (static_cast<std::uint16_t>(data[offset + 1]) << 8U);
}

std::int16_t readI16(const std::vector<unsigned char>& data,
                     std::size_t offset, const char* field) {
  const std::uint16_t value = readU16(data, offset, field);
  const int signedValue =
      value <= static_cast<std::uint16_t>(std::numeric_limits<std::int16_t>::max())
          ? static_cast<int>(value)
          : static_cast<int>(value) - 0x10000;
  return static_cast<std::int16_t>(signedValue);
}

std::uint32_t readU32(const std::vector<unsigned char>& data,
                      std::size_t offset, const char* field) {
  requireRange(data, offset, sizeof(std::uint32_t), field);
  return static_cast<std::uint32_t>(data[offset]) |
         (static_cast<std::uint32_t>(data[offset + 1]) << 8U) |
         (static_cast<std::uint32_t>(data[offset + 2]) << 16U) |
         (static_cast<std::uint32_t>(data[offset + 3]) << 24U);
}

float readF32(const std::vector<unsigned char>& data, std::size_t offset,
              const char* field) {
  const std::uint32_t bits = readU32(data, offset, field);
  float value = 0.0F;
  static_assert(sizeof(value) == sizeof(bits));
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

std::size_t detectorIndex(std::size_t x, std::size_t y) {
  return y * ThermalProcessor::kDetectorWidth + x;
}

std::uint16_t rawPixel(const std::vector<unsigned char>& rawFrame,
                       std::size_t x, std::size_t y) {
  const std::size_t wordIndex =
      y * ThermalProcessor::kTransportWidth + x;
  return readU16(rawFrame, wordIndex * sizeof(std::uint16_t), "frame data");
}

bool isMetadataPixel(std::size_t x, std::size_t y) {
  const std::size_t metadataColumn = 14 - (((y + 1) * 4) % 15);
  return x % 15 == metadataColumn;
}

std::int64_t floorDividePowerOfTwo(std::int64_t value,
                                   unsigned int shift) {
  const std::int64_t divisor = std::int64_t{1} << shift;
  if (value >= 0) {
    return value / divisor;
  }
  return -((-value + divisor - 1) / divisor);
}

float applyTlut(float value, const float* inputKnots,
                const float* outputKnots, std::size_t knotCount) {
  const float* upperKnot =
      std::upper_bound(inputKnots + 1, inputKnots + knotCount - 1, value);
  const std::size_t segment =
      static_cast<std::size_t>(upperKnot - inputKnots - 1);

  const float inputRange = inputKnots[segment + 1] - inputKnots[segment];
  const float position = (value - inputKnots[segment]) / inputRange;
  return outputKnots[segment] +
         position * (outputKnots[segment + 1] - outputKnots[segment]);
}
}  // namespace

ThermalProcessor::ThermalProcessor(
    const std::vector<unsigned char>& factoryData,
    const std::vector<unsigned char>& deviceInfo)
    : shutterReference_(kDetectorPixelCount, 0),
      highGainReference_(kDetectorPixelCount, 0),
      lowGainSlope_(kDetectorPixelCount, 0.0F),
      lowGainIntercept_(kDetectorPixelCount, 0.0F),
      highGainSlope_(kDetectorPixelCount, 0.0F),
      highGainIntercept_(kDetectorPixelCount, 0.0F),
      badPixelMask_(kDetectorPixelCount, 0),
      resolvedPixelMask_(kDetectorPixelCount, 0) {
  const std::uint16_t transportWidth =
      readU16(deviceInfo, 0x0c, "transport width");
  const std::uint16_t transportHeight =
      readU16(deviceInfo, 0x0e, "transport height");
  const std::uint16_t detectorWidth =
      readU16(deviceInfo, 0x14, "detector width");
  const std::uint16_t detectorHeight =
      readU16(deviceInfo, 0x16, "detector height");
  const std::uint16_t detectorBits =
      readU16(deviceInfo, 0x18, "detector bit depth");

  if (transportWidth != kTransportWidth ||
      transportHeight != kTransportHeight || detectorWidth != kDetectorWidth ||
      detectorHeight != kDetectorHeight || detectorBits != 14) {
    std::ostringstream message;
    message << "Unsupported Seek detector layout: transport " << transportWidth
            << 'x' << transportHeight << ", detector " << detectorWidth << 'x'
            << detectorHeight << ", " << detectorBits << " bits";
    throw std::runtime_error(message.str());
  }

  if (readU16(factoryData, kFactoryMarkerOffset, "factory marker") !=
      kFactoryMarker) {
    throw std::runtime_error("Camera factory calibration marker is invalid");
  }
  if (readU32(factoryData, kThermographyConfigOffset,
              "thermography algorithm") != kThermographyV4) {
    throw std::runtime_error(
        "This camera's thermography algorithm is not supported");
  }
  if (readU32(factoryData, kTemperatureModeOffset, "temperature mode") !=
      kOldLsmMode) {
    throw std::runtime_error(
        "This camera's signal-correction mode is not supported");
  }
  const std::uint32_t temperatureFlags =
      readU32(factoryData, kTemperatureFlagsOffset, "temperature flags");
  if (((temperatureFlags >> 1U) & 5U) != 0U) {
    throw std::runtime_error(
        "This camera's signal-correction flags are not supported");
  }

  gainThreshold_ =
      readU16(factoryData, kGainThresholdOffset, "gain threshold");
  fpaReference_ = readF32(factoryData, kFpaReferenceOffset, "FPA reference");
  alternateTemperatureOffset_ = readF32(
      factoryData, kAlternateTemperatureOffset, "alternate gain offset");
  if (gainThreshold_ == 0 || !std::isfinite(fpaReference_) ||
      !std::isfinite(alternateTemperatureOffset_)) {
    throw std::runtime_error("Camera factory temperature calibration is invalid");
  }

  for (std::size_t y = 0; y < kDetectorHeight; ++y) {
    for (std::size_t x = 0; x < kDetectorWidth; ++x) {
      badPixelMask_[detectorIndex(x, y)] =
          static_cast<unsigned char>(isMetadataPixel(x, y));
    }
  }
  pendingPixelIndices_.reserve(kDetectorPixelCount);
  pendingPixelValues_.reserve(kDetectorPixelCount);
}

bool ThermalProcessor::processFrame(
    const std::vector<unsigned char>& rawFrame, ThermalFrame& output) {
  if (rawFrame.size() != kRawFrameByteCount) {
    throw std::runtime_error("Camera returned an incomplete thermal frame");
  }

  const std::uint16_t frameType =
      readU16(rawFrame, 10 * sizeof(std::uint16_t), "frame type");
  updateFpaReference(frameType, rawFrame);

  switch (frameType) {
  case 1:
  case 8:
    storeReferenceFrame(rawFrame, shutterReference_);
    hasShutterReference_ = true;
    break;
  case 3:
    if (isCalibrated()) {
      processNormalFrame(rawFrame, output);
      return true;
    }
    break;
  case 4:
    storeBadPixelMap(rawFrame);
    hasBadPixelMap_ = true;
    break;
  case 7:
    storeReferenceFrame(rawFrame, highGainReference_);
    hasHighGainReference_ = true;
    break;
  case 14:
    storeTlut(rawFrame);
    hasTlut_ = true;
    break;
  case 25:
    storeSlopeFrame(rawFrame, lowGainSlope_);
    hasLowGainSlope_ = true;
    break;
  case 26:
    storeInterceptFrame(rawFrame, lowGainIntercept_);
    hasLowGainIntercept_ = true;
    break;
  case 27:
    storeSlopeFrame(rawFrame, highGainSlope_);
    hasHighGainSlope_ = true;
    break;
  case 28:
    storeInterceptFrame(rawFrame, highGainIntercept_);
    hasHighGainIntercept_ = true;
    break;
  default:
    break;
  }

  return false;
}

bool ThermalProcessor::isCalibrated() const noexcept {
  return hasShutterReference_ && hasHighGainReference_ && hasBadPixelMap_ &&
         hasTlut_ && hasLowGainSlope_ && hasLowGainIntercept_ &&
         hasHighGainSlope_ && hasHighGainIntercept_;
}

std::string ThermalProcessor::missingCalibration() const {
  std::ostringstream missing;
  const auto append = [&missing](const char* name) {
    if (missing.tellp() > 0) {
      missing << ", ";
    }
    missing << name;
  };

  if (!hasShutterReference_) {
    append("shutter reference");
  }
  if (!hasHighGainReference_) {
    append("high-gain reference");
  }
  if (!hasBadPixelMap_) {
    append("bad-pixel map");
  }
  if (!hasTlut_) {
    append("temperature lookup table");
  }
  if (!hasLowGainSlope_ || !hasLowGainIntercept_) {
    append("low-gain coefficients");
  }
  if (!hasHighGainSlope_ || !hasHighGainIntercept_) {
    append("high-gain coefficients");
  }
  return missing.str();
}

void ThermalProcessor::updateFpaReference(
    std::uint16_t frameType,
    const std::vector<unsigned char>& rawFrame) {
  if (frameType != 1 && frameType != 3 && frameType != 6 && frameType != 8) {
    return;
  }

  const std::uint32_t currentFpaQ16 =
      static_cast<std::uint32_t>(rawPixel(rawFrame, 1, 0)) << 16U;
  if (smoothedFpaQ16_ == 0) {
    smoothedFpaQ16_ = currentFpaQ16;
    return;
  }

  smoothedFpaQ16_ = static_cast<std::uint32_t>(
      (static_cast<std::uint64_t>(smoothedFpaQ16_) * 7U +
       (static_cast<std::uint64_t>(currentFpaQ16) | 4U)) >>
      3U);
}

void ThermalProcessor::storeReferenceFrame(
    const std::vector<unsigned char>& rawFrame,
    std::vector<std::uint16_t>& destination) {
  for (std::size_t y = 0; y < kDetectorHeight; ++y) {
    for (std::size_t x = 0; x < kDetectorWidth; ++x) {
      destination[detectorIndex(x, y)] = rawPixel(rawFrame, x, y);
    }
  }
}

void ThermalProcessor::storeBadPixelMap(
    const std::vector<unsigned char>& rawFrame) {
  for (std::size_t y = 0; y < kDetectorHeight; ++y) {
    for (std::size_t x = 0; x < kDetectorWidth; ++x) {
      const std::size_t index = detectorIndex(x, y);
      badPixelMask_[index] = static_cast<unsigned char>(
          isMetadataPixel(x, y) || rawPixel(rawFrame, x, y) == 0);
    }
  }
}

void ThermalProcessor::storeTlut(
    const std::vector<unsigned char>& rawFrame) {
  if (readU32(rawFrame, kTlutMarkerOffset, "TLUT marker") !=
      kTlutV4Marker) {
    throw std::runtime_error("Camera temperature lookup table is unsupported");
  }

  const std::size_t axisCount =
      readU32(rawFrame, kTlutAxisCountOffset, "TLUT axis count");
  const std::size_t knotCount =
      readU32(rawFrame, kTlutKnotCountOffset, "TLUT knot count");
  if (axisCount < 2 || axisCount > kMaximumTlutEntries || knotCount < 2 ||
      knotCount > kMaximumTlutEntries) {
    throw std::runtime_error("Camera temperature lookup table dimensions are invalid");
  }

  const unsigned int fractionalReduction =
      readU16(rawFrame, kTlutFlagsOffset, "TLUT flags") & 0x0fU;
  if (fractionalReduction > 5U) {
    throw std::runtime_error("Camera temperature lookup table precision is invalid");
  }
  tlutFractionalBits_ = 5U - fractionalReduction;

  requireRange(rawFrame, kTlutAxisOffset,
               axisCount * sizeof(std::int16_t), "TLUT axis");
  requireRange(rawFrame,
               kTlutInputTableOffset + (knotCount - 1) * kTlutRowStride,
               axisCount * sizeof(std::int16_t), "TLUT input table");
  requireRange(rawFrame,
               kTlutOutputTableOffset + (knotCount - 1) * kTlutRowStride,
               axisCount * sizeof(std::int16_t), "TLUT output table");

  tlutAxis_.resize(axisCount);
  for (std::size_t column = 0; column < axisCount; ++column) {
    tlutAxis_[column] = readI16(
        rawFrame, kTlutAxisOffset + column * sizeof(std::int16_t),
        "TLUT axis");
  }
  const bool ascending = tlutAxis_.front() < tlutAxis_.back();
  for (std::size_t column = 1; column < axisCount; ++column) {
    if ((ascending && tlutAxis_[column - 1] >= tlutAxis_[column]) ||
        (!ascending && tlutAxis_[column - 1] <= tlutAxis_[column])) {
      throw std::runtime_error("Camera temperature lookup axis is not monotonic");
    }
  }

  tlutInputTable_.resize(knotCount * axisCount);
  tlutOutputTable_.resize(knotCount * axisCount);
  for (std::size_t row = 0; row < knotCount; ++row) {
    for (std::size_t column = 0; column < axisCount; ++column) {
      const std::size_t tableIndex = row * axisCount + column;
      const std::size_t rowOffset = row * kTlutRowStride;
      const std::size_t columnOffset = column * sizeof(std::int16_t);
      tlutInputTable_[tableIndex] = readI16(
          rawFrame, kTlutInputTableOffset + rowOffset + columnOffset,
          "TLUT input table");
      tlutOutputTable_[tableIndex] = readI16(
          rawFrame, kTlutOutputTableOffset + rowOffset + columnOffset,
          "TLUT output table");
    }
  }
  tlutKnotCount_ = knotCount;
}

void ThermalProcessor::storeSlopeFrame(
    const std::vector<unsigned char>& rawFrame,
    std::vector<float>& destination) {
  for (std::size_t y = 0; y < kDetectorHeight; ++y) {
    for (std::size_t x = 0; x < kDetectorWidth; ++x) {
      destination[detectorIndex(x, y)] =
          static_cast<float>(rawPixel(rawFrame, x, y)) / kSlopeDivisor;
    }
  }
}

void ThermalProcessor::storeInterceptFrame(
    const std::vector<unsigned char>& rawFrame,
    std::vector<float>& destination) {
  for (std::size_t y = 0; y < kDetectorHeight; ++y) {
    for (std::size_t x = 0; x < kDetectorWidth; ++x) {
      const std::uint16_t raw = rawPixel(rawFrame, x, y);
      const int signedValue = raw <= 0x7fffU ? static_cast<int>(raw)
                                            : static_cast<int>(raw) - 0x10000;
      destination[detectorIndex(x, y)] =
          static_cast<float>(signedValue) / kInterceptDivisor;
    }
  }
}

void ThermalProcessor::processNormalFrame(
    const std::vector<unsigned char>& rawFrame, ThermalFrame& output) {
  std::array<float, kMaximumTlutEntries> inputKnots{};
  std::array<float, kMaximumTlutEntries> outputKnots{};
  const std::size_t knotCount =
      buildTlutKnots(inputKnots.data(), outputKnots.data());

  output.width = kDetectorWidth;
  output.height = kDetectorHeight;
  output.celsius.resize(kDetectorPixelCount);
  std::fill(resolvedPixelMask_.begin(), resolvedPixelMask_.end(), 0);

  for (std::size_t y = 0; y < kDetectorHeight; ++y) {
    for (std::size_t x = 0; x < kDetectorWidth; ++x) {
      const std::size_t index = detectorIndex(x, y);
      const std::uint16_t raw = rawPixel(rawFrame, x, y);
      const bool highGain = raw >= gainThreshold_;
      const std::uint16_t reference =
          highGain ? highGainReference_[index] : shutterReference_[index];
      const float slope =
          highGain ? highGainSlope_[index] : lowGainSlope_[index];
      const float intercept =
          highGain ? alternateTemperatureOffset_ + highGainIntercept_[index]
                   : lowGainIntercept_[index];

      if (badPixelMask_[index] != 0 || raw == 0 || reference == 0 ||
          !std::isfinite(slope) || slope <= 0.0F ||
          !std::isfinite(intercept)) {
        continue;
      }

      const int correctedSignal = std::clamp(
          highGain ? static_cast<int>(gainThreshold_) + static_cast<int>(raw) -
                         static_cast<int>(reference)
                   : static_cast<int>(raw) - static_cast<int>(reference) +
                         kSignalBaseline,
          0, static_cast<int>(std::numeric_limits<std::uint16_t>::max()));
      const float temperature =
          intercept + slope * static_cast<float>(correctedSignal - kSignalBaseline);
      const float mappedTemperature = applyTlut(
          temperature, inputKnots.data(), outputKnots.data(), knotCount);
      if (!std::isfinite(mappedTemperature)) {
        continue;
      }

      output.celsius[index] = mappedTemperature;
      resolvedPixelMask_[index] = 1;
    }
  }

  interpolateBadPixels(output);

  const auto extrema =
      std::minmax_element(output.celsius.begin(), output.celsius.end());
  output.minimumCelsius = *extrema.first;
  output.maximumCelsius = *extrema.second;
  output.centerCelsius =
      output.celsius[detectorIndex(kDetectorWidth / 2, kDetectorHeight / 2)];
}

std::size_t ThermalProcessor::buildTlutKnots(float* inputKnots,
                                             float* outputKnots) const {
  if (smoothedFpaQ16_ == 0 || tlutAxis_.size() < 2 || tlutKnotCount_ < 2) {
    throw std::runtime_error("Camera temperature lookup state is incomplete");
  }

  const std::int64_t fpaReferenceQ16 =
      std::llround(static_cast<double>(fpaReference_) * 65536.0);
  const std::int64_t fpaMarkQ16 =
      static_cast<std::int64_t>(smoothedFpaQ16_) - fpaReferenceQ16;
  const unsigned int axisShift = 16U - tlutFractionalBits_;
  const std::int64_t roundedMark = floorDividePowerOfTwo(
      fpaMarkQ16 + (std::int64_t{1} << (axisShift - 1U)), axisShift);

  const bool ascending = tlutAxis_.front() < tlutAxis_.back();
  std::size_t axisIndex = 0;
  if (ascending) {
    if (roundedMark >= tlutAxis_.back()) {
      axisIndex = tlutAxis_.size() - 2;
    } else if (roundedMark > tlutAxis_.front()) {
      while (axisIndex + 1 < tlutAxis_.size() - 1 &&
             roundedMark > tlutAxis_[axisIndex + 1]) {
        ++axisIndex;
      }
    }
  } else {
    if (roundedMark <= tlutAxis_.back()) {
      axisIndex = tlutAxis_.size() - 2;
    } else if (roundedMark < tlutAxis_.front()) {
      while (axisIndex + 1 < tlutAxis_.size() - 1 &&
             roundedMark < tlutAxis_[axisIndex + 1]) {
        ++axisIndex;
      }
    }
  }

  const std::int64_t axisScale = std::int64_t{1} << axisShift;
  const std::int64_t firstAxisQ16 =
      static_cast<std::int64_t>(tlutAxis_[axisIndex]) * axisScale;
  const std::int64_t secondAxisQ16 =
      static_cast<std::int64_t>(tlutAxis_[axisIndex + 1]) * axisScale;
  const double axisPosition = std::clamp(
      static_cast<double>(fpaMarkQ16 - firstAxisQ16) /
          static_cast<double>(secondAxisQ16 - firstAxisQ16),
      0.0, 1.0);
  const float tableScale =
      static_cast<float>(std::uint32_t{1} << tlutFractionalBits_);

  std::size_t compactedCount = 0;
  for (std::size_t row = 0; row < tlutKnotCount_; ++row) {
    const std::size_t firstIndex = row * tlutAxis_.size() + axisIndex;
    const std::size_t secondIndex = firstIndex + 1;
    const float input =
        (static_cast<float>(tlutInputTable_[firstIndex]) +
         static_cast<float>(axisPosition) *
             static_cast<float>(tlutInputTable_[secondIndex] -
                                tlutInputTable_[firstIndex])) /
        tableScale;
    const float output =
        (static_cast<float>(tlutOutputTable_[firstIndex]) +
         static_cast<float>(axisPosition) *
             static_cast<float>(tlutOutputTable_[secondIndex] -
                                tlutOutputTable_[firstIndex])) /
        tableScale;

    if (compactedCount > 0 && input <= inputKnots[compactedCount - 1]) {
      if (std::abs(input - inputKnots[compactedCount - 1]) < 0.0001F) {
        outputKnots[compactedCount - 1] = output;
        continue;
      }
      throw std::runtime_error(
          "Camera temperature lookup table became non-monotonic");
    }
    inputKnots[compactedCount] = input;
    outputKnots[compactedCount] = output;
    ++compactedCount;
  }

  if (compactedCount < 2) {
    throw std::runtime_error("Camera temperature lookup table has no usable range");
  }
  return compactedCount;
}

void ThermalProcessor::interpolateBadPixels(ThermalFrame& output) {
  std::size_t unresolved = static_cast<std::size_t>(std::count(
      resolvedPixelMask_.begin(), resolvedPixelMask_.end(),
      static_cast<unsigned char>(0)));

  const auto collectRepairs = [this, &output](std::size_t minimumNeighbors) {
    pendingPixelIndices_.clear();
    pendingPixelValues_.clear();
    for (std::size_t y = 0; y < kDetectorHeight; ++y) {
      for (std::size_t x = 0; x < kDetectorWidth; ++x) {
        const std::size_t index = detectorIndex(x, y);
        if (resolvedPixelMask_[index] != 0) {
          continue;
        }

        float weightedSum = 0.0F;
        float totalWeight = 0.0F;
        std::size_t neighborCount = 0;
        for (int yOffset = -1; yOffset <= 1; ++yOffset) {
          for (int xOffset = -1; xOffset <= 1; ++xOffset) {
            if (xOffset == 0 && yOffset == 0) {
              continue;
            }
            const int neighborX = static_cast<int>(x) + xOffset;
            const int neighborY = static_cast<int>(y) + yOffset;
            if (neighborX < 0 || neighborY < 0 ||
                neighborX >= static_cast<int>(kDetectorWidth) ||
                neighborY >= static_cast<int>(kDetectorHeight)) {
              continue;
            }
            const std::size_t neighborIndex = detectorIndex(
                static_cast<std::size_t>(neighborX),
                static_cast<std::size_t>(neighborY));
            if (resolvedPixelMask_[neighborIndex] == 0) {
              continue;
            }
            const float weight =
                xOffset == 0 || yOffset == 0 ? 1.0F : 0.70710678F;
            weightedSum += output.celsius[neighborIndex] * weight;
            totalWeight += weight;
            ++neighborCount;
          }
        }

        if (neighborCount >= minimumNeighbors) {
          pendingPixelIndices_.push_back(index);
          pendingPixelValues_.push_back(weightedSum / totalWeight);
        }
      }
    }
  };

  while (unresolved != 0) {
    collectRepairs(2);
    if (pendingPixelIndices_.empty()) {
      collectRepairs(1);
    }
    if (pendingPixelIndices_.empty()) {
      throw std::runtime_error("Camera frame contains an unrepairable bad-pixel region");
    }

    for (std::size_t repair = 0; repair < pendingPixelIndices_.size(); ++repair) {
      const std::size_t index = pendingPixelIndices_[repair];
      output.celsius[index] = pendingPixelValues_[repair];
      resolvedPixelMask_[index] = 1;
    }
    unresolved -= pendingPixelIndices_.size();
  }
}
