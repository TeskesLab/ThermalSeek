#include "thermal_palette.hpp"

#include <array>
#include <cstddef>

namespace {
struct ColorAnchor final {
  std::size_t index;
  int red;
  int green;
  int blue;
};

constexpr std::array<ColorAnchor, 11> kInfernoAnchors{{
    {0, 0, 0, 4},       {28, 22, 11, 57},   {57, 66, 10, 104},
    {85, 106, 23, 110}, {113, 147, 38, 103}, {142, 188, 55, 84},
    {170, 221, 81, 58}, {198, 243, 120, 25}, {227, 252, 165, 10},
    {244, 246, 215, 70}, {255, 252, 255, 164},
}};

constexpr std::array<ColorAnchor, 6> kIronAnchors{{
    {0, 0, 0, 0},       {51, 54, 0, 72},    {102, 142, 0, 74},
    {153, 224, 42, 20},  {204, 255, 166, 0}, {255, 255, 255, 255},
}};

constexpr std::array<ColorAnchor, 5> kViridisAnchors{{
    {0, 68, 1, 84},
    {64, 59, 82, 139},
    {128, 33, 145, 140},
    {192, 94, 201, 98},
    {255, 253, 231, 37},
}};

int interpolateChannel(int first, int second, std::size_t position,
                       std::size_t span) noexcept {
  const int integerPosition = static_cast<int>(position);
  const int integerSpan = static_cast<int>(span);
  return (first * (integerSpan - integerPosition) +
          second * integerPosition + integerSpan / 2) /
         integerSpan;
}

template <std::size_t AnchorCount>
std::array<QRgb, 256> createAnchoredPalette(
    const std::array<ColorAnchor, AnchorCount>& anchors) noexcept {
  std::array<QRgb, 256> colors{};
  for (std::size_t segment = 0; segment + 1 < anchors.size(); ++segment) {
    const ColorAnchor& first = anchors[segment];
    const ColorAnchor& second = anchors[segment + 1];
    const std::size_t span = second.index - first.index;
    for (std::size_t index = first.index; index <= second.index; ++index) {
      const std::size_t position = index - first.index;
      colors[index] =
          qRgb(interpolateChannel(first.red, second.red, position, span),
               interpolateChannel(first.green, second.green, position, span),
               interpolateChannel(first.blue, second.blue, position, span));
    }
  }
  return colors;
}

std::array<QRgb, 256> createGrayscalePalette(bool inverted) noexcept {
  std::array<QRgb, 256> colors{};
  for (std::size_t index = 0; index < colors.size(); ++index) {
    const int value =
        inverted ? 255 - static_cast<int>(index) : static_cast<int>(index);
    colors[index] = qRgb(value, value, value);
  }
  return colors;
}
}  // namespace

const std::array<ThermalPaletteDescriptor, kThermalPaletteCount>&
thermalPaletteDescriptors() noexcept {
  static constexpr std::array<ThermalPaletteDescriptor, kThermalPaletteCount>
      descriptors{{
          {ThermalPaletteId::Inferno, "Inferno"},
          {ThermalPaletteId::Iron, "Iron"},
          {ThermalPaletteId::WhiteHot, "White Hot"},
          {ThermalPaletteId::BlackHot, "Black Hot"},
          {ThermalPaletteId::Viridis, "Viridis"},
      }};
  return descriptors;
}

const std::array<QRgb, 256>&
thermalPalette(ThermalPaletteId paletteId) noexcept {
  static const std::array<QRgb, 256> inferno =
      createAnchoredPalette(kInfernoAnchors);
  static const std::array<QRgb, 256> iron =
      createAnchoredPalette(kIronAnchors);
  static const std::array<QRgb, 256> whiteHot =
      createGrayscalePalette(false);
  static const std::array<QRgb, 256> blackHot =
      createGrayscalePalette(true);
  static const std::array<QRgb, 256> viridis =
      createAnchoredPalette(kViridisAnchors);

  switch (paletteId) {
    case ThermalPaletteId::Inferno:
      return inferno;
    case ThermalPaletteId::Iron:
      return iron;
    case ThermalPaletteId::WhiteHot:
      return whiteHot;
    case ThermalPaletteId::BlackHot:
      return blackHot;
    case ThermalPaletteId::Viridis:
      return viridis;
  }
  return inferno;
}
