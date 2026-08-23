#include "main_window.hpp"

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
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPen>
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

class ThermalImageWidget final : public QWidget {
public:
  explicit ThermalImageWidget(QWidget* parent = nullptr) : QWidget(parent) {
    setMinimumSize(640, 480);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    message_ = QStringLiteral("Connecting to thermal camera…");
  }

  void setFrame(const ThermalRenderResult& frame) {
    image_ = frame.image;
    coldestPoint_ = frame.coldestPoint;
    hottestPoint_ = frame.hottestPoint;
    message_.clear();
    update();
  }

  void clearFrame(const QString& message) {
    image_ = QImage();
    coldestPoint_ = QPoint(-1, -1);
    hottestPoint_ = QPoint(-1, -1);
    message_ = message;
    update();
  }

  bool hasFrame() const noexcept {
    return !image_.isNull();
  }

  void setMarkersVisible(bool visible) {
    markersVisible_ = visible;
    update();
  }

protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    if (image_.isNull()) {
      painter.setPen(QColor(240, 240, 240));
      painter.drawText(rect(), Qt::AlignCenter, message_);
      return;
    }

    QSize scaledSize = image_.size();
    scaledSize.scale(size(), Qt::KeepAspectRatio);
    const QRect targetRect(
        QPoint((width() - scaledSize.width()) / 2,
               (height() - scaledSize.height()) / 2),
        scaledSize);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawImage(targetRect, image_);

    if (!markersVisible_) {
      return;
    }

    drawMarker(painter, targetRect, coldestPoint_, QStringLiteral("MIN"),
               QColor(64, 200, 255), QPointF(-50.0, -28.0));
    drawMarker(painter, targetRect, hottestPoint_, QStringLiteral("MAX"),
               QColor(255, 80, 48), QPointF(10.0, 10.0));
  }

private:
  void drawMarker(QPainter& painter, const QRect& targetRect,
                  const QPoint& imagePoint, const QString& label,
                  const QColor& color, const QPointF& labelOffset) const {
    if (!image_.rect().contains(imagePoint)) {
      return;
    }

    const QPointF center(
        targetRect.left() +
            (static_cast<qreal>(imagePoint.x()) + 0.5) *
                static_cast<qreal>(targetRect.width()) / image_.width(),
        targetRect.top() +
            (static_cast<qreal>(imagePoint.y()) + 0.5) *
                static_cast<qreal>(targetRect.height()) / image_.height());
    const auto drawShape = [&painter, &center]() {
      painter.drawEllipse(center, 7.0, 7.0);
      painter.drawLine(center + QPointF(-12.0, 0.0),
                       center + QPointF(12.0, 0.0));
      painter.drawLine(center + QPointF(0.0, -12.0),
                       center + QPointF(0.0, 12.0));
    };

    painter.setPen(
        QPen(Qt::black, 5.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    drawShape();
    painter.setPen(
        QPen(color, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    drawShape();

    constexpr qreal kLabelWidth = 42.0;
    constexpr qreal kLabelHeight = 20.0;
    QRectF labelRect(center + labelOffset, QSizeF(kLabelWidth, kLabelHeight));
    labelRect.moveLeft(std::clamp(
        labelRect.left(), 2.0,
        std::max(2.0, static_cast<qreal>(width()) - kLabelWidth - 2.0)));
    labelRect.moveTop(std::clamp(
        labelRect.top(), 2.0,
        std::max(2.0, static_cast<qreal>(height()) - kLabelHeight - 2.0)));

    painter.fillRect(labelRect, QColor(0, 0, 0, 200));
    painter.setPen(QPen(color, 1.0));
    painter.drawRect(labelRect);
    QFont labelFont = painter.font();
    labelFont.setBold(true);
    painter.setFont(labelFont);
    painter.drawText(labelRect, Qt::AlignCenter, label);
  }

  QImage image_;
  QPoint coldestPoint_{-1, -1};
  QPoint hottestPoint_{-1, -1};
  QString message_;
  bool markersVisible_ = true;
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
  imageView_->setFrame(frame);
  currentDisplayRange_ = frame.displayRange;
  hasDisplayRange_ = isValidTemperatureRange(currentDisplayRange_);
  if (hasDisplayRange_) {
    temperatureScale_->setRange(currentDisplayRange_.minimumCelsius,
                                currentDisplayRange_.maximumCelsius);
  } else {
    temperatureScale_->clearRange();
  }
  showLiveStatus(
      QStringLiteral(
          "Live — %1 — Center %2 °C — Min %3 °C — Max %4 °C")
          .arg(cameraName_)
          .arg(frame.centerCelsius, 0, 'f', 1)
          .arg(frame.minimumCelsius, 0, 'f', 1)
          .arg(frame.maximumCelsius, 0, 'f', 1));
}

void MainWindow::showCameraConnected(const QString& cameraName) {
  cameraName_ = cameraName;
  hasDisplayRange_ = false;
  imageView_->clearFrame(QStringLiteral("Calibrating thermal camera…"));
  temperatureScale_->clearRange();
  showLiveStatus(QStringLiteral("Calibrating — %1").arg(cameraName_));
  qInfo().noquote() << "Connected to" << cameraName_;
}

void MainWindow::showCaptureError(const QString& message) {
  cameraName_.clear();
  hasDisplayRange_ = false;
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

void MainWindow::applyRenderSettings() {
  ThermalRenderSettings settings;
  settings.palette = selectedPalette_;
  settings.fixedRange = fixedRange_;
  cameraThread_.setRenderSettings(settings);
}

void MainWindow::updateRangeActionChecks() {
  automaticRangeAction_->setChecked(rangeMode_ == RangeMode::Automatic);
  lockedRangeAction_->setChecked(rangeMode_ == RangeMode::Locked);
  manualRangeAction_->setChecked(rangeMode_ == RangeMode::Manual);
}
