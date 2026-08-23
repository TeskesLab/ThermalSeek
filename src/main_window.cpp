#include "main_window.hpp"

#include "thermal_palette.hpp"

#include <algorithm>
#include <cmath>

#include <QDebug>
#include <QDateTime>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QKeySequence>
#include <QPainter>
#include <QPixmap>
#include <QResizeEvent>
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
    const auto& palette = thermalPalette();
    for (int y = 0; y < gradient_.height(); ++y) {
      auto* pixel = reinterpret_cast<QRgb*>(gradient_.scanLine(y));
      pixel[0] = palette[static_cast<std::size_t>(255 - y)];
    }

    setFixedWidth(112);
    setMinimumHeight(180);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
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

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      imageLabel_(new QLabel(this)),
      temperatureScale_(new TemperatureScaleWidget(this)),
      cameraThread_(this) {
  setWindowTitle(QStringLiteral("ThermalSeek"));
  resize(900, 640);

  imageLabel_->setAlignment(Qt::AlignCenter);
  imageLabel_->setMinimumSize(640, 480);
  imageLabel_->setStyleSheet(QStringLiteral("background: black; color: white;"));
  imageLabel_->setText(QStringLiteral("Connecting to thermal camera…"));

  auto* content = new QWidget(this);
  content->setStyleSheet(QStringLiteral("background: black;"));
  auto* layout = new QHBoxLayout(content);
  layout->setContentsMargins(8, 8, 8, 8);
  layout->setSpacing(8);
  layout->addWidget(temperatureScale_);
  layout->addWidget(imageLabel_, 1);
  setCentralWidget(content);
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

  cameraThread_.start();
}

MainWindow::~MainWindow() {
  cameraThread_.stop();
}

void MainWindow::resizeEvent(QResizeEvent* event) {
  QMainWindow::resizeEvent(event);
  updatePixmap();
}

void MainWindow::displayFrame(const QImage& image, float minimumCelsius,
                              float maximumCelsius, float centerCelsius) {
  currentFrame_ = image;
  temperatureScale_->setRange(minimumCelsius, maximumCelsius);
  showLiveStatus(
      QStringLiteral(
          "Live — %1 — Center %2 °C — Min %3 °C — Max %4 °C")
          .arg(cameraName_)
          .arg(centerCelsius, 0, 'f', 1)
          .arg(minimumCelsius, 0, 'f', 1)
          .arg(maximumCelsius, 0, 'f', 1));
  updatePixmap();
}

void MainWindow::showCameraConnected(const QString& cameraName) {
  cameraName_ = cameraName;
  temperatureScale_->clearRange();
  showLiveStatus(QStringLiteral("Calibrating — %1").arg(cameraName_));
  qInfo().noquote() << "Connected to" << cameraName_;
}

void MainWindow::showCaptureError(const QString& message) {
  currentFrame_ = QImage();
  cameraName_.clear();
  temperatureScale_->clearRange();
  imageLabel_->setPixmap(QPixmap());
  imageLabel_->setText(QStringLiteral("Camera unavailable"));
  transientStatusMessage_.clear();
  showLiveStatus(message);
  qWarning().noquote() << "Camera capture failed:" << message;
}

void MainWindow::saveScreenshot() {
  if (currentFrame_.isNull()) {
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

void MainWindow::updatePixmap() {
  if (currentFrame_.isNull()) {
    return;
  }

  const QImage scaled = currentFrame_.scaled(
      imageLabel_->size(), Qt::KeepAspectRatio, Qt::FastTransformation);
  imageLabel_->setPixmap(QPixmap::fromImage(scaled));
}
