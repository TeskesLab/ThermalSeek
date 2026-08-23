#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <QColor>

enum class ThermalPaletteId : std::uint8_t {
  Inferno,
  Iron,
  WhiteHot,
  BlackHot,
  Viridis,
};

struct ThermalPaletteDescriptor final {
  ThermalPaletteId id;
  std::string_view name;
};

inline constexpr std::size_t kThermalPaletteCount = 5;

const std::array<ThermalPaletteDescriptor, kThermalPaletteCount>&
thermalPaletteDescriptors() noexcept;
const std::array<QRgb, 256>&
thermalPalette(ThermalPaletteId paletteId) noexcept;
