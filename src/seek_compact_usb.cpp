#include "seek_compact_usb.hpp"

#include <libusb.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
constexpr std::uint16_t kSeekVendorId = 0x289d;
constexpr std::uint16_t kSeekCompactProductId = 0x0010;
constexpr int kConfiguration = 1;
constexpr int kInterface = 0;
constexpr unsigned char kFrameEndpoint = 0x81;
constexpr unsigned int kTransferTimeoutMilliseconds = 5000;
constexpr int kDeinitializeCommandCount = 3;
constexpr std::size_t kFactoryChunkSize = 64;
constexpr auto kFactoryChunkInterval = std::chrono::milliseconds(10);
constexpr auto kStopSettlingInterval = std::chrono::milliseconds(300);

constexpr std::uint8_t kVendorInterfaceOut =
    LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR |
    LIBUSB_RECIPIENT_INTERFACE;
constexpr std::uint8_t kVendorInterfaceIn =
    LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR |
    LIBUSB_RECIPIENT_INTERFACE;

enum class Request : std::uint8_t {
  status = 0x36,
  operationMode = 0x3c,
  operationModeStatus = 0x3d,
  imageProcessingMode = 0x3e,
  deviceInfo = 0x4e,
  frame = 0x53,
  initialize = 0x54,
  selectDeviceInfo = 0x55,
  selectFactoryData = 0x56,
  factoryData = 0x58,
};

std::string usbErrorMessage(const std::string& operation, int errorCode) {
  return operation + ": " + libusb_error_name(errorCode);
}

std::string requestDescription(const char* direction, std::uint8_t request) {
  std::ostringstream description;
  description << "Seek Compact USB " << direction << " request 0x" << std::hex
              << std::setw(2) << std::setfill('0')
              << static_cast<unsigned int>(request);
  return description.str();
}

void writeLittleEndian16(unsigned char* destination, std::uint16_t value) {
  destination[0] = static_cast<unsigned char>(value);
  destination[1] = static_cast<unsigned char>(value >> 8U);
}

void writeLittleEndian32(unsigned char* destination, std::uint32_t value) {
  destination[0] = static_cast<unsigned char>(value);
  destination[1] = static_cast<unsigned char>(value >> 8U);
  destination[2] = static_cast<unsigned char>(value >> 16U);
  destination[3] = static_cast<unsigned char>(value >> 24U);
}
}  // namespace

SeekCompactUsbError::SeekCompactUsbError(const std::string& operation,
                                         int errorCode)
    : std::runtime_error(usbErrorMessage(operation, errorCode)),
      errorCode_(errorCode) {}

bool SeekCompactUsbError::isTimeout() const noexcept {
  return errorCode_ == LIBUSB_ERROR_TIMEOUT;
}

SeekCompactUsb::SeekCompactUsb() {
  libusb_context* context = nullptr;
  const int result = libusb_init(&context);
  if (result != LIBUSB_SUCCESS) {
    if (context != nullptr) {
      libusb_exit(context);
    }
    throw SeekCompactUsbError("Initializing libusb", result);
  }
  context_ = context;
}

SeekCompactUsb::~SeekCompactUsb() {
  disconnect();
  libusb_exit(context_);
}

void SeekCompactUsb::connect(std::uint8_t bus, std::uint8_t address) {
  if (handle_ != nullptr) {
    throw std::logic_error("Seek Compact camera is already connected");
  }

  libusb_device** devices = nullptr;
  const ssize_t deviceCount = libusb_get_device_list(context_, &devices);
  if (deviceCount < 0) {
    throw SeekCompactUsbError("Enumerating USB devices",
                              static_cast<int>(deviceCount));
  }

  bool foundSupportedDevice = false;
  int openError = LIBUSB_ERROR_NOT_FOUND;
  libusb_device_handle* openedHandle = nullptr;
  libusb_device_descriptor openedDescriptor{};
  for (ssize_t index = 0; index < deviceCount; ++index) {
    libusb_device_descriptor descriptor{};
    if (libusb_get_device_descriptor(devices[index], &descriptor) !=
        LIBUSB_SUCCESS) {
      continue;
    }
    if (descriptor.idVendor != kSeekVendorId ||
        descriptor.idProduct != kSeekCompactProductId ||
        libusb_get_bus_number(devices[index]) != bus ||
        libusb_get_device_address(devices[index]) != address) {
      continue;
    }

    foundSupportedDevice = true;
    openError = libusb_open(devices[index], &openedHandle);
    if (openError == LIBUSB_SUCCESS) {
      openedDescriptor = descriptor;
      break;
    }
  }
  libusb_free_device_list(devices, 1);

  if (openedHandle == nullptr) {
    if (!foundSupportedDevice) {
      throw std::runtime_error("The selected Seek Compact camera is no longer connected");
    }
    throw SeekCompactUsbError("Opening Seek Compact camera", openError);
  }

  handle_ = openedHandle;
  try {
    int activeConfiguration = 0;
    int result = libusb_get_configuration(handle_, &activeConfiguration);
    if (result != LIBUSB_SUCCESS) {
      throw SeekCompactUsbError("Reading USB configuration", result);
    }
    if (activeConfiguration != kConfiguration) {
      result = libusb_set_configuration(handle_, kConfiguration);
      if (result != LIBUSB_SUCCESS) {
        throw SeekCompactUsbError("Selecting USB configuration", result);
      }
    }

    result = libusb_set_auto_detach_kernel_driver(handle_, 1);
    if (result != LIBUSB_SUCCESS && result != LIBUSB_ERROR_NOT_SUPPORTED) {
      throw SeekCompactUsbError("Enabling automatic kernel-driver detach",
                                result);
    }

    result = libusb_claim_interface(handle_, kInterface);
    if (result != LIBUSB_SUCCESS) {
      throw SeekCompactUsbError("Claiming Seek Compact USB interface", result);
    }
    interfaceClaimed_ = true;

    std::array<unsigned char, 256> productName{};
    const int productNameSize = openedDescriptor.iProduct == 0
                                    ? LIBUSB_ERROR_NOT_FOUND
                                    : libusb_get_string_descriptor_ascii(
                                          handle_, openedDescriptor.iProduct,
                                          productName.data(),
                                          static_cast<int>(productName.size()));
    if (productNameSize > 0) {
      const auto productNameEnd =
          std::find(productName.begin(),
                    productName.begin() + productNameSize, '\0');
      if (productNameEnd != productName.begin()) {
        name_ = "Seek ";
        name_.append(
            reinterpret_cast<const char*>(productName.data()),
            static_cast<std::size_t>(
                std::distance(productName.begin(), productNameEnd)));
        name_ += " Thermal Camera";
      }
    }
    if (name_.empty()) {
      name_ = "Seek Compact Thermal Camera";
    }
  } catch (...) {
    if (interfaceClaimed_) {
      libusb_release_interface(handle_, kInterface);
      interfaceClaimed_ = false;
    }
    libusb_close(handle_);
    handle_ = nullptr;
    throw;
  }
}

void SeekCompactUsb::initialize(std::vector<unsigned char>& factoryData,
                                std::vector<unsigned char>& deviceInfo) {
  if (handle_ == nullptr || !interfaceClaimed_) {
    throw std::logic_error("Seek Compact camera is not connected");
  }

  deinitialize();

  std::array<unsigned char, 1> initializePayload{1};
  try {
    controlOut(static_cast<std::uint8_t>(Request::initialize),
               initializePayload.data(), initializePayload.size());
  } catch (const SeekCompactUsbError&) {
    deinitialize();
    controlOut(static_cast<std::uint8_t>(Request::initialize),
               initializePayload.data(), initializePayload.size());
  }

  std::array<unsigned char, 2> stopPayload{0, 0};
  controlOut(static_cast<std::uint8_t>(Request::operationMode),
             stopPayload.data(), stopPayload.size());

  std::array<unsigned char, 4> initialDeviceInfo{};
  controlIn(static_cast<std::uint8_t>(Request::deviceInfo),
            initialDeviceInfo.data(), initialDeviceInfo.size());

  std::array<unsigned char, 12> initialStatus{};
  controlIn(static_cast<std::uint8_t>(Request::status), initialStatus.data(),
            initialStatus.size());

  factoryData.resize(kFactoryDataSize);
  for (std::size_t offset = 0; offset < factoryData.size();
       offset += kFactoryChunkSize) {
    const std::size_t count =
        std::min(kFactoryChunkSize, factoryData.size() - offset);
    const auto countWords = static_cast<std::uint16_t>(count / 2);
    const auto offsetWords = static_cast<std::uint16_t>(offset / 2);

    std::array<unsigned char, 6> selection{};
    writeLittleEndian16(selection.data(), countWords);
    writeLittleEndian16(selection.data() + 2, offsetWords);
    controlOut(static_cast<std::uint8_t>(Request::selectFactoryData),
               selection.data(), selection.size());
    controlIn(static_cast<std::uint8_t>(Request::factoryData),
              factoryData.data() + offset, count);
    // Match the Android factory-read sequence's 10 ms inter-chunk pacing.
    std::this_thread::sleep_for(kFactoryChunkInterval);
  }

  std::array<unsigned char, 2> deviceInfoSelection{21, 0};
  controlOut(static_cast<std::uint8_t>(Request::selectDeviceInfo),
             deviceInfoSelection.data(), deviceInfoSelection.size());
  deviceInfo.resize(kDeviceInfoSize);
  controlIn(static_cast<std::uint8_t>(Request::deviceInfo), deviceInfo.data(),
            deviceInfo.size());

  std::array<unsigned char, 2> imageProcessingMode{8, 0};
  controlOut(static_cast<std::uint8_t>(Request::imageProcessingMode),
             imageProcessingMode.data(), imageProcessingMode.size());
  std::array<unsigned char, 2> operationModeStatus{};
  controlIn(static_cast<std::uint8_t>(Request::operationModeStatus),
            operationModeStatus.data(), operationModeStatus.size());

  controlOut(static_cast<std::uint8_t>(Request::imageProcessingMode),
             imageProcessingMode.data(), imageProcessingMode.size());
  std::array<unsigned char, 2> startPayload{1, 0};
  controlOut(static_cast<std::uint8_t>(Request::operationMode),
             startPayload.data(), startPayload.size());
  controlIn(static_cast<std::uint8_t>(Request::operationModeStatus),
            operationModeStatus.data(), operationModeStatus.size());
}

void SeekCompactUsb::readFrame(std::vector<unsigned char>& frame) {
  if (handle_ == nullptr || !interfaceClaimed_) {
    throw std::logic_error("Seek Compact camera is not connected");
  }
  if (frame.empty() || frame.size() % 2 != 0 ||
      frame.size() / 2 > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument(
        "Seek Compact frame buffer must contain a nonzero whole number of "
        "16-bit pixels");
  }

  const auto pixelCount = static_cast<std::uint32_t>(frame.size() / 2);
  std::array<unsigned char, 4> frameRequest{};
  writeLittleEndian32(frameRequest.data(), pixelCount);
  controlOut(static_cast<std::uint8_t>(Request::frame), frameRequest.data(),
             frameRequest.size());

  std::size_t offset = 0;
  while (offset < frame.size()) {
    const std::size_t remaining = frame.size() - offset;
    const int transferSize = static_cast<int>(std::min(
        remaining, static_cast<std::size_t>(std::numeric_limits<int>::max())));
    int transferred = 0;
    const int result = libusb_bulk_transfer(
        handle_, kFrameEndpoint, frame.data() + offset, transferSize,
        &transferred, kTransferTimeoutMilliseconds);
    if (result != LIBUSB_SUCCESS) {
      throw SeekCompactUsbError("Reading Seek Compact frame", result);
    }
    if (transferred <= 0) {
      throw std::runtime_error(
          "Seek Compact returned an empty successful frame transfer");
    }
    offset += static_cast<std::size_t>(transferred);
  }
}

void SeekCompactUsb::disconnect() noexcept {
  if (handle_ == nullptr) {
    return;
  }

  deinitialize();
  // Match the Android stop sequence's pause before tearing down the session.
  std::this_thread::sleep_for(kStopSettlingInterval);
  if (interfaceClaimed_) {
    libusb_release_interface(handle_, kInterface);
    interfaceClaimed_ = false;
  }
  libusb_close(handle_);
  handle_ = nullptr;
  name_.clear();
}

const std::string& SeekCompactUsb::name() const noexcept {
  return name_;
}

void SeekCompactUsb::deinitialize() noexcept {
  if (handle_ == nullptr) {
    return;
  }

  std::array<unsigned char, 2> stopPayload{0, 0};
  for (int attempt = 0; attempt < kDeinitializeCommandCount; ++attempt) {
    try {
      controlOut(static_cast<std::uint8_t>(Request::operationMode),
                 stopPayload.data(), stopPayload.size());
    } catch (...) {
      // A stale or disconnected camera may reject individual stop commands.
    }
  }
}

void SeekCompactUsb::controlOut(std::uint8_t request, unsigned char* data,
                                std::size_t size) {
  if (size > std::numeric_limits<std::uint16_t>::max()) {
    throw std::length_error("Seek Compact USB control payload is too large");
  }

  const int result = libusb_control_transfer(
      handle_, kVendorInterfaceOut, request, 0, kInterface, data,
      static_cast<std::uint16_t>(size), kTransferTimeoutMilliseconds);
  if (result < 0) {
    throw SeekCompactUsbError(requestDescription("OUT", request), result);
  }
  if (static_cast<std::size_t>(result) != size) {
    throw std::runtime_error(requestDescription("OUT", request) +
                             " transferred " + std::to_string(result) +
                             " of " + std::to_string(size) + " bytes");
  }
}

void SeekCompactUsb::controlIn(std::uint8_t request, unsigned char* data,
                               std::size_t size) {
  if (size > std::numeric_limits<std::uint16_t>::max()) {
    throw std::length_error("Seek Compact USB control payload is too large");
  }

  const int result = libusb_control_transfer(
      handle_, kVendorInterfaceIn, request, 0, kInterface, data,
      static_cast<std::uint16_t>(size), kTransferTimeoutMilliseconds);
  if (result < 0) {
    throw SeekCompactUsbError(requestDescription("IN", request), result);
  }
  if (static_cast<std::size_t>(result) != size) {
    throw std::runtime_error(requestDescription("IN", request) +
                             " transferred " + std::to_string(result) +
                             " of " + std::to_string(size) + " bytes");
  }
}
