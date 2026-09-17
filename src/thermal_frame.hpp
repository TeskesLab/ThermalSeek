#pragma once

#include <cstddef>
#include <vector>

enum class ThermalOrientation {
  Native,
  CounterClockwise,
};

struct ThermalFrame final {
  std::size_t width = 0;
  std::size_t height = 0;
  std::vector<float> celsius;
  float minimumCelsius = 0.0F;
  float maximumCelsius = 0.0F;
  float centerCelsius = 0.0F;
  std::size_t minimumX = 0;
  std::size_t minimumY = 0;
  std::size_t maximumX = 0;
  std::size_t maximumY = 0;
};
