#include "fixed_pattern_profile_store.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <QByteArray>
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QSaveFile>
#include <QStandardPaths>

namespace {
constexpr qsizetype kChecksumSize = 32;
constexpr qint64 kMaximumProfileFileSize = 64LL * 1024LL * 1024LL;
constexpr quint32 kMaximumProfilePixels = 4U * 1024U * 1024U;
const QByteArray kProfileMagic("TSPFPRF1", 8);

void setError(QString *error, const QString &message) {
  if (error != nullptr) {
    *error = message;
  }
}

void configureStream(QDataStream &stream) {
  stream.setVersion(QDataStream::Qt_6_4);
  stream.setByteOrder(QDataStream::LittleEndian);
  stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
}

bool appendRaw(QDataStream &stream, const char *data, qsizetype size) {
  if (size > std::numeric_limits<int>::max()) {
    return false;
  }
  return stream.writeRawData(data, static_cast<int>(size)) == size;
}

QByteArray serializeProfile(const FixedPatternProfile &profile,
                            QString *error) {
  if (profile.width > std::numeric_limits<quint32>::max() ||
      profile.height > std::numeric_limits<quint32>::max() ||
      profile.biasCelsius.size() > std::numeric_limits<quint32>::max()) {
    setError(error, QStringLiteral("Fixed-pattern profile is too large"));
    return {};
  }

  QByteArray payload;
  QDataStream stream(&payload, QIODevice::WriteOnly);
  configureStream(stream);
  if (!appendRaw(stream, kProfileMagic.constData(), kProfileMagic.size()) ||
      !appendRaw(
          stream,
          reinterpret_cast<const char *>(profile.cameraFingerprint.data()),
          static_cast<qsizetype>(profile.cameraFingerprint.size()))) {
    setError(error, QStringLiteral("Could not serialize profile header"));
    return {};
  }

  stream << static_cast<quint32>(profile.formatVersion)
         << static_cast<quint32>(profile.width)
         << static_cast<quint32>(profile.height)
         << static_cast<quint32>(profile.sampleCount)
         << static_cast<quint32>(profile.shutterCount)
         << profile.metrics.referenceTemperatureCelsius
         << profile.metrics.biasRmsCelsius << profile.metrics.biasP01Celsius
         << profile.metrics.biasP99Celsius
         << profile.metrics.maximumAbsoluteBiasCelsius
         << profile.metrics.lowFrequencySpanCelsius
         << profile.metrics.globalMedianSpanCelsius
         << profile.metrics.temporalNoiseRmsCelsius
         << static_cast<quint32>(profile.biasCelsius.size());
  for (float bias : profile.biasCelsius) {
    stream << bias;
  }
  if (stream.status() != QDataStream::Ok) {
    setError(error,
             QStringLiteral("Could not serialize fixed-pattern profile"));
    return {};
  }
  return payload;
}

FixedPatternProfileLoadResult invalidLoad(const QString &message) {
  FixedPatternProfileLoadResult result;
  result.status = FixedPatternProfileLoadStatus::Invalid;
  result.error = message;
  return result;
}
} // namespace

CameraFingerprint
computeCameraFingerprint(const std::vector<unsigned char> &factoryData,
                         const std::vector<unsigned char> &deviceInfo) {
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!factoryData.empty()) {
    hash.addData(
        QByteArrayView(reinterpret_cast<const char *>(factoryData.data()),
                       static_cast<qsizetype>(factoryData.size())));
  }
  if (!deviceInfo.empty()) {
    hash.addData(
        QByteArrayView(reinterpret_cast<const char *>(deviceInfo.data()),
                       static_cast<qsizetype>(deviceInfo.size())));
  }

  const QByteArray digest = hash.result();
  CameraFingerprint fingerprint{};
  std::copy_n(reinterpret_cast<const std::uint8_t *>(digest.constData()),
              fingerprint.size(), fingerprint.begin());
  return fingerprint;
}

QString cameraFingerprintHex(const CameraFingerprint &fingerprint) {
  return QString::fromLatin1(
      QByteArray(reinterpret_cast<const char *>(fingerprint.data()),
                 static_cast<qsizetype>(fingerprint.size()))
          .toHex());
}

QString fixedPatternProfilePath(const CameraFingerprint &fingerprint) {
  const QString applicationData =
      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  if (applicationData.isEmpty()) {
    return {};
  }
  return QDir(applicationData)
      .filePath(QStringLiteral("fixed-pattern/%1.tspfp")
                    .arg(cameraFingerprintHex(fingerprint)));
}

bool saveFixedPatternProfile(const FixedPatternProfile &profile,
                             const QString &filePath, QString *error) {
  std::string validationError;
  if (!isValidFixedPatternProfile(profile, &validationError)) {
    setError(error,
             QStringLiteral("Cannot save invalid fixed-pattern profile: %1")
                 .arg(QString::fromStdString(validationError)));
    return false;
  }
  if (filePath.isEmpty()) {
    setError(error,
             QStringLiteral("Fixed-pattern profile path is unavailable"));
    return false;
  }

  const QFileInfo fileInfo(filePath);
  if (!QDir().mkpath(fileInfo.absolutePath())) {
    setError(error, QStringLiteral("Cannot create profile directory: %1")
                        .arg(fileInfo.absolutePath()));
    return false;
  }

  QString serializationError;
  QByteArray contents = serializeProfile(profile, &serializationError);
  if (contents.isEmpty()) {
    setError(error, serializationError);
    return false;
  }
  contents.append(
      QCryptographicHash::hash(contents, QCryptographicHash::Sha256));

  QSaveFile file(filePath);
  file.setDirectWriteFallback(false);
  if (!file.open(QIODevice::WriteOnly)) {
    setError(error, QStringLiteral("Cannot open profile for writing: %1")
                        .arg(file.errorString()));
    return false;
  }
  if (file.write(contents) != contents.size()) {
    const QString writeError = file.errorString();
    file.cancelWriting();
    setError(error, QStringLiteral("Cannot write profile: %1").arg(writeError));
    return false;
  }
  if (!file.commit()) {
    setError(error, QStringLiteral("Cannot commit profile atomically: %1")
                        .arg(file.errorString()));
    return false;
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

FixedPatternProfileLoadResult
loadFixedPatternProfile(const QString &filePath,
                        const CameraFingerprint &expectedFingerprint) {
  if (filePath.isEmpty()) {
    return invalidLoad(
        QStringLiteral("Fixed-pattern profile path is unavailable"));
  }
  if (!QFileInfo::exists(filePath)) {
    return {};
  }

  QFile file(filePath);
  if (!file.open(QIODevice::ReadOnly)) {
    return invalidLoad(QStringLiteral("Cannot read fixed-pattern profile: %1")
                           .arg(file.errorString()));
  }
  if (file.size() <= kChecksumSize || file.size() > kMaximumProfileFileSize) {
    return invalidLoad(
        QStringLiteral("Fixed-pattern profile file size is invalid"));
  }
  const QByteArray contents = file.readAll();
  if (contents.size() != file.size()) {
    return invalidLoad(
        QStringLiteral("Could not read the complete fixed-pattern profile"));
  }

  const qsizetype payloadSize = contents.size() - kChecksumSize;
  const QByteArray payload = contents.first(payloadSize);
  const QByteArray storedChecksum = contents.sliced(payloadSize);
  const QByteArray calculatedChecksum =
      QCryptographicHash::hash(payload, QCryptographicHash::Sha256);
  if (storedChecksum != calculatedChecksum) {
    return invalidLoad(
        QStringLiteral("Fixed-pattern profile checksum is invalid"));
  }

  QDataStream stream(payload);
  configureStream(stream);
  std::array<char, 8> magic{};
  if (stream.readRawData(magic.data(), static_cast<int>(magic.size())) !=
          static_cast<int>(magic.size()) ||
      QByteArray(magic.data(), static_cast<qsizetype>(magic.size())) !=
          kProfileMagic) {
    return invalidLoad(
        QStringLiteral("Fixed-pattern profile header is invalid"));
  }

  FixedPatternProfile profile;
  if (stream.readRawData(
          reinterpret_cast<char *>(profile.cameraFingerprint.data()),
          static_cast<int>(profile.cameraFingerprint.size())) !=
      static_cast<int>(profile.cameraFingerprint.size())) {
    return invalidLoad(
        QStringLiteral("Fixed-pattern profile fingerprint is incomplete"));
  }

  quint32 formatVersion = 0;
  quint32 width = 0;
  quint32 height = 0;
  quint32 sampleCount = 0;
  quint32 shutterCount = 0;
  quint32 biasCount = 0;
  stream >> formatVersion >> width >> height >> sampleCount >> shutterCount >>
      profile.metrics.referenceTemperatureCelsius >>
      profile.metrics.biasRmsCelsius >> profile.metrics.biasP01Celsius >>
      profile.metrics.biasP99Celsius >>
      profile.metrics.maximumAbsoluteBiasCelsius >>
      profile.metrics.lowFrequencySpanCelsius >>
      profile.metrics.globalMedianSpanCelsius >>
      profile.metrics.temporalNoiseRmsCelsius >> biasCount;
  if (stream.status() != QDataStream::Ok || width == 0 || height == 0 ||
      width > kMaximumProfilePixels || height > kMaximumProfilePixels ||
      static_cast<quint64>(width) * static_cast<quint64>(height) !=
          static_cast<quint64>(biasCount) ||
      biasCount > kMaximumProfilePixels) {
    return invalidLoad(
        QStringLiteral("Fixed-pattern profile dimensions are invalid"));
  }
  if (profile.cameraFingerprint != expectedFingerprint) {
    return invalidLoad(
        QStringLiteral("Fixed-pattern profile belongs to another camera"));
  }

  profile.formatVersion = formatVersion;
  profile.width = width;
  profile.height = height;
  profile.sampleCount = sampleCount;
  profile.shutterCount = shutterCount;
  profile.biasCelsius.resize(biasCount);
  for (float &bias : profile.biasCelsius) {
    stream >> bias;
  }
  if (stream.status() != QDataStream::Ok || !stream.atEnd()) {
    return invalidLoad(
        QStringLiteral("Fixed-pattern profile payload is incomplete"));
  }

  std::string validationError;
  if (!isValidFixedPatternProfile(profile, &validationError)) {
    return invalidLoad(QStringLiteral("Fixed-pattern profile is invalid: %1")
                           .arg(QString::fromStdString(validationError)));
  }

  FixedPatternProfileLoadResult result;
  result.status = FixedPatternProfileLoadStatus::Loaded;
  result.profile = std::move(profile);
  return result;
}

bool removeFixedPatternProfile(const QString &filePath, QString *error) {
  if (filePath.isEmpty()) {
    setError(error,
             QStringLiteral("Fixed-pattern profile path is unavailable"));
    return false;
  }
  if (!QFileInfo::exists(filePath)) {
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }
  if (!QFile::remove(filePath)) {
    setError(error, QStringLiteral("Cannot remove fixed-pattern profile: %1")
                        .arg(filePath));
    return false;
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}
