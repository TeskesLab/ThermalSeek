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

constexpr std::array<ColorAnchor, 11> kColorAnchors{{
    {0, 0, 0, 4},       {28, 22, 11, 57},   {57, 66, 10, 104},
    {85, 106, 23, 110}, {113, 147, 38, 103}, {142, 188, 55, 84},
    {170, 221, 81, 58}, {198, 243, 120, 25}, {227, 252, 165, 10},
    {244, 246, 215, 70}, {255, 252, 255, 164},
}};

int interpolateChannel(int first, int second, std::size_t position,
                       std::size_t span) {
  const int integerPosition = static_cast<int>(position);
  const int integerSpan = static_cast<int>(span);
  return (first * (integerSpan - integerPosition) +
          second * integerPosition + integerSpan / 2) /
         integerSpan;
}
}  // namespace

const std::array<QRgb, 256>& thermalPalette() noexcept {
  static const std::array<QRgb, 256> palette = [] {
    std::array<QRgb, 256> colors{};
    for (std::size_t segment = 0; segment + 1 < kColorAnchors.size();
         ++segment) {
      const ColorAnchor& first = kColorAnchors[segment];
      const ColorAnchor& second = kColorAnchors[segment + 1];
      const std::size_t span = second.index - first.index;
      for (std::size_t index = first.index; index <= second.index; ++index) {
        const std::size_t position = index - first.index;
        colors[index] = qRgb(
            interpolateChannel(first.red, second.red, position, span),
            interpolateChannel(first.green, second.green, position, span),
            interpolateChannel(first.blue, second.blue, position, span));
      }
    }
    return colors;
  }();
  return palette;
}
