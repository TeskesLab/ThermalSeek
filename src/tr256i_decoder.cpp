#include "tr256i_decoder.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
constexpr std::size_t width = 256;
constexpr std::size_t height = 192;
constexpr std::size_t planeBytes = width * height * 2;
constexpr std::size_t frameBytes = planeBytes * 2;

std::uint16_t readLittleEndian(const unsigned char *data) noexcept {
  return static_cast<std::uint16_t>(data[0]) |
         (static_cast<std::uint16_t>(data[1]) << 8);
}
}  // namespace

bool decodeTr256iFrame(const unsigned char *data, std::size_t size,
                       ThermalFrame &frame) {
  if (data == nullptr || size != frameBytes) {
    throw std::runtime_error(
        "TR256i requires a complete 196608-byte YUYV 256x384 frame; received " +
        std::to_string(size) + " bytes. Reopen the camera in radiometric mode.");
  }

  bool placeholder = true;
  for (std::size_t offset = 0; offset < frameBytes; offset += 2) {
    if (readLittleEndian(data + offset) != 0x8000) {
      placeholder = false;
      break;
    }
  }
  if (placeholder) {
    return false;
  }

  frame.width = width;
  frame.height = height;
  frame.celsius.resize(width * height);
  frame.minimumCelsius = std::numeric_limits<float>::infinity();
  frame.maximumCelsius = -std::numeric_limits<float>::infinity();
  for (std::size_t index = 0; index < width * height; ++index) {
    const auto sample = readLittleEndian(data + planeBytes + index * 2);
    if (sample == 0 || sample > 65000) {
      throw std::runtime_error(
          "TR256i returned invalid radiometric sample " +
          std::to_string(sample) + " at (" + std::to_string(index % width) +
          ", " + std::to_string(index / width) +
          "). Reconnect the camera; no replacement temperature was generated.");
    }
    const float celsius = static_cast<float>(sample) / 64.0F - 273.15F;
    frame.celsius[index] = celsius;
    if (celsius < frame.minimumCelsius) {
      frame.minimumCelsius = celsius;
      frame.minimumX = index % width;
      frame.minimumY = index / width;
    }
    if (celsius > frame.maximumCelsius) {
      frame.maximumCelsius = celsius;
      frame.maximumX = index % width;
      frame.maximumY = index / width;
    }
  }
  frame.centerCelsius = frame.celsius[(height / 2) * width + width / 2];
  return true;
}
