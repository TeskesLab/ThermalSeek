#include "thermal_inspection.hpp"
#include "thermal_palette.hpp"
#include "thermal_renderer.hpp"

#include <cmath>
#include <iostream>
#include <memory>
#include <string>

namespace {
int failures = 0;

void expect(bool condition, const std::string &message) {
  if (condition) {
    return;
  }
  std::cerr << "FAIL: " << message << '\n';
  ++failures;
}
bool nearlyEqual(float actual, float expected, float tolerance = 0.0001F) {
  return std::abs(actual - expected) <= tolerance;
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
  const auto &descriptors = thermalPaletteDescriptors();
  expect(descriptors.size() == kThermalPaletteCount,
         "palette registry exposes every palette");

  const auto &whiteHot = thermalPalette(ThermalPaletteId::WhiteHot);
  expect(whiteHot.front() == qRgb(0, 0, 0), "white-hot starts at black");
  expect(whiteHot.back() == qRgb(255, 255, 255), "white-hot ends at white");

  const auto &blackHot = thermalPalette(ThermalPaletteId::BlackHot);
  expect(blackHot.front() == qRgb(255, 255, 255), "black-hot starts at white");
  expect(blackHot.back() == qRgb(0, 0, 0), "black-hot ends at black");
}

void testAutomaticRangeAndMarkerMapping() {
  ThermalRenderSettings settings;
  settings.palette = ThermalPaletteId::WhiteHot;
  const auto frame = std::make_shared<ThermalFrame>(sampleFrame());
  const ThermalRenderResult result = renderThermalFrame(frame, frame, settings);

  expect(result.cameraFrame == frame && result.apparentFrame == frame &&
             result.measurementFrame == frame && !result.fixedPatternApplied,
         "identity corrections reuse the original camera frame");
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

void testNativeRenderingAndMeasurementsAgree() {
  ThermalRenderSettings settings;
  settings.palette = ThermalPaletteId::WhiteHot;
  settings.orientation = ThermalOrientation::Native;
  const auto frame = std::make_shared<ThermalFrame>(sampleFrame());
  const ThermalRenderResult result = renderThermalFrame(frame, frame, settings);
  expect(result.image.size() == QSize(3, 2),
         "native non-square frame keeps detector image dimensions");
  if (result.image.size() != QSize(3, 2) || !result.measurementFrame) {
    return;
  }
  expect(result.coldestPoint == QPoint(0, 0) &&
             result.hottestPoint == QPoint(2, 1),
         "native extrema retain their detector coordinates");
  const auto cold = measureThermalPoint(
      *result.measurementFrame, result.coldestPoint, result.orientation);
  const auto hot = measureThermalPoint(
      *result.measurementFrame, result.hottestPoint, result.orientation);
  expect(cold.has_value() && cold->celsius == result.minimumCelsius &&
             hot.has_value() && hot->celsius == result.maximumCelsius,
         "displayed native extrema agree with spot measurements");
  for (int y = 0; y < result.image.height(); ++y) {
    for (int x = 0; x < result.image.width(); ++x) {
      const auto point = measureThermalPoint(
          *result.measurementFrame, QPoint(x, y), result.orientation);
      const int expectedGray = (y * 3 + x) * 51;
      expect(point.has_value() && point->detectorPoint == QPoint(x, y) &&
                 point->celsius == 10.0F * (y * 3 + x + 1) &&
                 result.image.pixel(x, y) ==
                     qRgb(expectedGray, expectedGray, expectedGray),
             "native rendered pixels and temperatures share one coordinate system");
    }
  }
  const auto region = measureThermalRegion(
      *result.measurementFrame, QRect(0, 0, 3, 2), result.orientation);
  expect(region.has_value() && region->sampleCount == 6 &&
             region->minimumCelsius == result.minimumCelsius &&
             region->maximumCelsius == result.maximumCelsius &&
             region->coldestImagePoint == result.coldestPoint &&
             region->hottestImagePoint == result.hottestPoint &&
             region->averageCelsius == 35.0F && region->centerCelsius == 50.0F,
         "native ROI temperatures and extrema agree with the rendered frame");
  const auto rightColumn = measureThermalRegion(
      *result.measurementFrame, QRect(2, 0, 1, 2), result.orientation);
  expect(rightColumn.has_value() && rightColumn->sampleCount == 2 &&
             rightColumn->minimumCelsius == 30.0F &&
             rightColumn->maximumCelsius == 60.0F &&
             rightColumn->averageCelsius == 45.0F &&
             rightColumn->hottestImagePoint == result.hottestPoint,
         "native ROI uses the unrotated right edge of a non-square frame");
}

void testFixedRangeClamping() {
  ThermalRenderSettings settings;
  settings.palette = ThermalPaletteId::WhiteHot;
  settings.fixedRange = TemperatureRange{20.0F, 40.0F};
  const auto frame = std::make_shared<ThermalFrame>(sampleFrame());
  const ThermalRenderResult result = renderThermalFrame(frame, frame, settings);

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
  const auto frame = std::make_shared<ThermalFrame>(sampleFrame());
  const ThermalRenderResult result = renderThermalFrame(frame, frame, settings);

  expect(result.displayRange.minimumCelsius == 10.0F &&
             result.displayRange.maximumCelsius == 60.0F,
         "invalid fixed range falls back to frame extrema");
}

void testRadiometricCorrectionDrivesRendering() {
  ThermalRenderSettings settings;
  settings.palette = ThermalPaletteId::WhiteHot;
  settings.radiometry = RadiometricSettings{0.8F, 20.0F};
  const auto frame = std::make_shared<ThermalFrame>(sampleFrame());
  const auto correctionBuffer = std::make_shared<ThermalFrame>();
  const ThermalRenderResult result =
      renderThermalFrame(frame, frame, settings, correctionBuffer);

  const float expectedMinimum =
      correctApparentTemperature(10.0F, settings.radiometry);
  const float expectedMaximum =
      correctApparentTemperature(60.0F, settings.radiometry);
  expect(result.apparentFrame == frame &&
             result.measurementFrame == correctionBuffer,
         "non-identity radiometry preserves the apparent frame and reuses "
         "the supplied correction buffer");
  expect(result.radiometricSettings.emissivity == 0.8F &&
             result.radiometricSettings.reflectedTemperatureCelsius == 20.0F,
         "render result identifies the correction applied to its pixels");
  expect(result.measurementFrame &&
             nearlyEqual(result.measurementFrame->celsius.front(),
                         expectedMinimum),
         "inspection frame contains corrected measurements");
  expect(nearlyEqual(result.minimumCelsius, expectedMinimum) &&
             nearlyEqual(result.maximumCelsius, expectedMaximum),
         "frame statistics use corrected temperatures");
  expect(nearlyEqual(result.displayRange.minimumCelsius, expectedMinimum) &&
             nearlyEqual(result.displayRange.maximumCelsius, expectedMaximum),
         "automatic display range uses corrected extrema");
  expect(result.coldestPoint == QPoint(0, 2) &&
             result.hottestPoint == QPoint(1, 0),
         "corrected extrema drive the rendered markers");
  expect(result.image.pixel(0, 2) == qRgb(0, 0, 0) &&
             result.image.pixel(1, 0) == qRgb(255, 255, 255),
         "corrected range maps to the full palette");
}

void testFixedPatternSourceIsPreserved() {
  ThermalRenderSettings settings;
  const auto cameraFrame = std::make_shared<ThermalFrame>(sampleFrame());
  const auto apparentFrame = std::make_shared<ThermalFrame>(sampleFrame());
  apparentFrame->celsius.front() = 11.0F;
  apparentFrame->minimumCelsius = 11.0F;

  const ThermalRenderResult result =
      renderThermalFrame(cameraFrame, apparentFrame, settings);
  expect(result.cameraFrame == cameraFrame &&
             result.apparentFrame == apparentFrame &&
             result.measurementFrame == apparentFrame,
         "renderer preserves camera and fixed-pattern-corrected frames");
  expect(result.fixedPatternApplied,
         "renderer reports that fixed-pattern correction was applied");
}
} // namespace

int main() {
  testPaletteRegistry();
  testAutomaticRangeAndMarkerMapping();
  testNativeRenderingAndMeasurementsAgree();
  testFixedRangeClamping();
  testInvalidFixedRangeFallsBackToAutomatic();
  testRadiometricCorrectionDrivesRendering();
  testFixedPatternSourceIsPreserved();

  if (failures != 0) {
    std::cerr << failures << " renderer test(s) failed\n";
    return 1;
  }
  std::cout << "All thermal renderer tests passed\n";
  return 0;
}
