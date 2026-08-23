#pragma once

#include <optional>
#include <vector>

#include <QColor>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QWidget>

#include "thermal_renderer.hpp"

class QEvent;
class QMouseEvent;
class QPaintEvent;
class QPainter;

class ThermalImageWidget final : public QWidget {
public:
  explicit ThermalImageWidget(QWidget* parent = nullptr);

  void setFrame(const ThermalRenderResult& frame);
  void clearFrame(const QString& message);
  bool hasFrame() const noexcept;
  void setMarkersVisible(bool visible);
  void setFrozen(bool frozen);
  void clearMeasurements();

protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void leaveEvent(QEvent* event) override;

private:
  QRect imageTargetRect() const noexcept;
  std::optional<QPoint> imagePointAt(const QPointF& widgetPoint) const noexcept;
  std::optional<QPoint> clampedImagePointAt(
      const QPointF& widgetPoint) const noexcept;
  QRectF imageRegionToWidget(const QRect& imageRegion,
                             const QRect& targetRect) const noexcept;
  void drawMarker(QPainter& painter, const QRect& targetRect,
                  const QPoint& imagePoint, const QString& label,
                  const QColor& color, const QPointF& labelOffset) const;
  void drawRegion(QPainter& painter, const QRect& targetRect,
                  const QRect& imageRegion) const;
  void drawLabel(QPainter& painter, const QString& text,
                 const QPointF& preferredTopLeft, const QColor& color) const;
  void togglePinnedPoint(const QPoint& imagePoint);

  ThermalRenderResult frame_;
  QString message_;
  std::optional<QPoint> cursorPoint_;
  std::optional<QPoint> dragStartPoint_;
  std::optional<QPoint> dragCurrentPoint_;
  std::optional<QRect> region_;
  std::vector<QPoint> pinnedPoints_;
  QPoint pressWidgetPoint_;
  bool markersVisible_ = true;
  bool frozen_ = false;
};
