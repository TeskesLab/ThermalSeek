#include "thermal_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace {
QPoint detectorPointToImage(std::size_t detectorX, std::size_t detectorY,
                            std::size_t detectorWidth,
                            ThermalOrientation orientation) noexcept {
  if (orientation == ThermalOrientation::Native) {
    return QPoint(static_cast<int>(detectorX), static_cast<int>(detectorY));
  }
  return QPoint(static_cast<int>(detectorY),
                static_cast<int>(detectorWidth - 1 - detectorX));
}

bool hasValidDimensions(const ThermalFrame &frame) noexcept {
  if (frame.width == 0 || frame.height == 0 ||
      frame.width > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      frame.height >
          static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  if (frame.width > std::numeric_limits<std::size_t>::max() / frame.height) {
    return false;
  }
  return frame.celsius.size() == frame.width * frame.height;
}
} // namespace

bool isValidTemperatureRange(const TemperatureRange &range) noexcept {
  return std::isfinite(range.minimumCelsius) &&
         std::isfinite(range.maximumCelsius) &&
         range.maximumCelsius > range.minimumCelsius;
}

ThermalRenderResult
renderThermalFrame(std::shared_ptr<const ThermalFrame> cameraFrame,
                   std::shared_ptr<const ThermalFrame> apparentFrame,
                   const ThermalRenderSettings &settings,
                   std::shared_ptr<ThermalFrame> radiometricCorrectionBuffer) {
  ThermalRenderResult result;
  result.cameraFrame = std::move(cameraFrame);
  result.apparentFrame = std::move(apparentFrame);
  result.radiometricSettings = settings.radiometry;
  result.orientation = settings.orientation;
  result.fixedPatternApplied =
      result.cameraFrame && result.apparentFrame &&
      result.cameraFrame.get() != result.apparentFrame.get();
  if (!result.cameraFrame || !result.apparentFrame ||
      !hasValidDimensions(*result.cameraFrame) ||
      !hasValidDimensions(*result.apparentFrame) ||
      result.cameraFrame->width != result.apparentFrame->width ||
      result.cameraFrame->height != result.apparentFrame->height) {
    return result;
  }

  if (isIdentityRadiometricCorrection(settings.radiometry)) {
    result.measurementFrame = result.apparentFrame;
  } else {
    if (!radiometricCorrectionBuffer) {
      radiometricCorrectionBuffer = std::make_shared<ThermalFrame>();
    }
    correctThermalFrame(*result.apparentFrame, settings.radiometry,
                        *radiometricCorrectionBuffer);
    result.measurementFrame = std::move(radiometricCorrectionBuffer);
  }

  const ThermalFrame &frame = *result.measurementFrame;
  result.minimumCelsius = frame.minimumCelsius;
  result.maximumCelsius = frame.maximumCelsius;
  result.centerCelsius = frame.centerCelsius;
  result.displayRange =
      settings.fixedRange.has_value() &&
              isValidTemperatureRange(*settings.fixedRange)
          ? *settings.fixedRange
          : TemperatureRange{frame.minimumCelsius, frame.maximumCelsius};

  const bool native = settings.orientation == ThermalOrientation::Native;
  result.image = QImage(static_cast<int>(native ? frame.width : frame.height),
                        static_cast<int>(native ? frame.height : frame.width),
                        QImage::Format_RGB32);
  if (result.image.isNull()) {
    return result;
  }

  if (frame.minimumX < frame.width && frame.minimumY < frame.height) {
    result.coldestPoint =
        detectorPointToImage(frame.minimumX, frame.minimumY, frame.width,
                             settings.orientation);
  }
  if (frame.maximumX < frame.width && frame.maximumY < frame.height) {
    result.hottestPoint =
        detectorPointToImage(frame.maximumX, frame.maximumY, frame.width,
                             settings.orientation);
  }

  const auto &palette = thermalPalette(settings.palette);
  if (!isValidTemperatureRange(result.displayRange)) {
    result.image.fill(palette.front());
    return result;
  }

  const float span =
      result.displayRange.maximumCelsius - result.displayRange.minimumCelsius;
  const auto colorForTemperature = [&](float temperature) {
    float normalized = 0.0F;
    if (std::isfinite(temperature)) {
      normalized = std::clamp(
          (temperature - result.displayRange.minimumCelsius) / span, 0.0F,
          1.0F);
    }
    const std::size_t paletteIndex =
        static_cast<std::size_t>(std::lround(normalized * 255.0F));
    return palette[paletteIndex];
  };
  if (native) {
    for (std::size_t detectorY = 0; detectorY < frame.height; ++detectorY) {
      auto *destination = reinterpret_cast<QRgb *>(
          result.image.scanLine(static_cast<int>(detectorY)));
      const float *source = frame.celsius.data() + detectorY * frame.width;
      for (std::size_t detectorX = 0; detectorX < frame.width; ++detectorX) {
        destination[detectorX] = colorForTemperature(source[detectorX]);
      }
    }
  } else {
    for (std::size_t detectorX = 0; detectorX < frame.width; ++detectorX) {
      auto *destination = reinterpret_cast<QRgb *>(
          result.image.scanLine(static_cast<int>(frame.width - 1 - detectorX)));
      for (std::size_t detectorY = 0; detectorY < frame.height; ++detectorY) {
        destination[detectorY] =
            colorForTemperature(frame.celsius[detectorY * frame.width + detectorX]);
      }
    }
  }

  return result;
}
