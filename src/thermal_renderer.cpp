#include "thermal_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace {
QPoint detectorPointToImage(std::size_t detectorX, std::size_t detectorY,
                            std::size_t detectorWidth) noexcept {
  return QPoint(static_cast<int>(detectorY),
                static_cast<int>(detectorWidth - 1 - detectorX));
}

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
}  // namespace

bool isValidTemperatureRange(const TemperatureRange& range) noexcept {
  return std::isfinite(range.minimumCelsius) &&
         std::isfinite(range.maximumCelsius) &&
         range.maximumCelsius > range.minimumCelsius;
}

ThermalRenderResult renderThermalFrame(
    std::shared_ptr<const ThermalFrame> sourceFrame,
    const ThermalRenderSettings& settings) {
  ThermalRenderResult result;
  result.sourceFrame = std::move(sourceFrame);
  if (!result.sourceFrame) {
    return result;
  }

  const ThermalFrame& frame = *result.sourceFrame;
  result.minimumCelsius = frame.minimumCelsius;
  result.maximumCelsius = frame.maximumCelsius;
  result.centerCelsius = frame.centerCelsius;
  result.displayRange =
      settings.fixedRange.has_value() &&
              isValidTemperatureRange(*settings.fixedRange)
          ? *settings.fixedRange
          : TemperatureRange{frame.minimumCelsius, frame.maximumCelsius};

  if (!hasValidDimensions(frame)) {
    return result;
  }

  result.image = QImage(static_cast<int>(frame.height),
                        static_cast<int>(frame.width), QImage::Format_RGB32);
  if (result.image.isNull()) {
    return result;
  }

  if (frame.minimumX < frame.width && frame.minimumY < frame.height) {
    result.coldestPoint =
        detectorPointToImage(frame.minimumX, frame.minimumY, frame.width);
  }
  if (frame.maximumX < frame.width && frame.maximumY < frame.height) {
    result.hottestPoint =
        detectorPointToImage(frame.maximumX, frame.maximumY, frame.width);
  }

  const auto& palette = thermalPalette(settings.palette);
  if (!isValidTemperatureRange(result.displayRange)) {
    result.image.fill(palette.front());
    return result;
  }

  const float span = result.displayRange.maximumCelsius -
                     result.displayRange.minimumCelsius;
  for (std::size_t detectorX = 0; detectorX < frame.width; ++detectorX) {
    auto* destination = reinterpret_cast<QRgb*>(result.image.scanLine(
        static_cast<int>(frame.width - 1 - detectorX)));
    for (std::size_t detectorY = 0; detectorY < frame.height; ++detectorY) {
      const float temperature =
          frame.celsius[detectorY * frame.width + detectorX];
      float normalized = 0.0F;
      if (std::isfinite(temperature)) {
        normalized = std::clamp(
            (temperature - result.displayRange.minimumCelsius) / span, 0.0F,
            1.0F);
      }
      const std::size_t paletteIndex =
          static_cast<std::size_t>(std::lround(normalized * 255.0F));
      destination[detectorY] = palette[paletteIndex];
    }
  }

  return result;
}
