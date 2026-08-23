#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

struct libusb_context;
struct libusb_device_handle;

class SeekCompactUsbError final : public std::runtime_error {
 public:
  SeekCompactUsbError(const std::string& operation, int errorCode);

  [[nodiscard]] bool isTimeout() const noexcept;

 private:
  int errorCode_;
};

class SeekCompactUsb final {
 public:
  static constexpr std::size_t kFactoryDataSize = 5120;
  static constexpr std::size_t kDeviceInfoSize = 64;

  SeekCompactUsb();
  ~SeekCompactUsb();

  SeekCompactUsb(const SeekCompactUsb&) = delete;
  SeekCompactUsb& operator=(const SeekCompactUsb&) = delete;
  SeekCompactUsb(SeekCompactUsb&&) = delete;
  SeekCompactUsb& operator=(SeekCompactUsb&&) = delete;

  void connect();
  void initialize(std::vector<unsigned char>& factoryData,
                  std::vector<unsigned char>& deviceInfo);
  void readFrame(std::vector<unsigned char>& frame);
  void disconnect() noexcept;

  [[nodiscard]] const std::string& name() const noexcept;

 private:
  void deinitialize() noexcept;
  void controlOut(std::uint8_t request, unsigned char* data,
                  std::size_t size);
  void controlIn(std::uint8_t request, unsigned char* data,
                 std::size_t size);

  libusb_context* context_ = nullptr;
  libusb_device_handle* handle_ = nullptr;
  bool interfaceClaimed_ = false;
  std::string name_;
};
