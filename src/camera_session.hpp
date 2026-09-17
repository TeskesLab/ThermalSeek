#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "fixed_pattern_correction.hpp"
#include "thermal_frame.hpp"

enum class CameraKind {
  SeekCompact,
  MileseeyTr256i,
};

struct CameraDevice final {
  CameraKind kind = CameraKind::SeekCompact;
  std::string id;
  std::string name;
  std::string devicePath;
  std::uint8_t usbBus = 0;
  std::uint8_t usbAddress = 0;
};

struct CameraSessionInfo final {
  std::string name;
  std::size_t width = 0;
  std::size_t height = 0;
  ThermalOrientation orientation = ThermalOrientation::CounterClockwise;
  std::optional<CameraFingerprint> fixedPatternFingerprint;
};

enum class CameraFrameStatus {
  Ready,
  Calibration,
  Shutter,
  Timeout,
};

class CameraSession {
public:
  virtual ~CameraSession() = default;
  virtual const CameraSessionInfo &info() const noexcept = 0;
  // Every call must return or fail within a bounded interval so capture can stop.
  virtual CameraFrameStatus readFrame(ThermalFrame &frame) = 0;
};

std::vector<CameraDevice> discoverCameras();
std::unique_ptr<CameraSession> openCameraSession(const CameraDevice &device);
