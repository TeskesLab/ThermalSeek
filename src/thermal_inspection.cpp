#include "thermal_inspection.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
bool hasValidDimensions(const ThermalFrame& frame) noexcept {
  if (frame.width == 0 || frame.height == 0 ||
      frame.width > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      frame.height > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  if (frame.width > std::numeric_limits<std::size_t>::max() / frame.height) {
    return false;
  }
  return frame.celsius.size() == frame.width * frame.height;
}

std::optional<QPoint> imagePointToDetector(
    const ThermalFrame& frame, const QPoint& imagePoint) noexcept {
  if (!hasValidDimensions(frame) || imagePoint.x() < 0 || imagePoint.y() < 0 ||
      imagePoint.x() >= static_cast<int>(frame.height) ||
      imagePoint.y() >= static_cast<int>(frame.width)) {
    return std::nullopt;
  }

  return QPoint(static_cast<int>(frame.width) - 1 - imagePoint.y(),
                imagePoint.x());
}
}  // namespace

std::optional<QPoint> mapWidgetPointToImage(
    const QPointF& widgetPoint, const QRect& imageTarget,
    const QSize& imageSize) noexcept {
  if (imageTarget.width() <= 0 || imageTarget.height() <= 0 ||
      imageSize.width() <= 0 || imageSize.height() <= 0) {
    return std::nullopt;
  }

  const qreal relativeX = widgetPoint.x() - imageTarget.left();
  const qreal relativeY = widgetPoint.y() - imageTarget.top();
  if (relativeX < 0.0 || relativeY < 0.0 ||
      relativeX >= imageTarget.width() ||
      relativeY >= imageTarget.height()) {
    return std::nullopt;
  }

  const int imageX = std::min(
      imageSize.width() - 1,
      static_cast<int>(relativeX * imageSize.width() / imageTarget.width()));
  const int imageY = std::min(
      imageSize.height() - 1,
      static_cast<int>(relativeY * imageSize.height() / imageTarget.height()));
  return QPoint(imageX, imageY);
}

std::optional<ThermalPointMeasurement> measureThermalPoint(
    const ThermalFrame& frame, const QPoint& imagePoint) noexcept {
  const std::optional<QPoint> detectorPoint =
      imagePointToDetector(frame, imagePoint);
  if (!detectorPoint.has_value()) {
    return std::nullopt;
  }

  const std::size_t index =
      static_cast<std::size_t>(detectorPoint->y()) * frame.width +
      static_cast<std::size_t>(detectorPoint->x());
  const float celsius = frame.celsius[index];
  if (!std::isfinite(celsius)) {
    return std::nullopt;
  }

  return ThermalPointMeasurement{imagePoint, *detectorPoint, celsius};
}

std::optional<ThermalRegionStatistics> measureThermalRegion(
    const ThermalFrame& frame, const QRect& imageRegion) noexcept {
  if (!hasValidDimensions(frame)) {
    return std::nullopt;
  }

  const QRect imageBounds(0, 0, static_cast<int>(frame.height),
                          static_cast<int>(frame.width));
  const QRect region = imageRegion.normalized().intersected(imageBounds);
  if (region.isEmpty()) {
    return std::nullopt;
  }

  ThermalRegionStatistics statistics;
  statistics.imageRegion = region;
  statistics.minimumCelsius = std::numeric_limits<float>::infinity();
  statistics.maximumCelsius = -std::numeric_limits<float>::infinity();
  statistics.centerCelsius = std::numeric_limits<float>::quiet_NaN();
  double sum = 0.0;

  for (int imageY = region.top(); imageY <= region.bottom(); ++imageY) {
    for (int imageX = region.left(); imageX <= region.right(); ++imageX) {
      const QPoint imagePoint(imageX, imageY);
      const std::optional<ThermalPointMeasurement> measurement =
          measureThermalPoint(frame, imagePoint);
      if (!measurement.has_value()) {
        continue;
      }

      sum += measurement->celsius;
      ++statistics.sampleCount;
      if (measurement->celsius < statistics.minimumCelsius) {
        statistics.minimumCelsius = measurement->celsius;
        statistics.coldestImagePoint = imagePoint;
      }
      if (measurement->celsius > statistics.maximumCelsius) {
        statistics.maximumCelsius = measurement->celsius;
        statistics.hottestImagePoint = imagePoint;
      }
    }
  }

  if (statistics.sampleCount == 0) {
    return std::nullopt;
  }

  statistics.averageCelsius =
      static_cast<float>(sum / static_cast<double>(statistics.sampleCount));
  const QPoint centerPoint(region.left() + region.width() / 2,
                           region.top() + region.height() / 2);
  const std::optional<ThermalPointMeasurement> center =
      measureThermalPoint(frame, centerPoint);
  if (center.has_value()) {
    statistics.centerCelsius = center->celsius;
  }
  return statistics;
}
