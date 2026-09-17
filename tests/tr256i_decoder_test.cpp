#include "tr256i_decoder.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr std::size_t width = 256;
constexpr std::size_t height = 192;
constexpr std::size_t planeBytes = width * height * 2;
constexpr std::size_t frameBytes = planeBytes * 2;
int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool nearlyEqual(float actual, float expected) {
  return std::abs(actual - expected) <= 0.0001F;
}

void putSample(std::vector<unsigned char> &bytes, std::size_t offset,
               std::uint16_t sample) {
  bytes[offset] = static_cast<unsigned char>(sample & 0xff);
  bytes[offset + 1] = static_cast<unsigned char>(sample >> 8);
}

std::vector<unsigned char> makeFrame(std::uint16_t temperature) {
  std::vector<unsigned char> bytes(frameBytes, 0);
  for (std::size_t offset = planeBytes; offset < frameBytes; offset += 2) {
    putSample(bytes, offset, temperature);
  }
  return bytes;
}

void testStartupVersusUniformHotScene() {
  auto bytes = makeFrame(0x8000);
  for (std::size_t offset = 0; offset < planeBytes; offset += 2) {
    putSample(bytes, offset, 0x8000);
  }
  ThermalFrame frame;
  expect(!decodeTr256iFrame(bytes.data(), bytes.size(), frame),
         "complete dual-plane 0x8000 startup frame is not a measurement");

  bytes[planeBytes - 2] = 1;
  expect(decodeTr256iFrame(bytes.data(), bytes.size(), frame),
         "uniform 238.85 C thermal plane with a different image is real data");
  expect(nearlyEqual(frame.minimumCelsius, 238.85F) &&
             nearlyEqual(frame.maximumCelsius, 238.85F) &&
             nearlyEqual(frame.centerCelsius, 238.85F),
         "real uniform hot scene retains its measured temperature");
}

void testPlaneEndianStatisticsAndCenter() {
  // Zero image-plane samples are legal and must never be decoded as temperatures.
  auto bytes = makeFrame(0x4b12);
  putSample(bytes, planeBytes + (17 * width + 31) * 2, 0x4501);
  putSample(bytes, planeBytes + (191 * width + 255) * 2, 0x5002);
  putSample(bytes, planeBytes + (96 * width + 128) * 2, 0x4c03);
  ThermalFrame frame;
  expect(decodeTr256iFrame(bytes.data(), bytes.size(), frame),
         "dual-plane frame decodes its lower temperature plane");
  expect(frame.width == width && frame.height == height &&
             frame.celsius.size() == width * height &&
             nearlyEqual(frame.celsius[0], 27.13125F),
         "native thermal pixels use lower-plane offset and little-endian Kelvin/64");
  expect(nearlyEqual(frame.minimumCelsius, 2.865625F) &&
             frame.minimumX == 31 && frame.minimumY == 17,
         "minimum temperature and native location come from thermal pixels");
  expect(nearlyEqual(frame.maximumCelsius, 46.88125F) &&
             frame.maximumX == 255 && frame.maximumY == 191,
         "maximum includes the final thermal pixel");
  expect(nearlyEqual(frame.centerCelsius, 30.896875F),
         "center is the single native pixel at row 96 column 128, not an average");
}

void expectRejected(const unsigned char *bytes, std::size_t size,
                    const std::string &message) {
  ThermalFrame frame;
  bool rejected = false;
  try {
    decodeTr256iFrame(bytes, size, frame);
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  expect(rejected, message);
}

void testMalformedAndInvalidSamples() {
  auto bytes = makeFrame(0x4b12);
  expectRejected(nullptr, frameBytes, "null frame data is rejected");
  expectRejected(bytes.data(), frameBytes - 1, "truncated frame is rejected");
  bytes.push_back(0);
  expectRejected(bytes.data(), bytes.size(), "oversized frame is rejected");
  bytes.pop_back();

  putSample(bytes, planeBytes, 0);
  expectRejected(bytes.data(), bytes.size(),
                 "zero thermal sample fails instead of substituting 25 C");
  putSample(bytes, planeBytes, 1);
  putSample(bytes, frameBytes - 2, 65001);
  expectRejected(bytes.data(), bytes.size(),
                 "thermal sample above 65000 at final pixel is rejected");
  putSample(bytes, frameBytes - 2, 65000);
  ThermalFrame frame;
  expect(decodeTr256iFrame(bytes.data(), bytes.size(), frame) &&
             nearlyEqual(frame.minimumCelsius, -273.134375F) &&
             nearlyEqual(frame.maximumCelsius, 742.475F),
         "valid raw endpoints 1 and 65000 are not rejected or replaced");
}
}  // namespace

int main() {
  testStartupVersusUniformHotScene();
  testPlaneEndianStatisticsAndCenter();
  testMalformedAndInvalidSamples();
  if (failures != 0) {
    std::cerr << failures << " TR256i decoder test(s) failed\n";
    return 1;
  }
  std::cout << "All TR256i decoder tests passed\n";
  return 0;
}
