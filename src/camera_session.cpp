#include "camera_session.hpp"

#include "fixed_pattern_profile_store.hpp"
#include "seek_compact_usb.hpp"
#include "thermal_processor.hpp"
#include "tr256i_camera.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <utility>

#include <libusb.h>

namespace {
class SeekCameraSession final : public CameraSession {
public:
  explicit SeekCameraSession(const CameraDevice &device) {
    camera_.connect(device.usbBus, device.usbAddress);
    std::vector<unsigned char> factoryData;
    std::vector<unsigned char> deviceInfo;
    camera_.initialize(factoryData, deviceInfo);
    processor_.emplace(factoryData, deviceInfo);
    info_.name = camera_.name();
    info_.width = ThermalProcessor::kDetectorWidth;
    info_.height = ThermalProcessor::kDetectorHeight;
    info_.orientation = ThermalOrientation::CounterClockwise;
    info_.fixedPatternFingerprint = computeCameraFingerprint(factoryData, deviceInfo);
    rawFrame_.resize(ThermalProcessor::kRawFrameByteCount);
  }

  const CameraSessionInfo &info() const noexcept override { return info_; }

  CameraFrameStatus readFrame(ThermalFrame &frame) override {
    try {
      camera_.readFrame(rawFrame_);
    } catch (const SeekCompactUsbError &error) {
      if (error.isTimeout()) {
        return CameraFrameStatus::Timeout;
      }
      throw;
    }
    if (rawFrame_.size() != ThermalProcessor::kRawFrameByteCount) {
      throw std::runtime_error("Seek Compact returned an incomplete frame");
    }
    const std::uint16_t type = static_cast<std::uint16_t>(
        rawFrame_[20] | (static_cast<std::uint16_t>(rawFrame_[21]) << 8U));
    if (processor_->processFrame(rawFrame_, frame)) {
      uncalibratedNormalFrames_ = 0;
      return CameraFrameStatus::Ready;
    }
    if (type == 3 && !processor_->isCalibrated() &&
        ++uncalibratedNormalFrames_ > 30) {
      throw std::runtime_error("Camera calibration stream is incomplete: " +
                               processor_->missingCalibration());
    }
    return type == 1 || type == 8 ? CameraFrameStatus::Shutter
                                  : CameraFrameStatus::Calibration;
  }

private:
  SeekCompactUsb camera_;
  std::optional<ThermalProcessor> processor_;
  std::vector<unsigned char> rawFrame_;
  CameraSessionInfo info_;
  int uncalibratedNormalFrames_ = 0;
};
} // namespace

std::vector<CameraDevice> discoverCameras() {
  libusb_context *rawContext = nullptr;
  const int initialized = libusb_init(&rawContext);
  if (initialized != LIBUSB_SUCCESS) {
    throw SeekCompactUsbError("Discovering cameras", initialized);
  }
  const std::unique_ptr<libusb_context, decltype(&libusb_exit)> context(
      rawContext, &libusb_exit);
  libusb_device **rawDevices = nullptr;
  const ssize_t count = libusb_get_device_list(context.get(), &rawDevices);
  if (count < 0) {
    throw SeekCompactUsbError("Enumerating cameras", static_cast<int>(count));
  }
  const auto freeDevices = [](libusb_device **devices) {
    libusb_free_device_list(devices, 1);
  };
  const std::unique_ptr<libusb_device *, decltype(freeDevices)> devices(
      rawDevices, freeDevices);
  std::vector<CameraDevice> result;
  for (ssize_t index = 0; index < count; ++index) {
    libusb_device_descriptor descriptor{};
    if (libusb_get_device_descriptor(devices.get()[index], &descriptor) !=
            LIBUSB_SUCCESS ||
        descriptor.idVendor != 0x289d || descriptor.idProduct != 0x0010) {
      continue;
    }
    CameraDevice device;
    device.kind = CameraKind::SeekCompact;
    device.usbBus = libusb_get_bus_number(devices.get()[index]);
    device.usbAddress = libusb_get_device_address(devices.get()[index]);
    const std::string location = std::to_string(device.usbBus) + ":" +
                                 std::to_string(device.usbAddress);
    device.id = "seek:" + location;
    device.name = "Seek Compact / PIR206 (USB " + location + ")";
    result.push_back(std::move(device));
  }
  std::vector<CameraDevice> uvcCameras = discoverTr256iCameras();
  result.insert(result.end(), std::make_move_iterator(uvcCameras.begin()),
                std::make_move_iterator(uvcCameras.end()));
  std::sort(result.begin(), result.end(), [](const CameraDevice &a,
                                           const CameraDevice &b) {
    return a.id < b.id;
  });
  return result;
}

std::unique_ptr<CameraSession> openCameraSession(const CameraDevice &device) {
  switch (device.kind) {
  case CameraKind::SeekCompact:
    return std::make_unique<SeekCameraSession>(device);
  case CameraKind::MileseeyTr256i:
    return openTr256iCamera(device);
  }
  throw std::runtime_error("Unsupported camera model");
}
