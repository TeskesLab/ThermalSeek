#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct ThermalFrame final {
  std::size_t width = 0;
  std::size_t height = 0;
  std::vector<float> celsius;
  float minimumCelsius = 0.0F;
  float maximumCelsius = 0.0F;
  float centerCelsius = 0.0F;
};

class ThermalProcessor final {
public:
  static constexpr std::size_t kTransportWidth = 208;
  static constexpr std::size_t kTransportHeight = 156;
  static constexpr std::size_t kDetectorWidth = 206;
  static constexpr std::size_t kDetectorHeight = 156;
  static constexpr std::size_t kRawFrameByteCount =
      kTransportWidth * kTransportHeight * sizeof(std::uint16_t);

  ThermalProcessor(const std::vector<unsigned char>& factoryData,
                   const std::vector<unsigned char>& deviceInfo);

  bool processFrame(const std::vector<unsigned char>& rawFrame,
                    ThermalFrame& output);
  bool isCalibrated() const noexcept;
  std::string missingCalibration() const;

private:
  static constexpr std::size_t kDetectorPixelCount =
      kDetectorWidth * kDetectorHeight;

  void updateFpaReference(std::uint16_t frameType,
                          const std::vector<unsigned char>& rawFrame);
  void storeReferenceFrame(const std::vector<unsigned char>& rawFrame,
                           std::vector<std::uint16_t>& destination);
  void storeBadPixelMap(const std::vector<unsigned char>& rawFrame);
  void storeTlut(const std::vector<unsigned char>& rawFrame);
  void storeSlopeFrame(const std::vector<unsigned char>& rawFrame,
                       std::vector<float>& destination);
  void storeInterceptFrame(const std::vector<unsigned char>& rawFrame,
                           std::vector<float>& destination);
  void processNormalFrame(const std::vector<unsigned char>& rawFrame,
                          ThermalFrame& output);
  std::size_t buildTlutKnots(float* inputKnots, float* outputKnots) const;
  void interpolateBadPixels(ThermalFrame& output);

  float fpaReference_ = 0.0F;
  float alternateTemperatureOffset_ = 0.0F;
  std::uint16_t gainThreshold_ = 0;
  std::uint32_t smoothedFpaQ16_ = 0;
  unsigned int tlutFractionalBits_ = 0;

  std::vector<std::int16_t> tlutAxis_;
  std::vector<std::int16_t> tlutInputTable_;
  std::vector<std::int16_t> tlutOutputTable_;
  std::size_t tlutKnotCount_ = 0;

  std::vector<std::uint16_t> shutterReference_;
  std::vector<std::uint16_t> highGainReference_;
  std::vector<float> lowGainSlope_;
  std::vector<float> lowGainIntercept_;
  std::vector<float> highGainSlope_;
  std::vector<float> highGainIntercept_;
  std::vector<unsigned char> badPixelMask_;
  std::vector<unsigned char> resolvedPixelMask_;
  std::vector<std::size_t> pendingPixelIndices_;
  std::vector<float> pendingPixelValues_;

  bool hasShutterReference_ = false;
  bool hasHighGainReference_ = false;
  bool hasBadPixelMap_ = false;
  bool hasTlut_ = false;
  bool hasLowGainSlope_ = false;
  bool hasLowGainIntercept_ = false;
  bool hasHighGainSlope_ = false;
  bool hasHighGainIntercept_ = false;
};
