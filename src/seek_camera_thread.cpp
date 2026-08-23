#include "seek_camera_thread.hpp"
#include "fixed_pattern_correction.hpp"
#include "fixed_pattern_profile_store.hpp"
#include "seek_compact_usb.hpp"

#include "thermal_processor.hpp"

#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <QMutexLocker>

namespace {
constexpr int kMaxConsecutiveCaptureTimeouts = 3;
constexpr int kMaxUncalibratedNormalFrames = 30;

std::uint16_t rawFrameType(const std::vector<unsigned char> &rawFrame) {
  if (rawFrame.size() <= 21) {
    throw std::runtime_error("Camera returned an incomplete frame header");
  }
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(rawFrame[20]) |
      (static_cast<std::uint16_t>(rawFrame[21]) << 8U));
}

QString calibrationSummary(const FixedPatternProfile &profile) {
  return QStringLiteral(
             "Fixed-pattern profile saved. Bias RMS %1 °C; temporal noise "
             "%2 °C; covered-target span %3 °C; %4 shutter refreshes.")
      .arg(profile.metrics.biasRmsCelsius, 0, 'f', 3)
      .arg(profile.metrics.temporalNoiseRmsCelsius, 0, 'f', 3)
      .arg(profile.metrics.lowFrequencySpanCelsius, 0, 'f', 3)
      .arg(profile.shutterCount);
}

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
        frame.release(), [pool](ThermalFrame *returnedFrame) noexcept {
          pool->release(returnedFrame);
        });
  }

private:
  void release(ThermalFrame *frame) noexcept {
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
} // namespace

SeekCameraThread::SeekCameraThread(QObject *parent) : QThread(parent) {
  qRegisterMetaType<ThermalRenderResult>();
}

SeekCameraThread::~SeekCameraThread() { stop(); }

void SeekCameraThread::setRenderSettings(
    const ThermalRenderSettings &settings) {
  const QMutexLocker locker(&renderSettingsMutex_);
  renderSettings_ = settings;
}

void SeekCameraThread::setFixedPatternEnabled(bool enabled) {
  const QMutexLocker locker(&fixedPatternMutex_);
  fixedPatternEnabled_ = enabled;
}

void SeekCameraThread::requestFixedPatternCalibration() {
  const QMutexLocker locker(&fixedPatternMutex_);
  fixedPatternCalibrationRequested_ = true;
  fixedPatternCalibrationCancellationRequested_ = false;
}

void SeekCameraThread::cancelFixedPatternCalibration() {
  const QMutexLocker locker(&fixedPatternMutex_);
  fixedPatternCalibrationRequested_ = false;
  fixedPatternCalibrationCancellationRequested_ = true;
}

void SeekCameraThread::requestFixedPatternProfileDeletion() {
  const QMutexLocker locker(&fixedPatternMutex_);
  fixedPatternCalibrationRequested_ = false;
  fixedPatternCalibrationCancellationRequested_ = false;
  fixedPatternProfileDeletionRequested_ = true;
}

SeekCameraThread::FixedPatternCommands
SeekCameraThread::takeFixedPatternCommands() {
  const QMutexLocker locker(&fixedPatternMutex_);
  FixedPatternCommands commands;
  commands.enabled = fixedPatternEnabled_;
  commands.startCalibration = fixedPatternCalibrationRequested_;
  commands.cancelCalibration = fixedPatternCalibrationCancellationRequested_;
  commands.deleteProfile = fixedPatternProfileDeletionRequested_;
  fixedPatternCalibrationRequested_ = false;
  fixedPatternCalibrationCancellationRequested_ = false;
  fixedPatternProfileDeletionRequested_ = false;
  return commands;
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

    const CameraFingerprint cameraFingerprint =
        computeCameraFingerprint(factoryData, deviceInfo);
    const QString profilePath = fixedPatternProfilePath(cameraFingerprint);
    std::optional<FixedPatternProfile> fixedPatternProfile;
    QString initialProfileMessage;
    FixedPatternProfileLoadResult profileLoad =
        loadFixedPatternProfile(profilePath, cameraFingerprint);
    if (profileLoad.status == FixedPatternProfileLoadStatus::Loaded &&
        profileLoad.profile.has_value()) {
      if (profileLoad.profile->width == ThermalProcessor::kDetectorWidth &&
          profileLoad.profile->height == ThermalProcessor::kDetectorHeight) {
        fixedPatternProfile = std::move(*profileLoad.profile);
      } else {
        initialProfileMessage = QStringLiteral(
            "Fixed-pattern profile was ignored because its dimensions do "
            "not match this camera.");
      }
    } else if (profileLoad.status == FixedPatternProfileLoadStatus::Invalid) {
      initialProfileMessage =
          QStringLiteral("Fixed-pattern profile was ignored: %1")
              .arg(profileLoad.error);
    }

    emit cameraConnected(QString::fromStdString(camera->name()));

    std::vector<unsigned char> rawFrame(ThermalProcessor::kRawFrameByteCount);
    const auto framePool = std::make_shared<ThermalFramePool>();
    std::shared_ptr<ThermalFrame> thermalFrame = framePool->acquire();
    std::optional<FixedPatternCalibrator> fixedPatternCalibrator;
    bool fixedPatternEnabled = true;
    bool initialProfileStatePending = true;
    int consecutiveCaptureTimeouts = 0;
    int uncalibratedNormalFrames = 0;
    while (!isInterruptionRequested()) {
      try {
        camera->readFrame(rawFrame);
        consecutiveCaptureTimeouts = 0;
      } catch (const SeekCompactUsbError &error) {
        if (!error.isTimeout() ||
            ++consecutiveCaptureTimeouts > kMaxConsecutiveCaptureTimeouts) {
          throw;
        }
        continue;
      }

      const std::uint16_t frameType = rawFrameType(rawFrame);
      const FixedPatternCommands commands = takeFixedPatternCommands();
      const bool enabledChanged = fixedPatternEnabled != commands.enabled;
      fixedPatternEnabled = commands.enabled;

      bool profileStateChanged = initialProfileStatePending || enabledChanged;
      QString profileStateMessage =
          initialProfileStatePending ? initialProfileMessage : QString{};
      initialProfileStatePending = false;

      if (commands.deleteProfile) {
        if (fixedPatternCalibrator.has_value()) {
          fixedPatternCalibrator.reset();
          emit fixedPatternCalibrationFinished(
              false, true,
              QStringLiteral(
                  "Fixed-pattern calibration canceled because the profile "
                  "was deleted."));
        }

        QString deletionError;
        if (removeFixedPatternProfile(profilePath, &deletionError)) {
          fixedPatternProfile.reset();
          profileStateMessage =
              QStringLiteral("Fixed-pattern profile deleted.");
        } else {
          profileStateMessage =
              QStringLiteral("Could not delete fixed-pattern profile: %1")
                  .arg(deletionError);
        }
        profileStateChanged = true;
      } else {
        if (commands.cancelCalibration) {
          fixedPatternCalibrator.reset();
          emit fixedPatternCalibrationFinished(
              false, true,
              QStringLiteral("Fixed-pattern calibration canceled."));
        }
        if (commands.startCalibration && !commands.cancelCalibration) {
          fixedPatternCalibrator.emplace();
          emit fixedPatternCalibrationProgress(
              0, static_cast<int>(kFixedPatternCalibrationFrameCount), 0);
        }
      }

      if (profileStateChanged) {
        emit fixedPatternProfileStateChanged(
            fixedPatternProfile.has_value(),
            fixedPatternEnabled && fixedPatternProfile.has_value(),
            profileStateMessage);
      }

      if (fixedPatternCalibrator.has_value() &&
          (frameType == 1 || frameType == 8)) {
        fixedPatternCalibrator->noteShutterFrame();
        emit fixedPatternCalibrationProgress(
            static_cast<int>(fixedPatternCalibrator->frameCount()),
            static_cast<int>(kFixedPatternCalibrationFrameCount),
            static_cast<int>(fixedPatternCalibrator->shutterCount()));
      }

      const bool frameProduced =
          processor.processFrame(rawFrame, *thermalFrame);
      if (!frameProduced) {
        if (frameType == 3 && !processor.isCalibrated() &&
            ++uncalibratedNormalFrames > kMaxUncalibratedNormalFrames) {
          throw std::runtime_error("Camera calibration stream is incomplete: " +
                                   processor.missingCalibration());
        }
        continue;
      }

      uncalibratedNormalFrames = 0;
      if (fixedPatternCalibrator.has_value()) {
        if (fixedPatternCalibrator->frameCount() <
            kFixedPatternCalibrationFrameCount) {
          fixedPatternCalibrator->addFrame(*thermalFrame);
          emit fixedPatternCalibrationProgress(
              static_cast<int>(fixedPatternCalibrator->frameCount()),
              static_cast<int>(kFixedPatternCalibrationFrameCount),
              static_cast<int>(fixedPatternCalibrator->shutterCount()));
        }

        if (fixedPatternCalibrator->isComplete()) {
          FixedPatternCalibrationResult calibrationResult =
              fixedPatternCalibrator->finalize(cameraFingerprint);
          fixedPatternCalibrator.reset();
          if (!calibrationResult.profile.has_value()) {
            emit fixedPatternCalibrationFinished(
                false, false, QString::fromStdString(calibrationResult.error));
          } else {
            QString saveError;
            if (!saveFixedPatternProfile(*calibrationResult.profile,
                                         profilePath, &saveError)) {
              emit fixedPatternCalibrationFinished(
                  false, false,
                  QStringLiteral(
                      "Calibration passed validation but could not be "
                      "saved: %1")
                      .arg(saveError));
            } else {
              fixedPatternProfile = std::move(*calibrationResult.profile);
              emit fixedPatternProfileStateChanged(true, fixedPatternEnabled,
                                                   QString{});
              emit fixedPatternCalibrationFinished(
                  true, false, calibrationSummary(*fixedPatternProfile));
            }
          }
        }
      }

      const ThermalRenderSettings renderSettings = renderSettingsSnapshot();
      std::shared_ptr<const ThermalFrame> cameraFrame = thermalFrame;
      std::shared_ptr<const ThermalFrame> apparentFrame = cameraFrame;
      if (fixedPatternEnabled && fixedPatternProfile.has_value()) {
        std::shared_ptr<ThermalFrame> fixedPatternBuffer = framePool->acquire();
        applyFixedPatternCorrection(*cameraFrame, *fixedPatternProfile,
                                    *fixedPatternBuffer);
        apparentFrame = std::move(fixedPatternBuffer);
      }

      std::shared_ptr<ThermalFrame> radiometricCorrectionBuffer;
      if (!isIdentityRadiometricCorrection(renderSettings.radiometry)) {
        radiometricCorrectionBuffer = framePool->acquire();
      }
      emit frameReady(renderThermalFrame(
          std::move(cameraFrame), std::move(apparentFrame), renderSettings,
          std::move(radiometricCorrectionBuffer)));
      thermalFrame = framePool->acquire();
    }

    disconnectCamera();
  } catch (const std::exception &error) {
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
