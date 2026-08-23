#include "seek_camera_thread.hpp"
#include "seek_compact_usb.hpp"

#include "thermal_processor.hpp"

#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

#include <QMutexLocker>


namespace {
constexpr int kMaxConsecutiveCaptureTimeouts = 3;
constexpr int kMaxUncalibratedNormalFrames = 30;

class ThermalFramePool final
    : public std::enable_shared_from_this<ThermalFramePool> {
public:
  std::shared_ptr<ThermalFrame> acquire() {
    std::unique_ptr<ThermalFrame> frame;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (!available_.empty()) {
        frame = std::move(available_.back());
        available_.pop_back();
      }
    }
    if (!frame) {
      frame = std::make_unique<ThermalFrame>();
    }

    const std::shared_ptr<ThermalFramePool> pool = shared_from_this();
    return std::shared_ptr<ThermalFrame>(
        frame.release(), [pool](ThermalFrame* returnedFrame) noexcept {
          pool->release(returnedFrame);
        });
  }

private:
  void release(ThermalFrame* frame) noexcept {
    try {
      std::unique_ptr<ThermalFrame> returnedFrame(frame);
      const std::lock_guard<std::mutex> lock(mutex_);
      available_.push_back(std::move(returnedFrame));
    } catch (...) {
    }
  }

  std::mutex mutex_;
  std::vector<std::unique_ptr<ThermalFrame>> available_;
};
}  // namespace

SeekCameraThread::SeekCameraThread(QObject* parent) : QThread(parent) {
  qRegisterMetaType<ThermalRenderResult>();
}

SeekCameraThread::~SeekCameraThread() {
  stop();
}

void SeekCameraThread::setRenderSettings(
    const ThermalRenderSettings& settings) {
  const QMutexLocker locker(&renderSettingsMutex_);
  renderSettings_ = settings;
}

ThermalRenderSettings SeekCameraThread::renderSettingsSnapshot() const {
  const QMutexLocker locker(&renderSettingsMutex_);
  return renderSettings_;
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
    const auto framePool = std::make_shared<ThermalFramePool>();
    std::shared_ptr<ThermalFrame> thermalFrame = framePool->acquire();
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

      const bool frameProduced =
          processor.processFrame(rawFrame, *thermalFrame);
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
      const ThermalRenderSettings renderSettings = renderSettingsSnapshot();
      std::shared_ptr<ThermalFrame> correctionBuffer;
      if (!isIdentityRadiometricCorrection(renderSettings.radiometry)) {
        correctionBuffer = framePool->acquire();
      }
      emit frameReady(renderThermalFrame(
          thermalFrame, renderSettings, std::move(correctionBuffer)));
      thermalFrame = framePool->acquire();
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
