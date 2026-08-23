#include "fixed_pattern_correction.hpp"
#include "fixed_pattern_profile_store.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

namespace {
int failures = 0;
constexpr std::size_t kWidth = 32;
constexpr std::size_t kHeight = 24;

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

CameraFingerprint sampleFingerprint() {
  CameraFingerprint fingerprint{};
  for (std::size_t index = 0; index < fingerprint.size(); ++index) {
    fingerprint[index] = static_cast<std::uint8_t>(index * 7U + 3U);
  }
  return fingerprint;
}

float detectorBias(std::size_t index) {
  const int centered =
      static_cast<int>((index * 37U + (index / kWidth) * 13U) % 101U) - 50;
  return static_cast<float>(centered) * 0.008F;
}

float sceneTemperature(std::size_t x, std::size_t y, std::size_t frameNumber) {
  const float drift =
      static_cast<float>(frameNumber % kFixedPatternCalibrationFrameCount) /
      static_cast<float>(kFixedPatternCalibrationFrameCount - 1) * 0.6F;
  return 30.0F + drift +
         static_cast<float>(x) * 0.5F / static_cast<float>(kWidth - 1) +
         static_cast<float>(y) * 0.3F / static_cast<float>(kHeight - 1);
}

float sampleNoise(std::size_t index, std::size_t frameNumber) {
  const int centered =
      static_cast<int>((frameNumber * 17U + index * 11U) % 9U) - 4;
  return static_cast<float>(centered) * 0.004F;
}

ThermalFrame syntheticFrame(std::size_t frameNumber,
                            bool nonUniformTarget = false) {
  ThermalFrame frame;
  frame.width = kWidth;
  frame.height = kHeight;
  frame.celsius.resize(kWidth * kHeight);
  for (std::size_t y = 0; y < kHeight; ++y) {
    for (std::size_t x = 0; x < kWidth; ++x) {
      const std::size_t index = y * kWidth + x;
      const float target = nonUniformTarget
                               ? 30.0F + static_cast<float>(x) * 8.0F /
                                             static_cast<float>(kWidth - 1)
                               : sceneTemperature(x, y, frameNumber);
      frame.celsius[index] =
          target + detectorBias(index) + sampleNoise(index, frameNumber);
    }
  }
  return frame;
}

FixedPatternCalibrationResult trainSyntheticProfile(
    std::size_t shutterCount = kFixedPatternMinimumShutterCount) {
  FixedPatternCalibrator calibrator;
  for (std::size_t shutter = 0; shutter < shutterCount; ++shutter) {
    calibrator.noteShutterFrame();
  }
  for (std::size_t frame = 0; frame < kFixedPatternCalibrationFrameCount;
       ++frame) {
    calibrator.addFrame(syntheticFrame(frame));
  }
  return calibrator.finalize(sampleFingerprint());
}

float median(std::vector<float> values) {
  const std::size_t middle = values.size() / 2;
  std::nth_element(values.begin(), values.begin() + middle, values.end());
  const float upper = values[middle];
  if ((values.size() & 1U) != 0U) {
    return upper;
  }
  return (*std::max_element(values.begin(), values.begin() + middle) + upper) *
         0.5F;
}

void testCalibrationReducesHeldOutDetectorPattern() {
  const FixedPatternCalibrationResult calibration = trainSyntheticProfile();
  expect(calibration.profile.has_value(),
         "uniform covered-target calibration produces a profile");
  expect(calibration.error.empty(),
         "successful calibration does not report an error");
  if (!calibration.profile.has_value()) {
    return;
  }

  const ThermalFrame heldOut = syntheticFrame(137);
  ThermalFrame corrected;
  applyFixedPatternCorrection(heldOut, *calibration.profile, corrected);

  double beforeSquares = 0.0;
  double afterSquares = 0.0;
  for (std::size_t y = 0; y < kHeight; ++y) {
    for (std::size_t x = 0; x < kWidth; ++x) {
      const std::size_t index = y * kWidth + x;
      const double expected = sceneTemperature(x, y, 137);
      const double before =
          static_cast<double>(heldOut.celsius[index]) - expected;
      const double after =
          static_cast<double>(corrected.celsius[index]) - expected;
      beforeSquares += before * before;
      afterSquares += after * after;
    }
  }
  const double beforeRms =
      std::sqrt(beforeSquares / static_cast<double>(heldOut.celsius.size()));
  const double afterRms =
      std::sqrt(afterSquares / static_cast<double>(heldOut.celsius.size()));
  expect(afterRms < beforeRms * 0.6,
         "profile removes most detector-fixed error on a held-out frame");
  expect(std::abs(median(corrected.celsius) - median(heldOut.celsius)) < 0.1F,
         "zero-centered profile preserves scene median temperature");

  const auto extrema =
      std::minmax_element(corrected.celsius.begin(), corrected.celsius.end());
  const std::size_t minimumIndex =
      static_cast<std::size_t>(extrema.first - corrected.celsius.begin());
  const std::size_t maximumIndex =
      static_cast<std::size_t>(extrema.second - corrected.celsius.begin());
  expect(nearlyEqual(corrected.minimumCelsius, *extrema.first) &&
             nearlyEqual(corrected.maximumCelsius, *extrema.second),
         "correction recalculates temperature extrema");
  expect(corrected.minimumX == minimumIndex % kWidth &&
             corrected.minimumY == minimumIndex / kWidth &&
             corrected.maximumX == maximumIndex % kWidth &&
             corrected.maximumY == maximumIndex / kWidth,
         "correction recalculates extrema coordinates");
  const std::size_t centerIndex = (kHeight / 2) * kWidth + kWidth / 2;
  expect(nearlyEqual(corrected.centerCelsius, corrected.celsius[centerIndex]),
         "correction recalculates center temperature");
}

void testNonUniformTargetIsRejected() {
  FixedPatternCalibrator calibrator;
  for (std::size_t shutter = 0; shutter < kFixedPatternMinimumShutterCount;
       ++shutter) {
    calibrator.noteShutterFrame();
  }
  for (std::size_t frame = 0; frame < kFixedPatternCalibrationFrameCount;
       ++frame) {
    calibrator.addFrame(syntheticFrame(frame, true));
  }

  const FixedPatternCalibrationResult result =
      calibrator.finalize(sampleFingerprint());
  expect(!result.profile.has_value(), "non-uniform covered target is rejected");
  expect(result.metrics.lowFrequencySpanCelsius > 3.0F,
         "rejection reports excessive low-frequency target span");
}

void testInsufficientShutterRefreshesAreRejected() {
  FixedPatternCalibrator calibrator;
  for (std::size_t shutter = 0; shutter < 3; ++shutter) {
    calibrator.noteShutterFrame();
  }
  for (std::size_t frame = 0; frame < kFixedPatternCalibrationFrameCount;
       ++frame) {
    calibrator.addFrame(syntheticFrame(frame));
  }

  expect(!calibrator.isComplete(),
         "frame target alone does not complete calibration");
  const FixedPatternCalibrationResult incomplete =
      calibrator.finalize(sampleFingerprint());
  expect(!incomplete.profile.has_value(),
         "calibration with too few shutter refreshes is rejected");
  expect(incomplete.error.find("shutter refreshes") != std::string::npos,
         "shutter rejection explains the missing requirement");

  calibrator.noteShutterFrame();
  expect(calibrator.isComplete(),
         "calibration completes when the fourth shutter arrives");
  expect(calibrator.finalize(sampleFingerprint()).profile.has_value(),
         "collected frames remain valid while waiting for the fourth shutter");
}

void testProfilePersistenceAndIntegrity() {
  const FixedPatternCalibrationResult calibration = trainSyntheticProfile();
  if (!calibration.profile.has_value()) {
    expect(false, "persistence test requires a valid synthetic profile");
    return;
  }

  QTemporaryDir directory;
  expect(directory.isValid(), "temporary profile directory is available");
  if (!directory.isValid()) {
    return;
  }
  const QString filePath = directory.filePath(QStringLiteral("profile.tspfp"));
  QString error;
  expect(saveFixedPatternProfile(*calibration.profile, filePath, &error),
         "valid profile is saved atomically");
  expect(error.isEmpty(), "successful profile save clears its error");

  const FixedPatternProfileLoadResult loaded =
      loadFixedPatternProfile(filePath, sampleFingerprint());
  expect(loaded.status == FixedPatternProfileLoadStatus::Loaded &&
             loaded.profile.has_value(),
         "saved profile loads for the same camera fingerprint");
  if (loaded.profile.has_value()) {
    expect(loaded.profile->cameraFingerprint ==
                   calibration.profile->cameraFingerprint &&
               loaded.profile->width == calibration.profile->width &&
               loaded.profile->height == calibration.profile->height &&
               loaded.profile->sampleCount ==
                   calibration.profile->sampleCount &&
               loaded.profile->biasCelsius == calibration.profile->biasCelsius,
           "profile round trip preserves identity, metadata, and bias data");
  }

  CameraFingerprint otherFingerprint = sampleFingerprint();
  otherFingerprint.front() ^= 0xFFU;
  const FixedPatternProfileLoadResult mismatch =
      loadFixedPatternProfile(filePath, otherFingerprint);
  expect(mismatch.status == FixedPatternProfileLoadStatus::Invalid &&
             !mismatch.profile.has_value(),
         "profile is rejected for a different camera fingerprint");

  QFile file(filePath);
  expect(file.open(QIODevice::ReadWrite),
         "saved profile can be opened for corruption test");
  if (file.isOpen()) {
    expect(file.seek(12), "corruption test seeks into profile payload");
    char byte = 0;
    expect(file.getChar(&byte), "corruption test reads profile byte");
    expect(file.seek(12), "corruption test rewinds to modified byte");
    expect(file.putChar(static_cast<char>(byte ^ 0x5A)),
           "corruption test writes modified profile byte");
    file.close();
  }
  const FixedPatternProfileLoadResult corrupted =
      loadFixedPatternProfile(filePath, sampleFingerprint());
  expect(corrupted.status == FixedPatternProfileLoadStatus::Invalid &&
             !corrupted.profile.has_value() &&
             corrupted.error.contains(QStringLiteral("checksum")),
         "checksum rejects a corrupted profile payload");

  error = QStringLiteral("stale");
  expect(removeFixedPatternProfile(filePath, &error),
         "profile removal succeeds");
  expect(error.isEmpty() && !QFile::exists(filePath),
         "profile removal clears the file and error state");
  const FixedPatternProfileLoadResult removed =
      loadFixedPatternProfile(filePath, sampleFingerprint());
  expect(removed.status == FixedPatternProfileLoadStatus::NotFound &&
             !removed.profile.has_value(),
         "removed profile loads as absent");
}

void testCameraFingerprintUsesBothCalibrationBlobs() {
  const std::vector<unsigned char> factoryData{1, 2, 3, 4};
  const std::vector<unsigned char> deviceInfo{5, 6, 7};
  const CameraFingerprint original =
      computeCameraFingerprint(factoryData, deviceInfo);

  std::vector<unsigned char> changedFactoryData = factoryData;
  changedFactoryData.back() ^= 1U;
  std::vector<unsigned char> changedDeviceInfo = deviceInfo;
  changedDeviceInfo.back() ^= 1U;
  expect(computeCameraFingerprint(changedFactoryData, deviceInfo) != original,
         "camera fingerprint includes factory calibration data");
  expect(computeCameraFingerprint(factoryData, changedDeviceInfo) != original,
         "camera fingerprint includes device information");
}
} // namespace

int main(int argc, char *argv[]) {
  QCoreApplication application(argc, argv);
  testCalibrationReducesHeldOutDetectorPattern();
  testNonUniformTargetIsRejected();
  testInsufficientShutterRefreshesAreRejected();
  testProfilePersistenceAndIntegrity();
  testCameraFingerprintUsesBothCalibrationBlobs();

  if (failures != 0) {
    std::cerr << failures << " fixed-pattern test(s) failed\n";
    return 1;
  }
  std::cout << "All fixed-pattern correction tests passed\n";
  return 0;
}
