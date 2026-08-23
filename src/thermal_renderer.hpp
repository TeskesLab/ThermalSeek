#pragma once

#include <optional>

#include <QImage>
#include <QMetaType>
#include <QPoint>

#include "thermal_palette.hpp"
#include "thermal_processor.hpp"

struct TemperatureRange final {
  float minimumCelsius = 0.0F;
  float maximumCelsius = 0.0F;
};

bool isValidTemperatureRange(const TemperatureRange& range) noexcept;

struct ThermalRenderSettings final {
  ThermalPaletteId palette = ThermalPaletteId::Inferno;
  std::optional<TemperatureRange> fixedRange;
};

struct ThermalRenderResult final {
  QImage image;
  TemperatureRange displayRange;
  float minimumCelsius = 0.0F;
  float maximumCelsius = 0.0F;
  float centerCelsius = 0.0F;
  QPoint coldestPoint{-1, -1};
  QPoint hottestPoint{-1, -1};
};

ThermalRenderResult renderThermalFrame(
    const ThermalFrame& frame, const ThermalRenderSettings& settings);

Q_DECLARE_METATYPE(ThermalRenderResult)
