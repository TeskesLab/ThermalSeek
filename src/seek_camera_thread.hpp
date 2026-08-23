#pragma once

#include <QMutex>
#include <QString>
#include <QThread>

#include "thermal_renderer.hpp"

class SeekCameraThread final : public QThread {
  Q_OBJECT

public:
  explicit SeekCameraThread(QObject *parent = nullptr);
  ~SeekCameraThread() override;

  void setRenderSettings(const ThermalRenderSettings &settings);
  void setFixedPatternEnabled(bool enabled);
  void requestFixedPatternCalibration();
  void cancelFixedPatternCalibration();
  void requestFixedPatternProfileDeletion();
  void stop();

signals:
  void cameraConnected(const QString &cameraName);
  void frameReady(const ThermalRenderResult &frame);
  void captureFailed(const QString &message);
  void fixedPatternProfileStateChanged(bool available, bool active,
                                       const QString &message);
  void fixedPatternCalibrationProgress(int frameCount, int targetFrameCount,
                                       int shutterCount);
  void fixedPatternCalibrationFinished(bool success, bool canceled,
                                       const QString &message);

protected:
  void run() override;

private:
  struct FixedPatternCommands final {
    bool enabled = true;
    bool startCalibration = false;
    bool cancelCalibration = false;
    bool deleteProfile = false;
  };

  ThermalRenderSettings renderSettingsSnapshot() const;
  FixedPatternCommands takeFixedPatternCommands();

  mutable QMutex renderSettingsMutex_;
  ThermalRenderSettings renderSettings_;

  QMutex fixedPatternMutex_;
  bool fixedPatternEnabled_ = true;
  bool fixedPatternCalibrationRequested_ = false;
  bool fixedPatternCalibrationCancellationRequested_ = false;
  bool fixedPatternProfileDeletionRequested_ = false;
};
