#pragma once

#include <cstddef>
#include <optional>

#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSize>

#include "thermal_processor.hpp"

struct ThermalPointMeasurement final {
  QPoint imagePoint;
  QPoint detectorPoint;
  float celsius = 0.0F;
};

struct ThermalRegionStatistics final {
  QRect imageRegion;
  std::size_t sampleCount = 0;
  float minimumCelsius = 0.0F;
  float maximumCelsius = 0.0F;
  float averageCelsius = 0.0F;
  float centerCelsius = 0.0F;
  QPoint coldestImagePoint;
  QPoint hottestImagePoint;
};

std::optional<QPoint> mapWidgetPointToImage(
    const QPointF& widgetPoint, const QRect& imageTarget,
    const QSize& imageSize) noexcept;
std::optional<ThermalPointMeasurement> measureThermalPoint(
    const ThermalFrame& frame, const QPoint& imagePoint) noexcept;
std::optional<ThermalRegionStatistics> measureThermalRegion(
    const ThermalFrame& frame, const QRect& imageRegion) noexcept;
