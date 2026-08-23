#pragma once

#include <QImage>
#include <QMainWindow>
#include <QString>

#include "seek_camera_thread.hpp"

class QLabel;
class TemperatureScaleWidget;
class QResizeEvent;

class MainWindow final : public QMainWindow {
public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

protected:
  void resizeEvent(QResizeEvent* event) override;

private:
  void displayFrame(const QImage& image, float minimumCelsius,
                    float maximumCelsius, float centerCelsius);
  void showCameraConnected(const QString& cameraName);
  void showCaptureError(const QString& message);
  void saveScreenshot();
  void showLiveStatus(const QString& message);
  void showTransientStatus(const QString& message);
  void updatePixmap();

  QLabel* imageLabel_;
  TemperatureScaleWidget* temperatureScale_;
  QImage currentFrame_;
  QString cameraName_;
  QString liveStatusMessage_;
  QString transientStatusMessage_;
  SeekCameraThread cameraThread_;
};
