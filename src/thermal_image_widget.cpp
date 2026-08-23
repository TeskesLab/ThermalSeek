#include "thermal_image_widget.hpp"

#include "thermal_inspection.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <QEvent>
#include <QFont>
#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QSizePolicy>

ThermalImageWidget::ThermalImageWidget(QWidget* parent) : QWidget(parent) {
  setMinimumSize(640, 480);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  setMouseTracking(true);
  setCursor(Qt::CrossCursor);
  message_ = QStringLiteral("Connecting to thermal camera…");
}

void ThermalImageWidget::setFrame(const ThermalRenderResult& frame) {
  if (!frame_.image.isNull() && frame_.image.size() != frame.image.size()) {
    clearMeasurements();
    cursorPoint_.reset();
  }
  frame_ = frame;
  message_.clear();
  update();
}

void ThermalImageWidget::clearFrame(const QString& message) {
  frame_ = ThermalRenderResult{};
  message_ = message;
  cursorPoint_.reset();
  clearMeasurements();
  update();
}

bool ThermalImageWidget::hasFrame() const noexcept {
  return !frame_.image.isNull() && frame_.measurementFrame != nullptr;
}

void ThermalImageWidget::setMarkersVisible(bool visible) {
  markersVisible_ = visible;
  update();
}

void ThermalImageWidget::setFrozen(bool frozen) {
  frozen_ = frozen;
  update();
}

void ThermalImageWidget::clearMeasurements() {
  dragStartPoint_.reset();
  dragCurrentPoint_.reset();
  region_.reset();
  pinnedPoints_.clear();
  update();
}

void ThermalImageWidget::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.fillRect(rect(), Qt::black);

  if (!hasFrame()) {
    painter.setPen(QColor(240, 240, 240));
    painter.drawText(rect(), Qt::AlignCenter, message_);
    return;
  }

  const QRect targetRect = imageTargetRect();
  painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
  painter.drawImage(targetRect, frame_.image);

  if (markersVisible_) {
    drawMarker(painter, targetRect, frame_.coldestPoint,
               QStringLiteral("MIN"), QColor(64, 200, 255),
               QPointF(-50.0, -28.0));
    drawMarker(painter, targetRect, frame_.hottestPoint,
               QStringLiteral("MAX"), QColor(255, 80, 48),
               QPointF(10.0, 10.0));
  }

  const std::optional<QRect> activeRegion =
      dragStartPoint_.has_value() && dragCurrentPoint_.has_value()
          ? std::optional<QRect>(
                QRect(*dragStartPoint_, *dragCurrentPoint_).normalized())
          : region_;
  if (activeRegion.has_value()) {
    drawRegion(painter, targetRect, *activeRegion);
  }

  for (std::size_t index = 0; index < pinnedPoints_.size(); ++index) {
    const std::optional<ThermalPointMeasurement> measurement =
        measureThermalPoint(*frame_.measurementFrame, pinnedPoints_[index]);
    if (!measurement.has_value()) {
      continue;
    }
    drawMarker(
        painter, targetRect, pinnedPoints_[index],
        QStringLiteral("P%1  %2 °C")
            .arg(index + 1)
            .arg(measurement->celsius, 0, 'f', 1),
        QColor(255, 220, 64), QPointF(10.0, 10.0));
  }

  if (cursorPoint_.has_value()) {
    const std::optional<ThermalPointMeasurement> measurement =
        measureThermalPoint(*frame_.measurementFrame, *cursorPoint_);
    if (measurement.has_value()) {
      drawMarker(painter, targetRect, *cursorPoint_,
                 QStringLiteral("%1 °C").arg(measurement->celsius, 0, 'f', 1),
                 QColor(245, 245, 245), QPointF(10.0, -32.0));
    }
  }

  if (frozen_) {
    drawLabel(painter, QStringLiteral("FROZEN"),
              QPointF(targetRect.right() - 80.0, targetRect.top() + 8.0),
              QColor(64, 220, 255));
  }
}

void ThermalImageWidget::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton || !hasFrame()) {
    QWidget::mousePressEvent(event);
    return;
  }

  const std::optional<QPoint> imagePoint = imagePointAt(event->position());
  if (!imagePoint.has_value()) {
    QWidget::mousePressEvent(event);
    return;
  }

  pressWidgetPoint_ = event->position().toPoint();
  dragStartPoint_ = *imagePoint;
  dragCurrentPoint_ = *imagePoint;
  cursorPoint_ = *imagePoint;
  event->accept();
  update();
}

void ThermalImageWidget::mouseMoveEvent(QMouseEvent* event) {
  cursorPoint_ = imagePointAt(event->position());
  if (dragStartPoint_.has_value() &&
      event->buttons().testFlag(Qt::LeftButton)) {
    dragCurrentPoint_ = clampedImagePointAt(event->position());
  }
  event->accept();
  update();
}

void ThermalImageWidget::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton || !dragStartPoint_.has_value()) {
    QWidget::mouseReleaseEvent(event);
    return;
  }

  const std::optional<QPoint> releasePoint =
      clampedImagePointAt(event->position());
  if (releasePoint.has_value()) {
    const int dragDistance =
        (event->position().toPoint() - pressWidgetPoint_).manhattanLength();
    if (dragDistance < 5) {
      togglePinnedPoint(*dragStartPoint_);
    } else {
      region_ = QRect(*dragStartPoint_, *releasePoint).normalized();
    }
    cursorPoint_ = *releasePoint;
  }

  dragStartPoint_.reset();
  dragCurrentPoint_.reset();
  event->accept();
  update();
}

void ThermalImageWidget::leaveEvent(QEvent* event) {
  if (!dragStartPoint_.has_value()) {
    cursorPoint_.reset();
    update();
  }
  QWidget::leaveEvent(event);
}

QRect ThermalImageWidget::imageTargetRect() const noexcept {
  if (frame_.image.isNull()) {
    return QRect();
  }
  QSize scaledSize = frame_.image.size();
  scaledSize.scale(size(), Qt::KeepAspectRatio);
  return QRect(QPoint((width() - scaledSize.width()) / 2,
                      (height() - scaledSize.height()) / 2),
               scaledSize);
}

std::optional<QPoint> ThermalImageWidget::imagePointAt(
    const QPointF& widgetPoint) const noexcept {
  return mapWidgetPointToImage(widgetPoint, imageTargetRect(),
                               frame_.image.size());
}

std::optional<QPoint> ThermalImageWidget::clampedImagePointAt(
    const QPointF& widgetPoint) const noexcept {
  const QRect targetRect = imageTargetRect();
  if (targetRect.isEmpty()) {
    return std::nullopt;
  }
  const QPointF clampedPoint(
      std::clamp(widgetPoint.x(), static_cast<qreal>(targetRect.left()),
                 static_cast<qreal>(targetRect.right())),
      std::clamp(widgetPoint.y(), static_cast<qreal>(targetRect.top()),
                 static_cast<qreal>(targetRect.bottom())));
  return mapWidgetPointToImage(clampedPoint, targetRect, frame_.image.size());
}

QRectF ThermalImageWidget::imageRegionToWidget(
    const QRect& imageRegion, const QRect& targetRect) const noexcept {
  const qreal scaleX = static_cast<qreal>(targetRect.width()) /
                       static_cast<qreal>(frame_.image.width());
  const qreal scaleY = static_cast<qreal>(targetRect.height()) /
                       static_cast<qreal>(frame_.image.height());
  return QRectF(
      targetRect.left() + imageRegion.left() * scaleX,
      targetRect.top() + imageRegion.top() * scaleY,
      imageRegion.width() * scaleX, imageRegion.height() * scaleY);
}

void ThermalImageWidget::drawMarker(
    QPainter& painter, const QRect& targetRect, const QPoint& imagePoint,
    const QString& label, const QColor& color,
    const QPointF& labelOffset) const {
  if (!frame_.image.rect().contains(imagePoint)) {
    return;
  }

  const QPointF center(
      targetRect.left() +
          (static_cast<qreal>(imagePoint.x()) + 0.5) *
              static_cast<qreal>(targetRect.width()) / frame_.image.width(),
      targetRect.top() +
          (static_cast<qreal>(imagePoint.y()) + 0.5) *
              static_cast<qreal>(targetRect.height()) / frame_.image.height());
  const auto drawShape = [&painter, &center]() {
    painter.drawEllipse(center, 7.0, 7.0);
    painter.drawLine(center + QPointF(-12.0, 0.0),
                     center + QPointF(12.0, 0.0));
    painter.drawLine(center + QPointF(0.0, -12.0),
                     center + QPointF(0.0, 12.0));
  };

  painter.save();
  painter.setPen(
      QPen(Qt::black, 5.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  drawShape();
  painter.setPen(
      QPen(color, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  drawShape();
  painter.restore();
  drawLabel(painter, label, center + labelOffset, color);
}

void ThermalImageWidget::drawRegion(QPainter& painter, const QRect& targetRect,
                                    const QRect& imageRegion) const {
  if (!frame_.measurementFrame) {
    return;
  }
  const std::optional<ThermalRegionStatistics> statistics =
      measureThermalRegion(*frame_.measurementFrame, imageRegion);
  if (!statistics.has_value()) {
    return;
  }

  const QRectF widgetRegion =
      imageRegionToWidget(statistics->imageRegion, targetRect);
  painter.save();
  painter.setPen(QPen(Qt::black, 5.0, Qt::SolidLine, Qt::SquareCap,
                      Qt::MiterJoin));
  painter.drawRect(widgetRegion);
  painter.setPen(QPen(QColor(80, 255, 120), 2.0, Qt::DashLine,
                      Qt::SquareCap, Qt::MiterJoin));
  painter.drawRect(widgetRegion);
  painter.restore();

  const QString centerText =
      std::isfinite(statistics->centerCelsius)
          ? QStringLiteral("%1").arg(statistics->centerCelsius, 0, 'f', 1)
          : QStringLiteral("--");
  const QString label =
      QStringLiteral("ROI  Min %1 °C  Max %2 °C  Avg %3 °C  Center %4 °C")
          .arg(statistics->minimumCelsius, 0, 'f', 1)
          .arg(statistics->maximumCelsius, 0, 'f', 1)
          .arg(statistics->averageCelsius, 0, 'f', 1)
          .arg(centerText);
  drawLabel(painter, label,
            QPointF(widgetRegion.left(), widgetRegion.top() - 28.0),
            QColor(80, 255, 120));
}

void ThermalImageWidget::drawLabel(QPainter& painter, const QString& text,
                                   const QPointF& preferredTopLeft,
                                   const QColor& color) const {
  painter.save();
  QFont labelFont = painter.font();
  labelFont.setBold(true);
  painter.setFont(labelFont);
  const QFontMetricsF metrics(labelFont);
  const QSizeF labelSize(metrics.horizontalAdvance(text) + 12.0,
                         metrics.height() + 6.0);
  QRectF labelRect(preferredTopLeft, labelSize);
  labelRect.moveLeft(std::clamp(
      labelRect.left(), 2.0,
      std::max(2.0, static_cast<qreal>(width()) - labelSize.width() - 2.0)));
  labelRect.moveTop(std::clamp(
      labelRect.top(), 2.0,
      std::max(2.0, static_cast<qreal>(height()) - labelSize.height() - 2.0)));

  painter.fillRect(labelRect, QColor(0, 0, 0, 210));
  painter.setPen(QPen(color, 1.0));
  painter.drawRect(labelRect);
  painter.drawText(labelRect, Qt::AlignCenter, text);
  painter.restore();
}

void ThermalImageWidget::togglePinnedPoint(const QPoint& imagePoint) {
  const auto existing =
      std::find(pinnedPoints_.begin(), pinnedPoints_.end(), imagePoint);
  if (existing != pinnedPoints_.end()) {
    pinnedPoints_.erase(existing);
    return;
  }
  pinnedPoints_.push_back(imagePoint);
}
