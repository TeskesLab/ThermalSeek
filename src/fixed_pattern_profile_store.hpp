#pragma once

#include <optional>
#include <vector>

#include <QString>

#include "fixed_pattern_correction.hpp"

enum class FixedPatternProfileLoadStatus {
  Loaded,
  NotFound,
  Invalid,
};

struct FixedPatternProfileLoadResult final {
  FixedPatternProfileLoadStatus status =
      FixedPatternProfileLoadStatus::NotFound;
  std::optional<FixedPatternProfile> profile;
  QString error;
};

CameraFingerprint
computeCameraFingerprint(const std::vector<unsigned char> &factoryData,
                         const std::vector<unsigned char> &deviceInfo);
QString cameraFingerprintHex(const CameraFingerprint &fingerprint);
QString fixedPatternProfilePath(const CameraFingerprint &fingerprint);

bool saveFixedPatternProfile(const FixedPatternProfile &profile,
                             const QString &filePath, QString *error = nullptr);
FixedPatternProfileLoadResult
loadFixedPatternProfile(const QString &filePath,
                        const CameraFingerprint &expectedFingerprint);
bool removeFixedPatternProfile(const QString &filePath,
                               QString *error = nullptr);
