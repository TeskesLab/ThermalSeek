#include "camera_thread.hpp"
#include "fixed_pattern_correction.hpp"
#include "fixed_pattern_profile_store.hpp"

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

CameraThread::CameraThread(QObject *parent) : QThread(parent) {
  qRegisterMetaType<ThermalRenderResult>();
}

CameraThread::~CameraThread() { stop(); }

void CameraThread::startCamera(const CameraDevice &device) {
  stop();
  device_ = device;
  {
    const QMutexLocker locker(&fixedPatternMutex_);
    fixedPatternCalibrationRequested_ = false;
    fixedPatternCalibrationCancellationRequested_ = false;
    fixedPatternProfileDeletionRequested_ = false;
  }
  start();
}

void CameraThread::setRenderSettings(
    const ThermalRenderSettings &settings) {
  const QMutexLocker locker(&renderSettingsMutex_);
  renderSettings_ = settings;
}

void CameraThread::setFixedPatternEnabled(bool enabled) {
  const QMutexLocker locker(&fixedPatternMutex_);
  fixedPatternEnabled_ = enabled;
}

void CameraThread::requestFixedPatternCalibration() {
  const QMutexLocker locker(&fixedPatternMutex_);
  fixedPatternCalibrationRequested_ = true;
  fixedPatternCalibrationCancellationRequested_ = false;
}

void CameraThread::cancelFixedPatternCalibration() {
  const QMutexLocker locker(&fixedPatternMutex_);
  fixedPatternCalibrationRequested_ = false;
  fixedPatternCalibrationCancellationRequested_ = true;
}

void CameraThread::requestFixedPatternProfileDeletion() {
  const QMutexLocker locker(&fixedPatternMutex_);
  fixedPatternCalibrationRequested_ = false;
  fixedPatternCalibrationCancellationRequested_ = false;
  fixedPatternProfileDeletionRequested_ = true;
}

CameraThread::FixedPatternCommands
CameraThread::takeFixedPatternCommands() {
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

ThermalRenderSettings CameraThread::renderSettingsSnapshot() const {
  const QMutexLocker locker(&renderSettingsMutex_);
  return renderSettings_;
}

void CameraThread::stop() {
  requestInterruption();
  wait();
}

void CameraThread::run() {
  std::unique_ptr<CameraSession> camera;

  try {
    camera = openCameraSession(device_);
    const CameraSessionInfo &info = camera->info();
    const std::optional<CameraFingerprint> &cameraFingerprint =
        info.fixedPatternFingerprint;
    const QString profilePath = cameraFingerprint
                                    ? fixedPatternProfilePath(*cameraFingerprint)
                                    : QString{};
    std::optional<FixedPatternProfile> fixedPatternProfile;
    QString initialProfileMessage;
    FixedPatternProfileLoadResult profileLoad;
    if (cameraFingerprint) {
      profileLoad = loadFixedPatternProfile(profilePath, *cameraFingerprint);
    } else {
      initialProfileMessage = QStringLiteral(
          "Learned fixed-pattern correction is unavailable for this camera.");
    }
    if (profileLoad.status == FixedPatternProfileLoadStatus::Loaded &&
        profileLoad.profile.has_value()) {
      if (profileLoad.profile->width == info.width &&
          profileLoad.profile->height == info.height) {
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

    emit cameraConnected(QString::fromStdString(info.name),
                         cameraFingerprint.has_value());

    const auto framePool = std::make_shared<ThermalFramePool>();
    std::shared_ptr<ThermalFrame> thermalFrame = framePool->acquire();
    std::optional<FixedPatternCalibrator> fixedPatternCalibrator;
    bool fixedPatternEnabled = true;
    bool initialProfileStatePending = true;
    int consecutiveCaptureTimeouts = 0;
    while (!isInterruptionRequested()) {
      const CameraFrameStatus frameStatus = camera->readFrame(*thermalFrame);
      if (frameStatus == CameraFrameStatus::Timeout) {
        if (++consecutiveCaptureTimeouts > kMaxConsecutiveCaptureTimeouts) {
          throw std::runtime_error("Camera capture timed out repeatedly");
        }
        continue;
      }
      consecutiveCaptureTimeouts = 0;

      const FixedPatternCommands commands = takeFixedPatternCommands();
      const bool enabledChanged = fixedPatternEnabled != commands.enabled;
      fixedPatternEnabled = commands.enabled;

      bool profileStateChanged = initialProfileStatePending || enabledChanged;
      QString profileStateMessage =
          initialProfileStatePending ? initialProfileMessage : QString{};
      initialProfileStatePending = false;

      if (commands.deleteProfile && cameraFingerprint) {
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
        if (commands.startCalibration && !commands.cancelCalibration &&
            cameraFingerprint) {
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
          frameStatus == CameraFrameStatus::Shutter) {
        fixedPatternCalibrator->noteShutterFrame();
        emit fixedPatternCalibrationProgress(
            static_cast<int>(fixedPatternCalibrator->frameCount()),
            static_cast<int>(kFixedPatternCalibrationFrameCount),
            static_cast<int>(fixedPatternCalibrator->shutterCount()));
      }

      if (frameStatus != CameraFrameStatus::Ready) {
        continue;
      }
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
              fixedPatternCalibrator->finalize(*cameraFingerprint);
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

      ThermalRenderSettings renderSettings = renderSettingsSnapshot();
      renderSettings.orientation = info.orientation;
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

    camera.reset();
  } catch (const std::exception &error) {
    camera.reset();
    if (!isInterruptionRequested()) {
      emit captureFailed(QString::fromUtf8(error.what()));
    }
  } catch (...) {
    camera.reset();
    if (!isInterruptionRequested()) {
      emit captureFailed(QStringLiteral("Unknown camera capture error"));
    }
  }
}
