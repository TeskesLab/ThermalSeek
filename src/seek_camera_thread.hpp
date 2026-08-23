#pragma once

#include <QImage>
#include <QString>
#include <QThread>

class SeekCameraThread final : public QThread {
  Q_OBJECT

public:
  explicit SeekCameraThread(QObject* parent = nullptr);
  ~SeekCameraThread() override;

  void stop();

signals:
  void cameraConnected(const QString& cameraName);
  void frameReady(const QImage& image, float minimumCelsius,
                  float maximumCelsius, float centerCelsius);
  void captureFailed(const QString& message);

protected:
  void run() override;
};
