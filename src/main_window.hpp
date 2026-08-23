#pragma once

#include <optional>

#include <QMainWindow>
#include <QString>

#include "seek_camera_thread.hpp"

class QAction;
class QActionGroup;
class QLabel;
class TemperatureScaleWidget;
class ThermalImageWidget;

class MainWindow final : public QMainWindow {
public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

private:
  enum class RangeMode {
    Automatic,
    Locked,
    Manual,
  };

  void displayFrame(const ThermalRenderResult& frame);
  void presentFrame(const ThermalRenderResult& frame);
  void showCameraConnected(const QString& cameraName);
  void showCaptureError(const QString& message);
  void saveScreenshot();
  void showLiveStatus(const QString& message);
  void showTransientStatus(const QString& message);

  void createDisplayMenu();
  void createMeasurementMenu();
  void configureRadiometry();
  void loadSettings();
  void saveSettings() const;
  void updateRadiometricStatus(const RadiometricSettings& settings);
  void selectPalette(ThermalPaletteId paletteId);
  void cyclePalette();
  void selectAutomaticRange();
  void lockCurrentRange();
  void selectManualRange();
  void setMarkersVisible(bool visible);
  void setFrozen(bool frozen);
  void clearMeasurements();
  void applyRenderSettings();
  void updateRangeActionChecks();
  void updateFrameStatus();

  ThermalImageWidget* imageView_;
  TemperatureScaleWidget* temperatureScale_;
  QLabel* radiometricStatusLabel_;
  SeekCameraThread cameraThread_;
  ThermalPaletteId selectedPalette_ = ThermalPaletteId::Inferno;
  RangeMode rangeMode_ = RangeMode::Automatic;
  std::optional<TemperatureRange> fixedRange_;
  RadiometricSettings radiometricSettings_;
  ThermalRenderResult currentFrame_;
  TemperatureRange currentDisplayRange_;
  bool hasDisplayRange_ = false;
  bool markersVisible_ = true;
  bool frozen_ = false;

  QActionGroup* paletteActionGroup_ = nullptr;
  QAction* automaticRangeAction_ = nullptr;
  QAction* lockedRangeAction_ = nullptr;
  QAction* manualRangeAction_ = nullptr;
  QAction* markersAction_ = nullptr;
  QAction* freezeAction_ = nullptr;

  QString cameraName_;
  QString currentFrameStatusDetails_;
  QString liveStatusMessage_;
  QString transientStatusMessage_;
};
