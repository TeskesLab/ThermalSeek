#include "thermal_palette.hpp"
#include "thermal_renderer.hpp"

#include <iostream>
#include <memory>
#include <string>

namespace {
int failures = 0;

void expect(bool condition, const std::string& message) {
  if (condition) {
    return;
  }
  std::cerr << "FAIL: " << message << '\n';
  ++failures;
}

ThermalFrame sampleFrame() {
  ThermalFrame frame;
  frame.width = 3;
  frame.height = 2;
  frame.celsius = {10.0F, 20.0F, 30.0F, 40.0F, 50.0F, 60.0F};
  frame.minimumCelsius = 10.0F;
  frame.maximumCelsius = 60.0F;
  frame.centerCelsius = 30.0F;
  frame.minimumX = 0;
  frame.minimumY = 0;
  frame.maximumX = 2;
  frame.maximumY = 1;
  return frame;
}

void testPaletteRegistry() {
  const auto& descriptors = thermalPaletteDescriptors();
  expect(descriptors.size() == kThermalPaletteCount,
         "palette registry exposes every palette");

  const auto& whiteHot = thermalPalette(ThermalPaletteId::WhiteHot);
  expect(whiteHot.front() == qRgb(0, 0, 0),
         "white-hot starts at black");
  expect(whiteHot.back() == qRgb(255, 255, 255),
         "white-hot ends at white");

  const auto& blackHot = thermalPalette(ThermalPaletteId::BlackHot);
  expect(blackHot.front() == qRgb(255, 255, 255),
         "black-hot starts at white");
  expect(blackHot.back() == qRgb(0, 0, 0),
         "black-hot ends at black");
}

void testAutomaticRangeAndMarkerMapping() {
  ThermalRenderSettings settings;
  settings.palette = ThermalPaletteId::WhiteHot;
  const ThermalRenderResult result = renderThermalFrame(
      std::make_shared<ThermalFrame>(sampleFrame()), settings);

  expect(!result.image.isNull(), "valid thermal frame renders an image");
  expect(result.image.size() == QSize(2, 3),
         "renderer preserves the camera orientation transform");
  expect(result.displayRange.minimumCelsius == 10.0F &&
             result.displayRange.maximumCelsius == 60.0F,
         "automatic range uses frame extrema");
  expect(result.coldestPoint == QPoint(0, 2),
         "cold marker maps into rendered coordinates");
  expect(result.hottestPoint == QPoint(1, 0),
         "hot marker maps into rendered coordinates");
  expect(result.image.pixel(0, 2) == qRgb(0, 0, 0),
         "automatic range maps minimum to palette start");
  expect(result.image.pixel(1, 0) == qRgb(255, 255, 255),
         "automatic range maps maximum to palette end");
}

void testFixedRangeClamping() {
  ThermalRenderSettings settings;
  settings.palette = ThermalPaletteId::WhiteHot;
  settings.fixedRange = TemperatureRange{20.0F, 40.0F};
  const ThermalRenderResult result = renderThermalFrame(
      std::make_shared<ThermalFrame>(sampleFrame()), settings);

  expect(result.displayRange.minimumCelsius == 20.0F &&
             result.displayRange.maximumCelsius == 40.0F,
         "fixed range is reported to the scale");
  expect(result.minimumCelsius == 10.0F && result.maximumCelsius == 60.0F,
         "fixed range does not alter radiometric statistics");
  expect(result.image.pixel(0, 2) == qRgb(0, 0, 0),
         "temperature below fixed range is clamped low");
  expect(result.image.pixel(0, 0) == qRgb(128, 128, 128),
         "temperature inside fixed range is normalized");
  expect(result.image.pixel(1, 0) == qRgb(255, 255, 255),
         "temperature above fixed range is clamped high");
}

void testInvalidFixedRangeFallsBackToAutomatic() {
  ThermalRenderSettings settings;
  settings.fixedRange = TemperatureRange{50.0F, 40.0F};
  const ThermalRenderResult result = renderThermalFrame(
      std::make_shared<ThermalFrame>(sampleFrame()), settings);

  expect(result.displayRange.minimumCelsius == 10.0F &&
             result.displayRange.maximumCelsius == 60.0F,
         "invalid fixed range falls back to frame extrema");
}
}  // namespace

int main() {
  testPaletteRegistry();
  testAutomaticRangeAndMarkerMapping();
  testFixedRangeClamping();
  testInvalidFixedRangeFallsBackToAutomatic();

  if (failures != 0) {
    std::cerr << failures << " renderer test(s) failed\n";
    return 1;
  }
  std::cout << "All thermal renderer tests passed\n";
  return 0;
}
