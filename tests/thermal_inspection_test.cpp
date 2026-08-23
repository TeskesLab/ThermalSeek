#include "thermal_inspection.hpp"

#include <iostream>
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
  return frame;
}

void testWidgetToImageMapping() {
  const QRect target(10, 20, 200, 300);
  const QSize imageSize(2, 3);

  expect(mapWidgetPointToImage(QPointF(10.0, 20.0), target, imageSize) ==
             QPoint(0, 0),
         "top-left widget point maps to top-left image pixel");
  expect(mapWidgetPointToImage(QPointF(209.9, 319.9), target, imageSize) ==
             QPoint(1, 2),
         "bottom-right widget point maps to bottom-right image pixel");
  expect(!mapWidgetPointToImage(QPointF(210.0, 320.0), target, imageSize)
              .has_value(),
         "exclusive target boundary is outside the image");
  expect(!mapWidgetPointToImage(QPointF(9.9, 100.0), target, imageSize)
              .has_value(),
         "letterboxed area does not map to a thermal pixel");
}

void testPointMeasurementsUseDetectorOrientation() {
  const ThermalFrame frame = sampleFrame();
  const auto cold = measureThermalPoint(frame, QPoint(0, 2));
  expect(cold.has_value() && cold->detectorPoint == QPoint(0, 0) &&
             cold->celsius == 10.0F,
         "rotated cold point maps back to detector coordinates");

  const auto hot = measureThermalPoint(frame, QPoint(1, 0));
  expect(hot.has_value() && hot->detectorPoint == QPoint(2, 1) &&
             hot->celsius == 60.0F,
         "rotated hot point maps back to detector coordinates");

  expect(!measureThermalPoint(frame, QPoint(2, 0)).has_value(),
         "point outside rendered width is rejected");
}

void testRegionStatistics() {
  const ThermalFrame frame = sampleFrame();
  const auto statistics =
      measureThermalRegion(frame, QRect(0, 0, 2, 2));

  expect(statistics.has_value(), "valid image region produces statistics");
  if (!statistics.has_value()) {
    return;
  }
  expect(statistics->imageRegion == QRect(0, 0, 2, 2),
         "region remains in image coordinates");
  expect(statistics->sampleCount == 4,
         "region reports the number of sampled pixels");
  expect(statistics->minimumCelsius == 20.0F &&
             statistics->coldestImagePoint == QPoint(0, 1),
         "region minimum and location are correct");
  expect(statistics->maximumCelsius == 60.0F &&
             statistics->hottestImagePoint == QPoint(1, 0),
         "region maximum and location are correct");
  expect(statistics->averageCelsius == 40.0F,
         "region average uses every selected pixel");
  expect(statistics->centerCelsius == 50.0F,
         "region center uses the selected center pixel");
}
}  // namespace

int main() {
  testWidgetToImageMapping();
  testPointMeasurementsUseDetectorOrientation();
  testRegionStatistics();

  if (failures != 0) {
    std::cerr << failures << " inspection test(s) failed\n";
    return 1;
  }
  std::cout << "All thermal inspection tests passed\n";
  return 0;
}
