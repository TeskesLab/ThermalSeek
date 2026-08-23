#pragma once

#include <memory>
#include <optional>

#include <QImage>
#include <QMetaType>
#include <QPoint>

#include "thermal_palette.hpp"
#include "radiometric_correction.hpp"
#include "thermal_processor.hpp"

struct TemperatureRange final {
  float minimumCelsius = 0.0F;
  float maximumCelsius = 0.0F;
};

bool isValidTemperatureRange(const TemperatureRange& range) noexcept;

struct ThermalRenderSettings final {
  ThermalPaletteId palette = ThermalPaletteId::Inferno;
  std::optional<TemperatureRange> fixedRange;
  RadiometricSettings radiometry;
};

struct ThermalRenderResult final {
  std::shared_ptr<const ThermalFrame> apparentFrame;
  std::shared_ptr<const ThermalFrame> measurementFrame;
  RadiometricSettings radiometricSettings;
  QImage image;
  TemperatureRange displayRange;
  float minimumCelsius = 0.0F;
  float maximumCelsius = 0.0F;
  float centerCelsius = 0.0F;
  QPoint coldestPoint{-1, -1};
  QPoint hottestPoint{-1, -1};
};

ThermalRenderResult renderThermalFrame(
    std::shared_ptr<const ThermalFrame> apparentFrame,
    const ThermalRenderSettings& settings,
    std::shared_ptr<ThermalFrame> correctionBuffer = nullptr);

Q_DECLARE_METATYPE(ThermalRenderResult)
