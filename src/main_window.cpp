#include "main_window.hpp"
#include "thermal_image_widget.hpp"

#include "thermal_palette.hpp"

#include <algorithm>
#include <optional>
#include <cmath>

#include <QAction>
#include <QActionGroup>
#include <QDebug>
#include <QDateTime>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QShortcut>
#include <QSizePolicy>
#include <QStatusBar>
#include <QStandardPaths>
#include <QTimer>
#include <QWidget>

class TemperatureScaleWidget final : public QWidget {
public:
  explicit TemperatureScaleWidget(QWidget* parent = nullptr)
      : QWidget(parent), gradient_(1, 256, QImage::Format_RGB32) {
    setPalette(ThermalPaletteId::Inferno);
    setFixedWidth(112);
    setMinimumHeight(180);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
  }

  void setPalette(ThermalPaletteId paletteId) {
    const auto& palette = thermalPalette(paletteId);
    for (int y = 0; y < gradient_.height(); ++y) {
      auto* pixel = reinterpret_cast<QRgb*>(gradient_.scanLine(y));
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
  void paintEvent(QPaintEvent*) override {
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
    drawLabel(
        painter, middleY,
        QStringLiteral("%1 °C")
            .arg((minimumCelsius_ + maximumCelsius_) * 0.5F, 0, 'f', 1));
    drawLabel(painter, barRect.bottom(),
              QStringLiteral("%1 °C").arg(minimumCelsius_, 0, 'f', 1));
  }

private:
  void drawLabel(QPainter& painter, int centerY, const QString& text) const {
    const int lineHeight = painter.fontMetrics().height();
    const int labelTop =
        std::clamp(centerY - lineHeight / 2, 0, height() - lineHeight);
    painter.drawText(QRect(kLabelLeft, labelTop, width() - kLabelLeft,
                           lineHeight),
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
QString paletteName(ThermalPaletteId paletteId) {
  const auto& descriptors = thermalPaletteDescriptors();
  const auto descriptor =
      std::find_if(descriptors.begin(), descriptors.end(),
                   [paletteId](const ThermalPaletteDescriptor& candidate) {
                     return candidate.id == paletteId;
                   });
  if (descriptor == descriptors.end()) {
    return QStringLiteral("Unknown");
  }
  return QString::fromLatin1(descriptor->name.data(),
                             static_cast<int>(descriptor->name.size()));
}

std::optional<TemperatureRange> requestManualRange(
    QWidget* parent, const TemperatureRange& initialRange) {
  QDialog dialog(parent);
  dialog.setWindowTitle(QStringLiteral("Manual Temperature Range"));

  QFormLayout layout(&dialog);
  QDoubleSpinBox minimum(&dialog);
  QDoubleSpinBox maximum(&dialog);
  for (QDoubleSpinBox* input : {&minimum, &maximum}) {
    input->setDecimals(1);
    input->setRange(-273.1, 2000.0);
    input->setSingleStep(1.0);
    input->setSuffix(QStringLiteral(" °C"));
  }
  minimum.setValue(initialRange.minimumCelsius);
  maximum.setValue(initialRange.maximumCelsius);
  layout.addRow(QStringLiteral("&Minimum:"), &minimum);
  layout.addRow(QStringLiteral("M&aximum:"), &maximum);

  QDialogButtonBox buttons(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
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
}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      imageView_(new ThermalImageWidget(this)),
      temperatureScale_(new TemperatureScaleWidget(this)),
      cameraThread_(this) {
  setWindowTitle(QStringLiteral("ThermalSeek"));
  resize(900, 640);

  auto* content = new QWidget(this);
  content->setStyleSheet(QStringLiteral("background: black;"));
  auto* layout = new QHBoxLayout(content);
  layout->setContentsMargins(8, 8, 8, 8);
  layout->setSpacing(8);
  layout->addWidget(temperatureScale_);
  layout->addWidget(imageView_, 1);
  setCentralWidget(content);

  createDisplayMenu();
  auto* screenshotShortcut = new QShortcut(QKeySequence(Qt::Key_G), this);
  connect(screenshotShortcut, &QShortcut::activated, this,
          &MainWindow::saveScreenshot);
  showLiveStatus(QStringLiteral("Discovering USB cameras…"));

  connect(&cameraThread_, &SeekCameraThread::cameraConnected, this,
          &MainWindow::showCameraConnected, Qt::QueuedConnection);
  connect(&cameraThread_, &SeekCameraThread::frameReady, this,
          &MainWindow::displayFrame, Qt::QueuedConnection);
  connect(&cameraThread_, &SeekCameraThread::captureFailed, this,
          &MainWindow::showCaptureError, Qt::QueuedConnection);

  applyRenderSettings();
  cameraThread_.start();
}

MainWindow::~MainWindow() {
  cameraThread_.stop();
}


void MainWindow::displayFrame(const ThermalRenderResult& frame) {
  if (frozen_) {
    return;
  }
  presentFrame(frame);
}

void MainWindow::presentFrame(const ThermalRenderResult& frame) {
  currentFrame_ = frame;
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
  updateFrameStatus();
}

void MainWindow::showCameraConnected(const QString& cameraName) {
  cameraName_ = cameraName;
  currentFrame_ = ThermalRenderResult{};
  currentFrameStatusDetails_.clear();
  hasDisplayRange_ = false;
  frozen_ = false;
  freezeAction_->setChecked(false);
  imageView_->setFrozen(false);
  imageView_->clearFrame(QStringLiteral("Calibrating thermal camera…"));
  temperatureScale_->clearRange();
  showLiveStatus(QStringLiteral("Calibrating — %1").arg(cameraName_));
  qInfo().noquote() << "Connected to" << cameraName_;
}

void MainWindow::showCaptureError(const QString& message) {
  cameraName_.clear();
  currentFrame_ = ThermalRenderResult{};
  currentFrameStatusDetails_.clear();
  hasDisplayRange_ = false;
  frozen_ = false;
  freezeAction_->setChecked(false);
  imageView_->setFrozen(false);
  imageView_->clearFrame(QStringLiteral("Camera unavailable"));
  temperatureScale_->clearRange();
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

  const QString fileName =
      QStringLiteral("ThermalSeek_%1.png")
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

  const QString message =
      QStringLiteral("Screenshot saved: %1").arg(filePath);
  showTransientStatus(message);
  qInfo().noquote() << message;
}

void MainWindow::showLiveStatus(const QString& message) {
  liveStatusMessage_ = message;
  if (transientStatusMessage_.isEmpty()) {
    statusBar()->showMessage(liveStatusMessage_);
  }
}

void MainWindow::showTransientStatus(const QString& message) {
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
  auto* displayMenu = menuBar()->addMenu(QStringLiteral("&Display"));
  auto* paletteMenu = displayMenu->addMenu(QStringLiteral("&Palette"));
  paletteActionGroup_ = new QActionGroup(this);
  paletteActionGroup_->setExclusionPolicy(
      QActionGroup::ExclusionPolicy::Exclusive);

  for (const ThermalPaletteDescriptor& descriptor :
       thermalPaletteDescriptors()) {
    auto* action = paletteMenu->addAction(
        QString::fromLatin1(descriptor.name.data(),
                            static_cast<int>(descriptor.name.size())));
    action->setCheckable(true);
    action->setData(static_cast<int>(descriptor.id));
    action->setChecked(descriptor.id == selectedPalette_);
    paletteActionGroup_->addAction(action);
  }
  connect(paletteActionGroup_, &QActionGroup::triggered, this,
          [this](QAction* action) {
            selectPalette(
                static_cast<ThermalPaletteId>(action->data().toInt()));
          });

  paletteMenu->addSeparator();
  auto* cyclePaletteAction =
      paletteMenu->addAction(QStringLiteral("&Cycle Palette"));
  cyclePaletteAction->setShortcut(QKeySequence(Qt::Key_P));
  connect(cyclePaletteAction, &QAction::triggered, this,
          &MainWindow::cyclePalette);

  auto* rangeMenu =
      displayMenu->addMenu(QStringLiteral("Temperature &Range"));
  auto* rangeActionGroup = new QActionGroup(this);
  rangeActionGroup->setExclusionPolicy(
      QActionGroup::ExclusionPolicy::Exclusive);

  automaticRangeAction_ =
      rangeMenu->addAction(QStringLiteral("&Automatic"));
  lockedRangeAction_ =
      rangeMenu->addAction(QStringLiteral("&Lock Current Range"));
  manualRangeAction_ = rangeMenu->addAction(QStringLiteral("&Manual…"));
  for (QAction* action : {automaticRangeAction_, lockedRangeAction_,
                          manualRangeAction_}) {
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

  auto* clearMeasurementsAction =
      displayMenu->addAction(QStringLiteral("&Clear Measurements"));
  clearMeasurementsAction->setShortcut(QKeySequence(Qt::Key_Delete));
  connect(clearMeasurementsAction, &QAction::triggered, this,
          &MainWindow::clearMeasurements);
}

void MainWindow::selectPalette(ThermalPaletteId paletteId) {
  selectedPalette_ = paletteId;
  for (QAction* action : paletteActionGroup_->actions()) {
    action->setChecked(
        action->data().toInt() == static_cast<int>(selectedPalette_));
  }
  temperatureScale_->setPalette(selectedPalette_);
  applyRenderSettings();
  showTransientStatus(
      QStringLiteral("Palette: %1").arg(paletteName(selectedPalette_)));
}

void MainWindow::cyclePalette() {
  const auto& descriptors = thermalPaletteDescriptors();
  const auto current =
      std::find_if(descriptors.begin(), descriptors.end(),
                   [this](const ThermalPaletteDescriptor& descriptor) {
                     return descriptor.id == selectedPalette_;
                   });
  const std::size_t currentIndex =
      current == descriptors.end()
          ? 0
          : static_cast<std::size_t>(current - descriptors.begin());
  selectPalette(
      descriptors[(currentIndex + 1) % descriptors.size()].id);
}

void MainWindow::selectAutomaticRange() {
  rangeMode_ = RangeMode::Automatic;
  fixedRange_.reset();
  updateRangeActionChecks();
  applyRenderSettings();
  showTransientStatus(QStringLiteral("Display range: automatic"));
}

void MainWindow::lockCurrentRange() {
  if (!hasDisplayRange_) {
    updateRangeActionChecks();
    showTransientStatus(
        QStringLiteral("Cannot lock display range before a frame is available"));
    return;
  }

  rangeMode_ = RangeMode::Locked;
  fixedRange_ = currentDisplayRange_;
  updateRangeActionChecks();
  applyRenderSettings();
  showTransientStatus(
      QStringLiteral("Display range locked: %1–%2 °C")
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
  showTransientStatus(
      QStringLiteral("Manual display range: %1–%2 °C")
          .arg(fixedRange_->minimumCelsius, 0, 'f', 1)
          .arg(fixedRange_->maximumCelsius, 0, 'f', 1));
}

void MainWindow::setMarkersVisible(bool visible) {
  markersVisible_ = visible;
  imageView_->setMarkersVisible(markersVisible_);
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
  cameraThread_.setRenderSettings(settings);

  if (frozen_ && currentFrame_.sourceFrame) {
    presentFrame(renderThermalFrame(currentFrame_.sourceFrame, settings));
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
