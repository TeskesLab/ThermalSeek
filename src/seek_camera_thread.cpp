#include "seek_camera_thread.hpp"
#include "seek_compact_usb.hpp"

#include "thermal_processor.hpp"
#include "thermal_palette.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>


namespace {
constexpr int kMaxConsecutiveCaptureTimeouts = 3;
constexpr int kMaxUncalibratedNormalFrames = 30;


QImage renderFrame(const ThermalFrame& frame) {
  const int sourceWidth = static_cast<int>(frame.width);
  const int sourceHeight = static_cast<int>(frame.height);
  QImage image(sourceHeight, sourceWidth, QImage::Format_RGB32);
  const auto& palette = thermalPalette();

  const float range = frame.maximumCelsius - frame.minimumCelsius;
  if (!std::isfinite(frame.minimumCelsius) ||
      !std::isfinite(frame.maximumCelsius) ||
      range <= std::numeric_limits<float>::epsilon()) {
    image.fill(palette.front());
    return image;
  }

  for (int sourceX = 0; sourceX < sourceWidth; ++sourceX) {
    auto* destination =
        reinterpret_cast<QRgb*>(image.scanLine(sourceWidth - 1 - sourceX));
    for (int sourceY = 0; sourceY < sourceHeight; ++sourceY) {
      const float temperature =
          frame.celsius[static_cast<std::size_t>(sourceY) * frame.width +
                        static_cast<std::size_t>(sourceX)];
      const float normalized =
          std::clamp((temperature - frame.minimumCelsius) / range, 0.0F, 1.0F);
      const std::size_t paletteIndex =
          static_cast<std::size_t>(std::lround(normalized * 255.0F));
      destination[sourceY] = palette[paletteIndex];
    }
  }
  return image;
}
}  // namespace

SeekCameraThread::SeekCameraThread(QObject* parent) : QThread(parent) {}

SeekCameraThread::~SeekCameraThread() {
  stop();
}

void SeekCameraThread::stop() {
  requestInterruption();
  wait();
}

void SeekCameraThread::run() {
  std::optional<SeekCompactUsb> camera;
  bool connected = false;

  const auto disconnectCamera = [&camera, &connected]() noexcept {
    if (!connected || !camera.has_value()) {
      return;
    }

    camera->disconnect();
    connected = false;
  };

  try {
    camera.emplace();
    camera->connect();
    connected = true;

    std::vector<unsigned char> factoryData;
    std::vector<unsigned char> deviceInfo;
    camera->initialize(factoryData, deviceInfo);
    ThermalProcessor processor(factoryData, deviceInfo);

    emit cameraConnected(QString::fromStdString(camera->name()));

    std::vector<unsigned char> rawFrame(
        ThermalProcessor::kRawFrameByteCount);
    ThermalFrame thermalFrame;
    int consecutiveCaptureTimeouts = 0;
    int uncalibratedNormalFrames = 0;
    while (!isInterruptionRequested()) {
      try {
        camera->readFrame(rawFrame);
        consecutiveCaptureTimeouts = 0;
      } catch (const SeekCompactUsbError& error) {
        if (!error.isTimeout() ||
            ++consecutiveCaptureTimeouts > kMaxConsecutiveCaptureTimeouts) {
          throw;
        }
        continue;
      }

      const bool frameProduced = processor.processFrame(rawFrame, thermalFrame);
      if (!frameProduced) {
        if (rawFrame[20] == 3 && !processor.isCalibrated() &&
            ++uncalibratedNormalFrames > kMaxUncalibratedNormalFrames) {
          throw std::runtime_error(
              "Camera calibration stream is incomplete: " +
              processor.missingCalibration());
        }
        continue;
      }

      uncalibratedNormalFrames = 0;
      emit frameReady(renderFrame(thermalFrame), thermalFrame.minimumCelsius,
                      thermalFrame.maximumCelsius,
                      thermalFrame.centerCelsius);
    }

    disconnectCamera();
  } catch (const std::exception& error) {
    disconnectCamera();
    if (!isInterruptionRequested()) {
      emit captureFailed(QString::fromUtf8(error.what()));
    }
  } catch (...) {
    disconnectCamera();
    if (!isInterruptionRequested()) {
      emit captureFailed(QStringLiteral("Unknown camera capture error"));
    }
  }
}
