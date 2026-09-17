#pragma once

#include <memory>
#include <optional>

#include <QImage>
#include <QMetaType>
#include <QPoint>

#include "radiometric_correction.hpp"
#include "thermal_palette.hpp"
#include "thermal_frame.hpp"

struct TemperatureRange final {
  float minimumCelsius = 0.0F;
  float maximumCelsius = 0.0F;
};

bool isValidTemperatureRange(const TemperatureRange &range) noexcept;

struct ThermalRenderSettings final {
  ThermalPaletteId palette = ThermalPaletteId::Inferno;
  std::optional<TemperatureRange> fixedRange;
  RadiometricSettings radiometry;
  ThermalOrientation orientation = ThermalOrientation::CounterClockwise;
};

struct ThermalRenderResult final {
  std::shared_ptr<const ThermalFrame> cameraFrame;
  std::shared_ptr<const ThermalFrame> apparentFrame;
  std::shared_ptr<const ThermalFrame> measurementFrame;
  RadiometricSettings radiometricSettings;
  ThermalOrientation orientation = ThermalOrientation::CounterClockwise;
  bool fixedPatternApplied = false;
  QImage image;
  TemperatureRange displayRange;
  float minimumCelsius = 0.0F;
  float maximumCelsius = 0.0F;
  float centerCelsius = 0.0F;
  QPoint coldestPoint{-1, -1};
  QPoint hottestPoint{-1, -1};
};

ThermalRenderResult renderThermalFrame(
    std::shared_ptr<const ThermalFrame> cameraFrame,
    std::shared_ptr<const ThermalFrame> apparentFrame,
    const ThermalRenderSettings &settings,
    std::shared_ptr<ThermalFrame> radiometricCorrectionBuffer = nullptr);

Q_DECLARE_METATYPE(ThermalRenderResult)
