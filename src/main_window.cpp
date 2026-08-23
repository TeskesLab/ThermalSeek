#include "main_window.hpp"
#include "fixed_pattern_correction.hpp"
#include "thermal_image_widget.hpp"

#include "thermal_palette.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

#include <QAction>
#include <QActionGroup>
#include <QByteArray>
#include <QDateTime>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QProgressDialog>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QWidget>

class TemperatureScaleWidget final : public QWidget {
public:
  explicit TemperatureScaleWidget(QWidget *parent = nullptr)
      : QWidget(parent), gradient_(1, 256, QImage::Format_RGB32) {
    setPalette(ThermalPaletteId::Inferno);
    setFixedWidth(112);
    setMinimumHeight(180);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
  }

  void setPalette(ThermalPaletteId paletteId) {
    const auto &palette = thermalPalette(paletteId);
    for (int y = 0; y < gradient_.height(); ++y) {
      auto *pixel = reinterpret_cast<QRgb *>(gradient_.scanLine(y));
      pixel[0] = palette[static_cast<std::size_t>(255 - y)];
    }
    update();
  }

  void setRange(float minimumCelsius, float maximumCelsius) {
    minimumCelsius_ = minimumCelsius;
    maximumCelsius_ = maximumCelsius;
    hasRange_ = std::isfinite(minimumCelsius_) &&
                std::isfinite(maximumCelsius_) &&
                maximumCelsius_ > minimumCelsius_;
    update();
  }

  void clearRange() {
    hasRange_ = false;
    update();
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    const QRect barRect(kBarLeft, kVerticalMargin, kBarWidth,
                        std::max(1, height() - 2 * kVerticalMargin));
    painter.drawImage(barRect, gradient_);
    painter.setPen(QColor(210, 210, 210));
    painter.drawRect(barRect.adjusted(0, 0, -1, -1));

    const int middleY = barRect.center().y();
    for (const int y : {barRect.top(), middleY, barRect.bottom()}) {
      painter.drawLine(barRect.right() + 1, y, barRect.right() + 6, y);
    }

    painter.setPen(QColor(240, 240, 240));
    if (!hasRange_) {
      drawLabel(painter, middleY, QStringLiteral("-- °C"));
      return;
    }

    drawLabel(painter, barRect.top(),
              QStringLiteral("%1 °C").arg(maximumCelsius_, 0, 'f', 1));
    drawLabel(painter, middleY,
              QStringLiteral("%1 °C").arg(
                  (minimumCelsius_ + maximumCelsius_) * 0.5F, 0, 'f', 1));
    drawLabel(painter, barRect.bottom(),
              QStringLiteral("%1 °C").arg(minimumCelsius_, 0, 'f', 1));
  }

private:
  void drawLabel(QPainter &painter, int centerY, const QString &text) const {
    const int lineHeight = painter.fontMetrics().height();
    const int labelTop =
        std::clamp(centerY - lineHeight / 2, 0, height() - lineHeight);
    painter.drawText(
        QRect(kLabelLeft, labelTop, width() - kLabelLeft, lineHeight),
        Qt::AlignLeft | Qt::AlignVCenter, text);
  }

  static constexpr int kBarLeft = 8;
  static constexpr int kBarWidth = 28;
  static constexpr int kLabelLeft = 44;
  static constexpr int kVerticalMargin = 18;

  QImage gradient_;
  float minimumCelsius_ = 0.0F;
  float maximumCelsius_ = 0.0F;
  bool hasRange_ = false;
};

namespace {
int fixedPatternCalibrationPercent(int frameCount, int targetFrameCount,
                                   int shutterCount) noexcept {
  if (targetFrameCount <= 0) {
    return 0;
  }
  const int framePercent =
      std::clamp(static_cast<int>(static_cast<long long>(frameCount) * 100 /
                                  targetFrameCount),
                 0, 100);
  const int shutterPercent =
      std::clamp(static_cast<int>(
                     static_cast<long long>(shutterCount) * 100 /
                     static_cast<long long>(kFixedPatternMinimumShutterCount)),
                 0, 100);
  return std::min(framePercent, shutterPercent);
}

QString paletteName(ThermalPaletteId paletteId) {
  const auto &descriptors = thermalPaletteDescriptors();
  const auto descriptor =
      std::find_if(descriptors.begin(), descriptors.end(),
                   [paletteId](const ThermalPaletteDescriptor &candidate) {
                     return candidate.id == paletteId;
                   });
  if (descriptor == descriptors.end()) {
    return QStringLiteral("Unknown");
  }
  return QString::fromLatin1(descriptor->name.data(),
                             static_cast<int>(descriptor->name.size()));
}

std::optional<TemperatureRange>
requestManualRange(QWidget *parent, const TemperatureRange &initialRange) {
  QDialog dialog(parent);
  dialog.setWindowTitle(QStringLiteral("Manual Temperature Range"));

  QFormLayout layout(&dialog);
  QDoubleSpinBox minimum(&dialog);
  QDoubleSpinBox maximum(&dialog);
  for (QDoubleSpinBox *input : {&minimum, &maximum}) {
    input->setDecimals(1);
    input->setRange(-273.1, 2000.0);
    input->setSingleStep(1.0);
    input->setSuffix(QStringLiteral(" °C"));
  }
  minimum.setValue(initialRange.minimumCelsius);
  maximum.setValue(initialRange.maximumCelsius);
  layout.addRow(QStringLiteral("&Minimum:"), &minimum);
  layout.addRow(QStringLiteral("M&aximum:"), &maximum);

  QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                           &dialog);
  layout.addRow(&buttons);
  TemperatureRange selectedRange = initialRange;
  QObject::connect(&buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
    selectedRange = TemperatureRange{static_cast<float>(minimum.value()),
                                     static_cast<float>(maximum.value())};
    if (!isValidTemperatureRange(selectedRange)) {
      QMessageBox::warning(
          &dialog, QStringLiteral("Invalid Temperature Range"),
          QStringLiteral("Maximum temperature must be greater than minimum "
                         "temperature."));
      return;
    }
    dialog.accept();
  });
  QObject::connect(&buttons, &QDialogButtonBox::rejected, &dialog,
                   &QDialog::reject);

  if (dialog.exec() != QDialog::Accepted) {
    return std::nullopt;
  }
  return selectedRange;
}

std::optional<RadiometricSettings>
requestRadiometricSettings(QWidget *parent,
                           const RadiometricSettings &initialSettings) {
  QDialog dialog(parent);
  dialog.setWindowTitle(QStringLiteral("Radiometric Settings"));

  QFormLayout layout(&dialog);
  QDoubleSpinBox emissivity(&dialog);
  emissivity.setDecimals(2);
  emissivity.setRange(0.01, 1.0);
  emissivity.setSingleStep(0.01);
  emissivity.setValue(initialSettings.emissivity);
  layout.addRow(QStringLiteral("&Emissivity:"), &emissivity);

  QDoubleSpinBox reflectedTemperature(&dialog);
  reflectedTemperature.setDecimals(1);
  reflectedTemperature.setRange(-273.1, 2000.0);
  reflectedTemperature.setSingleStep(1.0);
  reflectedTemperature.setSuffix(QStringLiteral(" °C"));
  reflectedTemperature.setValue(initialSettings.reflectedTemperatureCelsius);
  layout.addRow(QStringLiteral("&Reflected temperature:"),
                &reflectedTemperature);

  QLabel explanation(
      QStringLiteral(
          "Emissivity 1.00 preserves the camera's apparent temperature. "
          "Lower values compensate for radiation reflected by the target."),
      &dialog);
  explanation.setWordWrap(true);
  layout.addRow(&explanation);

  QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                           &dialog);
  layout.addRow(&buttons);
  RadiometricSettings selectedSettings = initialSettings;
  QObject::connect(&buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
    selectedSettings =
        RadiometricSettings{static_cast<float>(emissivity.value()),
                            static_cast<float>(reflectedTemperature.value())};
    if (!isValidRadiometricSettings(selectedSettings)) {
      QMessageBox::warning(
          &dialog, QStringLiteral("Invalid Radiometric Settings"),
          QStringLiteral(
              "Emissivity must be greater than zero and no greater than "
              "one. Reflected temperature cannot be below absolute zero."));
      return;
    }
    dialog.accept();
  });
  QObject::connect(&buttons, &QDialogButtonBox::rejected, &dialog,
                   &QDialog::reject);

  if (dialog.exec() != QDialog::Accepted) {
    return std::nullopt;
  }
  return selectedSettings;
}
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), imageView_(new ThermalImageWidget(this)),
      temperatureScale_(new TemperatureScaleWidget(this)),
      radiometricStatusLabel_(new QLabel(this)),
      fixedPatternStatusLabel_(new QLabel(this)),
      fixedPatternProgressDialog_(new QProgressDialog(this)),
      cameraThread_(this) {
  setWindowTitle(QStringLiteral("ThermalSeek"));
  resize(900, 640);
  loadSettings();

  auto *content = new QWidget(this);
  content->setStyleSheet(QStringLiteral("background: black;"));
  auto *layout = new QHBoxLayout(content);
  layout->setContentsMargins(8, 8, 8, 8);
  layout->setSpacing(8);
  layout->addWidget(temperatureScale_);
  layout->addWidget(imageView_, 1);
  setCentralWidget(content);

  temperatureScale_->setPalette(selectedPalette_);
  imageView_->setMarkersVisible(markersVisible_);
  radiometricStatusLabel_->setToolTip(
      QStringLiteral("Emissivity and reflected-background temperature"));
  statusBar()->addPermanentWidget(radiometricStatusLabel_);
  fixedPatternStatusLabel_->setToolTip(
      QStringLiteral("Detector fixed-pattern correction status"));
  statusBar()->addPermanentWidget(fixedPatternStatusLabel_);
  fixedPatternProgressDialog_->setWindowTitle(
      QStringLiteral("Fixed-Pattern Calibration"));
  fixedPatternProgressDialog_->setWindowModality(Qt::WindowModal);
  fixedPatternProgressDialog_->setMinimumDuration(0);
  fixedPatternProgressDialog_->setAutoClose(false);
  fixedPatternProgressDialog_->setAutoReset(false);
  fixedPatternProgressDialog_->hide();
  connect(fixedPatternProgressDialog_, &QProgressDialog::canceled, this,
          &MainWindow::cancelFixedPatternCalibration);
  updateRadiometricStatus(radiometricSettings_);
  updateFixedPatternStatus();
  createDisplayMenu();
  createMeasurementMenu();
  updateFixedPatternActions();
  auto *screenshotShortcut = new QShortcut(QKeySequence(Qt::Key_G), this);
  connect(screenshotShortcut, &QShortcut::activated, this,
          &MainWindow::saveScreenshot);
  showLiveStatus(QStringLiteral("Discovering USB cameras…"));

  connect(&cameraThread_, &SeekCameraThread::cameraConnected, this,
          &MainWindow::showCameraConnected, Qt::QueuedConnection);
  connect(&cameraThread_, &SeekCameraThread::frameReady, this,
          &MainWindow::displayFrame, Qt::QueuedConnection);
  connect(&cameraThread_, &SeekCameraThread::captureFailed, this,
          &MainWindow::showCaptureError, Qt::QueuedConnection);
  connect(&cameraThread_, &SeekCameraThread::fixedPatternProfileStateChanged,
          this, &MainWindow::updateFixedPatternProfileState,
          Qt::QueuedConnection);
  connect(&cameraThread_, &SeekCameraThread::fixedPatternCalibrationProgress,
          this, &MainWindow::updateFixedPatternCalibrationProgress,
          Qt::QueuedConnection);
  connect(&cameraThread_, &SeekCameraThread::fixedPatternCalibrationFinished,
          this, &MainWindow::finishFixedPatternCalibration,
          Qt::QueuedConnection);

  applyRenderSettings();
  cameraThread_.setFixedPatternEnabled(fixedPatternEnabled_);
  cameraThread_.start();
}

MainWindow::~MainWindow() {
  saveSettings();
  cameraThread_.stop();
}

void MainWindow::displayFrame(const ThermalRenderResult &frame) {
  if (frozen_) {
    return;
  }
  presentFrame(frame);
}

void MainWindow::presentFrame(const ThermalRenderResult &frame) {
  currentFrame_ = frame;
  if (frame.cameraFrame) {
    cameraReady_ = true;
  }
  fixedPatternActive_ = frame.fixedPatternApplied;
  imageView_->setFrame(currentFrame_);
  currentDisplayRange_ = frame.displayRange;
  hasDisplayRange_ = isValidTemperatureRange(currentDisplayRange_);
  if (hasDisplayRange_) {
    temperatureScale_->setRange(currentDisplayRange_.minimumCelsius,
                                currentDisplayRange_.maximumCelsius);
  } else {
    temperatureScale_->clearRange();
  }
  currentFrameStatusDetails_ =
      QStringLiteral("%1 — Center %2 °C — Min %3 °C — Max %4 °C")
          .arg(cameraName_)
          .arg(frame.centerCelsius, 0, 'f', 1)
          .arg(frame.minimumCelsius, 0, 'f', 1)
          .arg(frame.maximumCelsius, 0, 'f', 1);
  updateRadiometricStatus(frame.radiometricSettings);
  updateFixedPatternActions();
  updateFixedPatternStatus();
  updateFrameStatus();
}

void MainWindow::showCameraConnected(const QString &cameraName) {
  cameraName_ = cameraName;
  currentFrame_ = ThermalRenderResult{};
  currentFrameStatusDetails_.clear();
  hasDisplayRange_ = false;
  cameraReady_ = false;
  fixedPatternProfileAvailable_ = false;
  fixedPatternActive_ = false;
  fixedPatternCalibrationRunning_ = false;
  fixedPatternProfileOperationPending_ = false;
  fixedPatternProgressDialog_->reset();
  fixedPatternProgressDialog_->hide();
  frozen_ = false;
  freezeAction_->setChecked(false);
  imageView_->setFrozen(false);
  imageView_->clearFrame(QStringLiteral("Calibrating thermal camera…"));
  temperatureScale_->clearRange();
  updateRadiometricStatus(radiometricSettings_);
  updateFixedPatternActions();
  updateFixedPatternStatus();
  showLiveStatus(QStringLiteral("Calibrating — %1").arg(cameraName_));
  qInfo().noquote() << "Connected to" << cameraName_;
}

void MainWindow::showCaptureError(const QString &message) {
  cameraName_.clear();
  currentFrame_ = ThermalRenderResult{};
  currentFrameStatusDetails_.clear();
  hasDisplayRange_ = false;
  cameraReady_ = false;
  fixedPatternProfileAvailable_ = false;
  fixedPatternActive_ = false;
  fixedPatternCalibrationRunning_ = false;
  fixedPatternProfileOperationPending_ = false;
  fixedPatternProgressDialog_->reset();
  fixedPatternProgressDialog_->hide();
  frozen_ = false;
  freezeAction_->setChecked(false);
  imageView_->setFrozen(false);
  imageView_->clearFrame(QStringLiteral("Camera unavailable"));
  temperatureScale_->clearRange();
  updateRadiometricStatus(radiometricSettings_);
  updateFixedPatternActions();
  updateFixedPatternStatus();
  transientStatusMessage_.clear();
  showLiveStatus(message);
  qWarning().noquote() << "Camera capture failed:" << message;
}

void MainWindow::saveScreenshot() {
  if (!imageView_->hasFrame()) {
    const QString message =
        QStringLiteral("Screenshot failed: no thermal frame is available");
    showTransientStatus(message);
    qWarning().noquote() << message;
    return;
  }

  const QString picturesPath =
      QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
  if (picturesPath.isEmpty()) {
    const QString message =
        QStringLiteral("Screenshot failed: Pictures folder is unavailable");
    showTransientStatus(message);
    qWarning().noquote() << message;
    return;
  }

  const QString screenshotDirectory =
      QDir(picturesPath).filePath(QStringLiteral("ThermalSeek"));
  if (!QDir().mkpath(screenshotDirectory)) {
    const QString message =
        QStringLiteral("Screenshot failed: cannot create %1")
            .arg(screenshotDirectory);
    showTransientStatus(message);
    qWarning().noquote() << message;
    return;
  }

  const QString fileName = QStringLiteral("ThermalSeek_%1.png")
                               .arg(QDateTime::currentDateTime().toString(
                                   QStringLiteral("yyyyMMdd_HHmmss_zzz")));
  const QString filePath = QDir(screenshotDirectory).filePath(fileName);

  statusBar()->showMessage(liveStatusMessage_);
  const QPixmap screenshot = grab();
  if (screenshot.isNull() || !screenshot.save(filePath, "PNG")) {
    const QString message =
        QStringLiteral("Screenshot failed: cannot save %1").arg(filePath);
    showTransientStatus(message);
    qWarning().noquote() << message;
    return;
  }

  const QString message = QStringLiteral("Screenshot saved: %1").arg(filePath);
  showTransientStatus(message);
  qInfo().noquote() << message;
}

void MainWindow::showLiveStatus(const QString &message) {
  liveStatusMessage_ = message;
  if (transientStatusMessage_.isEmpty()) {
    statusBar()->showMessage(liveStatusMessage_);
  }
}

void MainWindow::showTransientStatus(const QString &message) {
  transientStatusMessage_ = message;
  statusBar()->showMessage(transientStatusMessage_);
  QTimer::singleShot(5000, this, [this, message]() {
    if (transientStatusMessage_ != message) {
      return;
    }
    transientStatusMessage_.clear();
    statusBar()->showMessage(liveStatusMessage_);
  });
}

void MainWindow::createDisplayMenu() {
  auto *displayMenu = menuBar()->addMenu(QStringLiteral("&Display"));
  auto *paletteMenu = displayMenu->addMenu(QStringLiteral("&Palette"));
  paletteActionGroup_ = new QActionGroup(this);
  paletteActionGroup_->setExclusionPolicy(
      QActionGroup::ExclusionPolicy::Exclusive);

  for (const ThermalPaletteDescriptor &descriptor :
       thermalPaletteDescriptors()) {
    auto *action = paletteMenu->addAction(QString::fromLatin1(
        descriptor.name.data(), static_cast<int>(descriptor.name.size())));
    action->setCheckable(true);
    action->setData(static_cast<int>(descriptor.id));
    action->setChecked(descriptor.id == selectedPalette_);
    paletteActionGroup_->addAction(action);
  }
  connect(paletteActionGroup_, &QActionGroup::triggered, this,
          [this](QAction *action) {
            selectPalette(
                static_cast<ThermalPaletteId>(action->data().toInt()));
          });

  paletteMenu->addSeparator();
  auto *cyclePaletteAction =
      paletteMenu->addAction(QStringLiteral("&Cycle Palette"));
  cyclePaletteAction->setShortcut(QKeySequence(Qt::Key_P));
  connect(cyclePaletteAction, &QAction::triggered, this,
          &MainWindow::cyclePalette);

  auto *rangeMenu = displayMenu->addMenu(QStringLiteral("Temperature &Range"));
  auto *rangeActionGroup = new QActionGroup(this);
  rangeActionGroup->setExclusionPolicy(
      QActionGroup::ExclusionPolicy::Exclusive);

  automaticRangeAction_ = rangeMenu->addAction(QStringLiteral("&Automatic"));
  lockedRangeAction_ =
      rangeMenu->addAction(QStringLiteral("&Lock Current Range"));
  manualRangeAction_ = rangeMenu->addAction(QStringLiteral("&Manual…"));
  for (QAction *action :
       {automaticRangeAction_, lockedRangeAction_, manualRangeAction_}) {
    action->setCheckable(true);
    rangeActionGroup->addAction(action);
  }
  automaticRangeAction_->setShortcut(QKeySequence(Qt::Key_A));
  lockedRangeAction_->setShortcut(QKeySequence(Qt::Key_L));
  manualRangeAction_->setShortcut(QKeySequence(Qt::Key_M));
  updateRangeActionChecks();

  connect(automaticRangeAction_, &QAction::triggered, this,
          &MainWindow::selectAutomaticRange);
  connect(lockedRangeAction_, &QAction::triggered, this,
          &MainWindow::lockCurrentRange);
  connect(manualRangeAction_, &QAction::triggered, this,
          &MainWindow::selectManualRange);

  displayMenu->addSeparator();
  markersAction_ =
      displayMenu->addAction(QStringLiteral("Show &Hot/Cold Markers"));
  markersAction_->setCheckable(true);
  markersAction_->setChecked(markersVisible_);
  markersAction_->setShortcut(QKeySequence(Qt::Key_H));
  connect(markersAction_, &QAction::toggled, this,
          &MainWindow::setMarkersVisible);

  displayMenu->addSeparator();
  freezeAction_ = displayMenu->addAction(QStringLiteral("&Freeze Frame"));
  freezeAction_->setCheckable(true);
  freezeAction_->setShortcut(QKeySequence(Qt::Key_Space));
  connect(freezeAction_, &QAction::triggered, this, &MainWindow::setFrozen);

  auto *clearMeasurementsAction =
      displayMenu->addAction(QStringLiteral("&Clear Measurements"));
  clearMeasurementsAction->setShortcut(QKeySequence(Qt::Key_Delete));
  connect(clearMeasurementsAction, &QAction::triggered, this,
          &MainWindow::clearMeasurements);
}

void MainWindow::createMeasurementMenu() {
  auto *measurementMenu = menuBar()->addMenu(QStringLiteral("&Measurement"));
  auto *radiometricSettingsAction =
      measurementMenu->addAction(QStringLiteral("&Radiometric Settings…"));
  radiometricSettingsAction->setShortcut(QKeySequence(Qt::Key_R));
  connect(radiometricSettingsAction, &QAction::triggered, this,
          &MainWindow::configureRadiometry);

  measurementMenu->addSeparator();
  fixedPatternCalibrationAction_ = measurementMenu->addAction(
      QStringLiteral("&Calibrate Fixed-Pattern Correction…"));
  connect(fixedPatternCalibrationAction_, &QAction::triggered, this,
          &MainWindow::beginFixedPatternCalibration);

  fixedPatternEnabledAction_ = measurementMenu->addAction(
      QStringLiteral("&Enable Fixed-Pattern Correction"));
  fixedPatternEnabledAction_->setCheckable(true);
  fixedPatternEnabledAction_->setChecked(fixedPatternEnabled_);
  connect(fixedPatternEnabledAction_, &QAction::toggled, this,
          &MainWindow::setFixedPatternEnabled);

  fixedPatternDeleteAction_ = measurementMenu->addAction(
      QStringLiteral("&Delete Fixed-Pattern Profile…"));
  connect(fixedPatternDeleteAction_, &QAction::triggered, this,
          &MainWindow::deleteFixedPatternProfile);
}

void MainWindow::configureRadiometry() {
  const std::optional<RadiometricSettings> selectedSettings =
      requestRadiometricSettings(this, radiometricSettings_);
  if (!selectedSettings.has_value()) {
    return;
  }

  radiometricSettings_ = *selectedSettings;
  updateRadiometricStatus(radiometricSettings_);
  applyRenderSettings();
  saveSettings();
  showTransientStatus(
      QStringLiteral("Radiometry: emissivity %1, reflected %2 °C")
          .arg(radiometricSettings_.emissivity, 0, 'f', 2)
          .arg(radiometricSettings_.reflectedTemperatureCelsius, 0, 'f', 1));
}

void MainWindow::beginFixedPatternCalibration() {
  if (!cameraReady_ || fixedPatternCalibrationRunning_) {
    showTransientStatus(
        QStringLiteral("A live thermal frame is required for calibration"));
    return;
  }

  const QMessageBox::StandardButton response = QMessageBox::question(
      this, QStringLiteral("Calibrate Fixed-Pattern Correction"),
      QStringLiteral(
          "Place the lens directly against a smooth, matte, "
          "room-temperature surface with no gap. Keep the camera fully "
          "covered and still until collection finishes.\n\n"
          "Thermal capture will continue while ThermalSeek collects 96 "
          "usable frames spanning at least four shutter refreshes."),
      QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel);
  if (response != QMessageBox::Ok) {
    return;
  }

  if (frozen_) {
    setFrozen(false);
  }
  fixedPatternEnabled_ = true;
  {
    const QSignalBlocker blocker(fixedPatternEnabledAction_);
    fixedPatternEnabledAction_->setChecked(true);
  }
  cameraThread_.setFixedPatternEnabled(true);
  fixedPatternCalibrationRunning_ = true;
  fixedPatternCalibrationFrames_ = 0;
  fixedPatternCalibrationTargetFrames_ =
      static_cast<int>(kFixedPatternCalibrationFrameCount);
  fixedPatternCalibrationShutters_ = 0;
  fixedPatternProgressDialog_->reset();
  fixedPatternProgressDialog_->setCancelButtonText(QStringLiteral("Cancel"));
  fixedPatternProgressDialog_->setRange(0, 100);
  fixedPatternProgressDialog_->setValue(0);
  fixedPatternProgressDialog_->setLabelText(
      QStringLiteral("Collecting covered-lens frames…"));
  fixedPatternProgressDialog_->show();
  updateFixedPatternActions();
  updateFixedPatternStatus();
  saveSettings();
  cameraThread_.requestFixedPatternCalibration();
  showTransientStatus(QStringLiteral("Fixed-pattern calibration started"));
}

void MainWindow::cancelFixedPatternCalibration() {
  if (!fixedPatternCalibrationRunning_) {
    return;
  }
  cameraThread_.cancelFixedPatternCalibration();
  fixedPatternProgressDialog_->setLabelText(
      QStringLiteral("Canceling calibration…"));
  fixedPatternProgressDialog_->setCancelButtonText(QString{});
  showTransientStatus(QStringLiteral("Canceling fixed-pattern calibration"));
}

void MainWindow::setFixedPatternEnabled(bool enabled) {
  if (!fixedPatternProfileAvailable_) {
    const QSignalBlocker blocker(fixedPatternEnabledAction_);
    fixedPatternEnabledAction_->setChecked(fixedPatternEnabled_);
    return;
  }
  if (frozen_) {
    setFrozen(false);
  }

  fixedPatternEnabled_ = enabled;
  cameraThread_.setFixedPatternEnabled(enabled);
  saveSettings();
  showTransientStatus(
      enabled ? QStringLiteral("Fixed-pattern correction enabled")
              : QStringLiteral("Fixed-pattern correction disabled"));
}

void MainWindow::deleteFixedPatternProfile() {
  if (!fixedPatternProfileAvailable_ || fixedPatternProfileOperationPending_) {
    return;
  }
  const QMessageBox::StandardButton response = QMessageBox::question(
      this, QStringLiteral("Delete Fixed-Pattern Profile"),
      QStringLiteral(
          "Delete the calibration profile for the connected camera? "
          "Fixed-pattern correction will remain unavailable until the "
          "camera is recalibrated."),
      QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
  if (response != QMessageBox::Yes) {
    return;
  }

  if (frozen_) {
    setFrozen(false);
  }
  fixedPatternProfileOperationPending_ = true;
  updateFixedPatternActions();
  cameraThread_.requestFixedPatternProfileDeletion();
  showTransientStatus(QStringLiteral("Deleting fixed-pattern profile…"));
}

void MainWindow::updateFixedPatternProfileState(bool available, bool active,
                                                const QString &message) {
  fixedPatternProfileAvailable_ = available;
  fixedPatternActive_ = active;
  fixedPatternProfileOperationPending_ = false;
  updateFixedPatternActions();
  updateFixedPatternStatus();
  if (!message.isEmpty()) {
    showTransientStatus(message);
    qInfo().noquote() << message;
  }
}

void MainWindow::updateFixedPatternCalibrationProgress(int frameCount,
                                                       int targetFrameCount,
                                                       int shutterCount) {
  if (!fixedPatternCalibrationRunning_) {
    return;
  }
  fixedPatternCalibrationFrames_ = frameCount;
  fixedPatternCalibrationTargetFrames_ = targetFrameCount;
  fixedPatternCalibrationShutters_ = shutterCount;
  fixedPatternProgressDialog_->setRange(0, 100);
  fixedPatternProgressDialog_->setLabelText(
      QStringLiteral("%1 of %2 usable frames · %3 of %4 shutter refreshes")
          .arg(frameCount)
          .arg(targetFrameCount)
          .arg(shutterCount)
          .arg(kFixedPatternMinimumShutterCount));
  fixedPatternProgressDialog_->setValue(fixedPatternCalibrationPercent(
      frameCount, targetFrameCount, shutterCount));
  updateFixedPatternStatus();
}

void MainWindow::finishFixedPatternCalibration(bool success, bool canceled,
                                               const QString &message) {
  fixedPatternCalibrationRunning_ = false;
  fixedPatternProgressDialog_->hide();
  fixedPatternProgressDialog_->reset();
  fixedPatternProgressDialog_->setCancelButtonText(QStringLiteral("Cancel"));
  updateFixedPatternActions();
  updateFixedPatternStatus();

  if (success) {
    fixedPatternEnabled_ = true;
    {
      const QSignalBlocker blocker(fixedPatternEnabledAction_);
      fixedPatternEnabledAction_->setChecked(true);
    }
    cameraThread_.setFixedPatternEnabled(true);
    saveSettings();
    QMessageBox::information(
        this, QStringLiteral("Fixed-Pattern Calibration Complete"), message);
  } else if (canceled) {
    showTransientStatus(message);
  } else {
    QMessageBox::warning(
        this, QStringLiteral("Fixed-Pattern Calibration Failed"), message);
  }
}

void MainWindow::selectPalette(ThermalPaletteId paletteId) {
  selectedPalette_ = paletteId;
  for (QAction *action : paletteActionGroup_->actions()) {
    action->setChecked(action->data().toInt() ==
                       static_cast<int>(selectedPalette_));
  }
  temperatureScale_->setPalette(selectedPalette_);
  applyRenderSettings();
  saveSettings();
  showTransientStatus(
      QStringLiteral("Palette: %1").arg(paletteName(selectedPalette_)));
}

void MainWindow::cyclePalette() {
  const auto &descriptors = thermalPaletteDescriptors();
  const auto current =
      std::find_if(descriptors.begin(), descriptors.end(),
                   [this](const ThermalPaletteDescriptor &descriptor) {
                     return descriptor.id == selectedPalette_;
                   });
  const std::size_t currentIndex =
      current == descriptors.end()
          ? 0
          : static_cast<std::size_t>(current - descriptors.begin());
  selectPalette(descriptors[(currentIndex + 1) % descriptors.size()].id);
}

void MainWindow::selectAutomaticRange() {
  rangeMode_ = RangeMode::Automatic;
  fixedRange_.reset();
  updateRangeActionChecks();
  applyRenderSettings();
  saveSettings();
  showTransientStatus(QStringLiteral("Display range: automatic"));
}

void MainWindow::lockCurrentRange() {
  if (!hasDisplayRange_) {
    updateRangeActionChecks();
    showTransientStatus(QStringLiteral(
        "Cannot lock display range before a frame is available"));
    return;
  }

  rangeMode_ = RangeMode::Locked;
  fixedRange_ = currentDisplayRange_;
  updateRangeActionChecks();
  applyRenderSettings();
  saveSettings();
  showTransientStatus(QStringLiteral("Display range locked: %1–%2 °C")
                          .arg(fixedRange_->minimumCelsius, 0, 'f', 1)
                          .arg(fixedRange_->maximumCelsius, 0, 'f', 1));
}

void MainWindow::selectManualRange() {
  TemperatureRange initialRange{20.0F, 40.0F};
  if (fixedRange_.has_value() && isValidTemperatureRange(*fixedRange_)) {
    initialRange = *fixedRange_;
  } else if (hasDisplayRange_) {
    initialRange = currentDisplayRange_;
  }

  const std::optional<TemperatureRange> selectedRange =
      requestManualRange(this, initialRange);
  if (!selectedRange.has_value()) {
    updateRangeActionChecks();
    return;
  }

  rangeMode_ = RangeMode::Manual;
  fixedRange_ = *selectedRange;
  updateRangeActionChecks();
  applyRenderSettings();
  saveSettings();
  showTransientStatus(QStringLiteral("Manual display range: %1–%2 °C")
                          .arg(fixedRange_->minimumCelsius, 0, 'f', 1)
                          .arg(fixedRange_->maximumCelsius, 0, 'f', 1));
}

void MainWindow::setMarkersVisible(bool visible) {
  markersVisible_ = visible;
  imageView_->setMarkersVisible(markersVisible_);
  saveSettings();
  showTransientStatus(markersVisible_
                          ? QStringLiteral("Hot/cold markers shown")
                          : QStringLiteral("Hot/cold markers hidden"));
}

void MainWindow::setFrozen(bool frozen) {
  if (frozen && !imageView_->hasFrame()) {
    freezeAction_->setChecked(false);
    showTransientStatus(
        QStringLiteral("Cannot freeze before a frame is available"));
    return;
  }

  frozen_ = frozen;
  imageView_->setFrozen(frozen_);
  updateFrameStatus();
  showTransientStatus(frozen_ ? QStringLiteral("Frame frozen")
                              : QStringLiteral("Live capture resumed"));
}

void MainWindow::clearMeasurements() {
  imageView_->clearMeasurements();
  showTransientStatus(QStringLiteral("Measurements cleared"));
}

void MainWindow::applyRenderSettings() {
  ThermalRenderSettings settings;
  settings.palette = selectedPalette_;
  settings.fixedRange = fixedRange_;
  settings.radiometry = radiometricSettings_;
  cameraThread_.setRenderSettings(settings);

  if (currentFrame_.cameraFrame && currentFrame_.apparentFrame) {
    presentFrame(renderThermalFrame(currentFrame_.cameraFrame,
                                    currentFrame_.apparentFrame, settings));
  }
}

void MainWindow::loadSettings() {
  QSettings settings;

  const int savedPalette = settings
                               .value(QStringLiteral("display/palette"),
                                      static_cast<int>(selectedPalette_))
                               .toInt();
  const auto &palettes = thermalPaletteDescriptors();
  const auto palette =
      std::find_if(palettes.begin(), palettes.end(),
                   [savedPalette](const ThermalPaletteDescriptor &descriptor) {
                     return static_cast<int>(descriptor.id) == savedPalette;
                   });
  if (palette != palettes.end()) {
    selectedPalette_ = palette->id;
  }

  markersVisible_ =
      settings.value(QStringLiteral("display/showMarkers"), true).toBool();
  fixedPatternEnabled_ =
      settings.value(QStringLiteral("measurement/fixedPatternEnabled"), true)
          .toBool();

  const RadiometricSettings savedRadiometry{
      settings.value(QStringLiteral("measurement/emissivity"), 1.0).toFloat(),
      settings
          .value(QStringLiteral("measurement/reflectedTemperatureCelsius"),
                 20.0)
          .toFloat()};
  if (isValidRadiometricSettings(savedRadiometry)) {
    radiometricSettings_ = savedRadiometry;
  }

  const TemperatureRange savedRange{
      settings.value(QStringLiteral("display/fixedMinimumCelsius"), 0.0)
          .toFloat(),
      settings.value(QStringLiteral("display/fixedMaximumCelsius"), 0.0)
          .toFloat()};
  const QString savedRangeMode = settings
                                     .value(QStringLiteral("display/rangeMode"),
                                            QStringLiteral("automatic"))
                                     .toString();
  if (isValidTemperatureRange(savedRange) &&
      savedRangeMode == QStringLiteral("locked")) {
    rangeMode_ = RangeMode::Locked;
    fixedRange_ = savedRange;
  } else if (isValidTemperatureRange(savedRange) &&
             savedRangeMode == QStringLiteral("manual")) {
    rangeMode_ = RangeMode::Manual;
    fixedRange_ = savedRange;
  } else {
    rangeMode_ = RangeMode::Automatic;
    fixedRange_.reset();
  }

  const QByteArray savedGeometry =
      settings.value(QStringLiteral("window/geometry")).toByteArray();
  if (!savedGeometry.isEmpty()) {
    restoreGeometry(savedGeometry);
  }
}

void MainWindow::saveSettings() const {
  QSettings settings;
  settings.setValue(QStringLiteral("display/palette"),
                    static_cast<int>(selectedPalette_));
  settings.setValue(QStringLiteral("display/showMarkers"), markersVisible_);

  QString rangeMode = QStringLiteral("automatic");
  if (rangeMode_ == RangeMode::Locked) {
    rangeMode = QStringLiteral("locked");
  } else if (rangeMode_ == RangeMode::Manual) {
    rangeMode = QStringLiteral("manual");
  }
  settings.setValue(QStringLiteral("display/rangeMode"), rangeMode);
  if (fixedRange_.has_value() && isValidTemperatureRange(*fixedRange_)) {
    settings.setValue(QStringLiteral("display/fixedMinimumCelsius"),
                      fixedRange_->minimumCelsius);
    settings.setValue(QStringLiteral("display/fixedMaximumCelsius"),
                      fixedRange_->maximumCelsius);
  } else {
    settings.remove(QStringLiteral("display/fixedMinimumCelsius"));
    settings.remove(QStringLiteral("display/fixedMaximumCelsius"));
  }

  settings.setValue(QStringLiteral("measurement/emissivity"),
                    radiometricSettings_.emissivity);
  settings.setValue(QStringLiteral("measurement/reflectedTemperatureCelsius"),
                    radiometricSettings_.reflectedTemperatureCelsius);
  settings.setValue(QStringLiteral("measurement/fixedPatternEnabled"),
                    fixedPatternEnabled_);
  settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
  settings.sync();
  if (settings.status() != QSettings::NoError) {
    qWarning() << "Could not persist ThermalSeek settings";
  }
}

void MainWindow::updateRadiometricStatus(const RadiometricSettings &settings) {
  radiometricStatusLabel_->setText(
      QStringLiteral("ε %1 · Reflected %2 °C")
          .arg(settings.emissivity, 0, 'f', 2)
          .arg(settings.reflectedTemperatureCelsius, 0, 'f', 1));
}

void MainWindow::updateFixedPatternActions() {
  if (fixedPatternCalibrationAction_ == nullptr ||
      fixedPatternEnabledAction_ == nullptr ||
      fixedPatternDeleteAction_ == nullptr) {
    return;
  }

  const bool operationIdle =
      !fixedPatternCalibrationRunning_ && !fixedPatternProfileOperationPending_;
  if (freezeAction_ != nullptr) {
    freezeAction_->setEnabled(!fixedPatternCalibrationRunning_);
  }
  fixedPatternCalibrationAction_->setEnabled(cameraReady_ && operationIdle);
  fixedPatternEnabledAction_->setEnabled(
      cameraReady_ && fixedPatternProfileAvailable_ && operationIdle);
  fixedPatternDeleteAction_->setEnabled(
      cameraReady_ && fixedPatternProfileAvailable_ && operationIdle);
  const QSignalBlocker blocker(fixedPatternEnabledAction_);
  fixedPatternEnabledAction_->setChecked(fixedPatternEnabled_);
}

void MainWindow::updateFixedPatternStatus() {
  if (fixedPatternCalibrationRunning_) {
    const int percent = fixedPatternCalibrationPercent(
        fixedPatternCalibrationFrames_, fixedPatternCalibrationTargetFrames_,
        fixedPatternCalibrationShutters_);
    fixedPatternStatusLabel_->setText(
        QStringLiteral("FPN Calibrating %1%").arg(percent));
    fixedPatternStatusLabel_->setToolTip(
        QStringLiteral("%1 of %2 usable frames; %3 of %4 shutter refreshes")
            .arg(fixedPatternCalibrationFrames_)
            .arg(fixedPatternCalibrationTargetFrames_)
            .arg(fixedPatternCalibrationShutters_)
            .arg(kFixedPatternMinimumShutterCount));
  } else if (!fixedPatternProfileAvailable_) {
    fixedPatternStatusLabel_->setText(QStringLiteral("FPN No profile"));
    fixedPatternStatusLabel_->setToolTip(
        QStringLiteral("No fixed-pattern profile for the connected camera"));
  } else if (fixedPatternActive_) {
    fixedPatternStatusLabel_->setText(QStringLiteral("FPN On"));
    fixedPatternStatusLabel_->setToolTip(
        QStringLiteral("Fixed-pattern correction is active"));
  } else {
    fixedPatternStatusLabel_->setText(QStringLiteral("FPN Off"));
    fixedPatternStatusLabel_->setToolTip(
        QStringLiteral("A fixed-pattern profile is available but disabled"));
  }
}

void MainWindow::updateRangeActionChecks() {
  automaticRangeAction_->setChecked(rangeMode_ == RangeMode::Automatic);
  lockedRangeAction_->setChecked(rangeMode_ == RangeMode::Locked);
  manualRangeAction_->setChecked(rangeMode_ == RangeMode::Manual);
}

void MainWindow::updateFrameStatus() {
  if (currentFrameStatusDetails_.isEmpty()) {
    return;
  }
  showLiveStatus(
      QStringLiteral("%1 — %2")
          .arg(frozen_ ? QStringLiteral("Frozen") : QStringLiteral("Live"))
          .arg(currentFrameStatusDetails_));
}
