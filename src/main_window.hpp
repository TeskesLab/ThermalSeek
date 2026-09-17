#pragma once

#include <optional>

#include <QMainWindow>
#include <QString>

#include "camera_thread.hpp"

class QAction;
class QActionGroup;
class QLabel;
class QMenu;
class QProgressDialog;
class TemperatureScaleWidget;
class ThermalImageWidget;

class MainWindow final : public QMainWindow {
public:
  explicit MainWindow(QWidget *parent = nullptr);
  ~MainWindow() override;

private:
  enum class RangeMode {
    Automatic,
    Locked,
    Manual,
  };

  void displayFrame(const ThermalRenderResult &frame);
  void presentFrame(const ThermalRenderResult &frame);
  void showCameraConnected(const QString &cameraName, bool fixedPatternSupported);
  void showCaptureError(const QString &message);
  void saveScreenshot();
  void showLiveStatus(const QString &message);
  void showTransientStatus(const QString &message);

  void createCameraMenu();
  void rescanCameras();
  void selectCamera(const CameraDevice &device);
  void resetCameraView(const QString &message);

  void createDisplayMenu();
  void createMeasurementMenu();
  void configureRadiometry();
  void loadSettings();
  void beginFixedPatternCalibration();
  void cancelFixedPatternCalibration();
  void setFixedPatternEnabled(bool enabled);
  void deleteFixedPatternProfile();
  void updateFixedPatternProfileState(bool available, bool active,
                                      const QString &message);
  void updateFixedPatternCalibrationProgress(int frameCount,
                                             int targetFrameCount,
                                             int shutterCount);
  void finishFixedPatternCalibration(bool success, bool canceled,
                                     const QString &message);
  void saveSettings() const;
  void updateRadiometricStatus(const RadiometricSettings &settings);
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
  void updateFixedPatternActions();
  void updateFixedPatternStatus();

  ThermalImageWidget *imageView_;
  TemperatureScaleWidget *temperatureScale_;
  QLabel *radiometricStatusLabel_;
  QLabel *fixedPatternStatusLabel_;
  QProgressDialog *fixedPatternProgressDialog_;
  CameraThread cameraThread_;
  quint64 cameraGeneration_ = 0;
  std::optional<CameraDevice> selectedCamera_;
  ThermalPaletteId selectedPalette_ = ThermalPaletteId::Inferno;
  RangeMode rangeMode_ = RangeMode::Automatic;
  std::optional<TemperatureRange> fixedRange_;
  RadiometricSettings radiometricSettings_;
  bool fixedPatternEnabled_ = true;
  ThermalRenderResult currentFrame_;
  TemperatureRange currentDisplayRange_;
  bool hasDisplayRange_ = false;
  bool markersVisible_ = true;
  bool frozen_ = false;
  bool cameraReady_ = false;
  bool fixedPatternSupported_ = false;
  bool fixedPatternProfileAvailable_ = false;
  bool fixedPatternActive_ = false;
  bool fixedPatternCalibrationRunning_ = false;
  bool fixedPatternProfileOperationPending_ = false;
  int fixedPatternCalibrationFrames_ = 0;
  int fixedPatternCalibrationTargetFrames_ = 0;
  int fixedPatternCalibrationShutters_ = 0;

  QMenu *cameraDevicesMenu_ = nullptr;
  QActionGroup *cameraActionGroup_ = nullptr;
  QAction *reconnectCameraAction_ = nullptr;
  QActionGroup *paletteActionGroup_ = nullptr;
  QAction *automaticRangeAction_ = nullptr;
  QAction *lockedRangeAction_ = nullptr;
  QAction *manualRangeAction_ = nullptr;
  QAction *markersAction_ = nullptr;
  QAction *freezeAction_ = nullptr;
  QAction *fixedPatternCalibrationAction_ = nullptr;
  QAction *fixedPatternEnabledAction_ = nullptr;
  QAction *fixedPatternDeleteAction_ = nullptr;

  QString cameraName_;
  QString currentFrameStatusDetails_;
  QString liveStatusMessage_;
  QString transientStatusMessage_;
};
