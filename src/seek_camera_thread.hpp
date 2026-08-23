#pragma once

#include <QMutex>
#include <QString>
#include <QThread>

#include "thermal_renderer.hpp"

class SeekCameraThread final : public QThread {
  Q_OBJECT

public:
  explicit SeekCameraThread(QObject* parent = nullptr);
  ~SeekCameraThread() override;

  void setRenderSettings(const ThermalRenderSettings& settings);
  void stop();

signals:
  void cameraConnected(const QString& cameraName);
  void frameReady(const ThermalRenderResult& frame);
  void captureFailed(const QString& message);

protected:
  void run() override;

private:
  ThermalRenderSettings renderSettingsSnapshot() const;

  mutable QMutex renderSettingsMutex_;
  ThermalRenderSettings renderSettings_;
};
