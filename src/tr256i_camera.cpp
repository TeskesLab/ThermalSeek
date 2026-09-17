#include "tr256i_camera.hpp"

#include "tr256i_decoder.hpp"

#include <stdexcept>

#ifdef __linux__
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {
constexpr std::size_t frameBytes = 256 * 384 * 2;
using Clock = std::chrono::steady_clock;

[[noreturn]] void systemError(const std::string &operation) {
  const int error = errno;
  std::string message = "TR256i " + operation + ": " + std::strerror(error);
  if (error == EACCES || error == EPERM) {
    message += ". Grant your user read/write access to the video node and "
               "USB device (0bda:5840) through udev/group permissions, then reconnect.";
  } else if (error == EBUSY) {
    message += ". Close other applications using this camera, then retry.";
  }
  throw std::runtime_error(message);
}

class FileDescriptor final {
public:
  FileDescriptor(const std::string &path, int flags)
      : value_(::open(path.c_str(), flags | O_CLOEXEC)) {
    if (value_ < 0) {
      systemError("opening " + path);
    }
  }
  ~FileDescriptor() { ::close(value_); }
  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;
  int get() const noexcept { return value_; }

private:
  int value_;
};

void checkedIoctl(int fd, unsigned long request, void *argument,
                  const char *operation) {
  if (::ioctl(fd, request, argument) < 0) {
    systemError(operation);
  }
}

bool isCaptureDevice(int fd) {
  v4l2_capability capability{};
  checkedIoctl(fd, VIDIOC_QUERYCAP, &capability, "querying V4L2 capabilities");
  const auto flags = (capability.capabilities & V4L2_CAP_DEVICE_CAPS)
                         ? capability.device_caps
                         : capability.capabilities;
  return (flags & V4L2_CAP_VIDEO_CAPTURE) && (flags & V4L2_CAP_STREAMING);
}

std::string readAttribute(const std::filesystem::path &path) {
  std::ifstream stream(path);
  std::string value;
  stream >> value;
  return value;
}

std::optional<CameraDevice> identifyVideoNode(const std::filesystem::path &node) {
  std::error_code error;
  auto parent = std::filesystem::canonical(node / "device", error);
  if (error) {
    return std::nullopt;
  }
  while (!parent.empty() && parent != parent.root_path()) {
    if (readAttribute(parent / "idVendor") == "0bda" &&
        readAttribute(parent / "idProduct") == "5840") {
      const auto busText = readAttribute(parent / "busnum");
      const auto addressText = readAttribute(parent / "devnum");
      unsigned long bus = 0;
      unsigned long address = 0;
      try {
        std::size_t busEnd = 0;
        std::size_t addressEnd = 0;
        bus = std::stoul(busText, &busEnd);
        address = std::stoul(addressText, &addressEnd);
        if (busEnd != busText.size() || addressEnd != addressText.size()) {
          throw std::invalid_argument("USB address");
        }
      } catch (const std::exception &) {
        throw std::runtime_error("TR256i USB address is unavailable in " +
                                 parent.string() + ". Reconnect and refresh cameras.");
      }
      if (bus == 0 || bus > 255 || address == 0 || address > 127) {
        throw std::runtime_error("TR256i USB address is out of range; reconnect "
                                 "and refresh cameras.");
      }
      CameraDevice device;
      device.kind = CameraKind::MileseeyTr256i;
      device.devicePath = (std::filesystem::path("/dev") / node.filename()).string();
      device.id = "tr256i:" + parent.string() + ":" + node.filename().string();
      device.name = "Mileseey TR256i (" + device.devicePath + ")";
      device.usbBus = static_cast<std::uint8_t>(bus);
      device.usbAddress = static_cast<std::uint8_t>(address);
      return device;
    }
    parent = parent.parent_path();
  }
  return std::nullopt;
}

std::string usbPath(const CameraDevice &device) {
  std::array<char, 32> path{};
  std::snprintf(path.data(), path.size(), "/dev/bus/usb/%03u/%03u",
                static_cast<unsigned>(device.usbBus),
                static_cast<unsigned>(device.usbAddress));
  return path.data();
}

class CameraEmissivity final {
public:
  explicit CameraEmissivity(const CameraDevice &device)
      : fd_(usbPath(device), O_RDWR) {}

  ~CameraEmissivity() {
    if (!restore_) {
      return;
    }
    try {
      set(original_);
      if (get() != original_) {
        throw std::runtime_error("restored emissivity did not match readback");
      }
    } catch (const std::exception &error) {
      // Destructors cannot propagate errors through CameraSession's close path.
      std::fprintf(stderr,
                   "TR256i could not restore camera emissivity Q7=%u: %s. "
                   "Reconnect/power-cycle the camera before another application "
                   "uses it; no settings were saved to flash.\n",
                   static_cast<unsigned>(original_), error.what());
    }
  }

  void normalize() {
    original_ = get();
    if (original_ == 0 || original_ > 128) {
      throw std::runtime_error("TR256i emissivity query returned invalid Q7 value " +
                               std::to_string(original_) +
                               "; reconnect the camera. Capture stopped to avoid "
                               "incorrect radiometry.");
    }
    if (original_ != 128) {
      // Mark before issuing any write: a failed transfer can still reach hardware.
      restore_ = true;
      set(128);
      if (get() != 128) {
        throw std::runtime_error("TR256i could not verify temporary emissivity 1.0; "
                                 "capture stopped to avoid double correction.");
      }
    }
  }

private:
  void transfer(bool input, unsigned short index, unsigned char *data,
                unsigned short length) {
    usbdevfs_ctrltransfer control{};
    control.bRequestType = input ? 0xc1 : 0x41;
    control.bRequest = input ? 0x44 : 0x45;
    control.wValue = 0x78;
    control.wIndex = index;
    control.wLength = length;
    control.timeout = 250;
    control.data = data;
    const int transferred = ::ioctl(fd_.get(), USBDEVFS_CONTROL, &control);
    if (transferred < 0) {
      systemError("accessing camera emissivity over USB");
    }
    if (transferred != length) {
      throw std::runtime_error("TR256i emissivity USB transfer was short (" +
                               std::to_string(transferred) + " of " +
                               std::to_string(length) +
                               " bytes); reconnect the camera. Capture cannot "
                               "use an assumed emissivity.");
    }
  }

  void command(std::array<unsigned char, 8> bytes) {
    transfer(false, 0x9d00, bytes.data(), bytes.size());
    std::array<unsigned char, 8> trailer{0, 0, 0, 0, 0, 0, 0, 2};
    transfer(false, 0x1d08, trailer.data(), trailer.size());
    // Status 01 is busy: early reads return the previous command's data.
    // Writes take about 1.05 s; queries after stopping during startup took 5.42 s.
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    for (;;) {
      unsigned char status = 0;
      transfer(true, 0x0200, &status, 1);
      if (status == 0) {
        return;
      }
      if (status != 1) {
        throw std::runtime_error("TR256i returned unexpected USB command status " +
                                 std::to_string(status));
      }
      if (Clock::now() >= deadline) {
        throw std::runtime_error("TR256i USB command remained busy for 10 seconds; "
                                 "reconnect the camera. No stale reply was used.");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  std::uint16_t get() {
    command({0x14, 0x85, 0, 3, 0, 0, 0, 0});
    std::array<unsigned char, 2> value{};
    transfer(true, 0x1d10, value.data(), value.size());
    return (static_cast<std::uint16_t>(value[0]) << 8) | value[1];
  }

  void set(std::uint16_t value) {
    command({0x14, 0xc5, 0, 3, 0, 0, static_cast<unsigned char>(value >> 8),
             static_cast<unsigned char>(value & 0xff)});
  }

  FileDescriptor fd_;
  std::uint16_t original_ = 128;
  bool restore_ = false;
};

class MappedBuffer final {
public:
  MappedBuffer(int fd, const v4l2_buffer &buffer)
      : size_(buffer.length),
        data_(::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                     buffer.m.offset)) {
    if (data_ == MAP_FAILED) {
      systemError("mapping V4L2 capture buffer");
    }
  }
  ~MappedBuffer() {
    if (data_ != MAP_FAILED) {
      ::munmap(data_, size_);
    }
  }
  MappedBuffer(const MappedBuffer &) = delete;
  MappedBuffer &operator=(const MappedBuffer &) = delete;
  MappedBuffer(MappedBuffer &&other) noexcept
      : size_(other.size_), data_(std::exchange(other.data_, MAP_FAILED)) {}
  const unsigned char *data() const noexcept {
    return static_cast<const unsigned char *>(data_);
  }
  std::size_t size() const noexcept { return size_; }

private:
  std::size_t size_;
  void *data_;
};

class VideoStream final {
public:
  explicit VideoStream(const std::string &path)
      : fd_(path, O_RDWR | O_NONBLOCK) {
    if (!isCaptureDevice(fd_.get())) {
      throw std::runtime_error("TR256i selected node does not support V4L2 "
                               "video capture and streaming; refresh cameras.");
    }
    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = 256;
    format.fmt.pix.height = 384;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    format.fmt.pix.field = V4L2_FIELD_NONE;
    checkedIoctl(fd_.get(), VIDIOC_S_FMT, &format, "setting YUYV 256x384 format");
    if (format.fmt.pix.width != 256 || format.fmt.pix.height != 384 ||
        format.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV ||
        format.fmt.pix.bytesperline != 512 ||
        format.fmt.pix.sizeimage != frameBytes ||
        format.fmt.pix.field != V4L2_FIELD_NONE) {
      throw std::runtime_error("TR256i driver did not negotiate packed YUYV "
                               "256x384 (512-byte stride, 196608-byte frame). "
                               "Close other camera applications and reconnect.");
    }
    v4l2_streamparm parameters{};
    parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parameters.parm.capture.timeperframe.numerator = 1;
    parameters.parm.capture.timeperframe.denominator = 25;
    checkedIoctl(fd_.get(), VIDIOC_S_PARM, &parameters, "setting 25 fps capture");
    const auto &interval = parameters.parm.capture.timeperframe;
    if (interval.numerator == 0 || interval.denominator == 0 ||
        static_cast<std::uint64_t>(interval.numerator) * 25 !=
            interval.denominator) {
      throw std::runtime_error("TR256i driver did not negotiate 25 fps; "
                               "close other camera applications and reconnect.");
    }

    v4l2_requestbuffers request{};
    request.count = 4;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    checkedIoctl(fd_.get(), VIDIOC_REQBUFS, &request, "allocating V4L2 buffers");
    if (request.count == 0) {
      throw std::runtime_error("TR256i driver supplied no MMAP capture buffers.");
    }
    buffers_.reserve(request.count);
    for (unsigned int index = 0; index < request.count; ++index) {
      v4l2_buffer buffer{};
      buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buffer.memory = V4L2_MEMORY_MMAP;
      buffer.index = index;
      checkedIoctl(fd_.get(), VIDIOC_QUERYBUF, &buffer, "querying V4L2 buffer");
      if (buffer.length < frameBytes) {
        throw std::runtime_error("TR256i capture buffer cannot hold a complete "
                                 "radiometric frame.");
      }
      buffers_.emplace_back(fd_.get(), buffer);
      checkedIoctl(fd_.get(), VIDIOC_QBUF, &buffer, "queuing V4L2 buffer");
    }
  }

  ~VideoStream() {
    if (streaming_) {
      auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      ::ioctl(fd_.get(), VIDIOC_STREAMOFF, &type);
    }
  }

  void start() {
    auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    checkedIoctl(fd_.get(), VIDIOC_STREAMON, &type, "starting V4L2 capture");
    streaming_ = true;
  }

  CameraFrameStatus read(ThermalFrame &frame) {
    const auto deadline = Clock::now() + std::chrono::seconds(1);
    pollfd descriptor{fd_.get(), POLLIN, 0};
    int ready = 0;
    do {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 deadline - Clock::now()).count();
      if (remaining <= 0) {
        return CameraFrameStatus::Timeout;
      }
      ready = ::poll(&descriptor, 1, static_cast<int>(remaining));
    } while (ready < 0 && errno == EINTR);
    if (ready < 0) {
      systemError("waiting for V4L2 frame");
    }
    if (ready == 0) {
      return CameraFrameStatus::Timeout;
    }
    if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
      throw std::runtime_error("TR256i video stream disconnected or failed; "
                               "reconnect and refresh cameras.");
    }
    if (!(descriptor.revents & POLLIN)) {
      return CameraFrameStatus::Timeout;
    }
    v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    if (::ioctl(fd_.get(), VIDIOC_DQBUF, &buffer) < 0) {
      if (errno == EAGAIN) {
        return CameraFrameStatus::Timeout;
      }
      systemError("dequeueing V4L2 frame");
    }
    if (buffer.index >= buffers_.size() ||
        buffer.bytesused != frameBytes ||
        buffer.bytesused > buffers_[buffer.index].size() ||
        (buffer.flags & V4L2_BUF_FLAG_ERROR)) {
      throw std::runtime_error("TR256i returned an incomplete or damaged "
                               "196608-byte radiometric frame; reconnect the camera.");
    }
    const bool decoded = decodeTr256iFrame(buffers_[buffer.index].data(),
                                           buffer.bytesused, frame);
    checkedIoctl(fd_.get(), VIDIOC_QBUF, &buffer, "requeuing V4L2 frame");
    return decoded ? CameraFrameStatus::Ready : CameraFrameStatus::Calibration;
  }

private:
  FileDescriptor fd_;
  std::vector<MappedBuffer> buffers_;
  bool streaming_ = false;
};

class Tr256iSession final : public CameraSession {
public:
  explicit Tr256iSession(const CameraDevice &device)
      : info_{device.name, 256, 192, ThermalOrientation::Native, std::nullopt},
        emissivity_(device), video_(device.devicePath) {
    emissivity_.normalize();
    video_.start();
    startupDeadline_ = Clock::now() + std::chrono::seconds(10);
  }

  const CameraSessionInfo &info() const noexcept override { return info_; }

  CameraFrameStatus readFrame(ThermalFrame &frame) override {
    const auto status = video_.read(frame);
    if (status == CameraFrameStatus::Ready) {
      startupComplete_ = true;
    } else if (!startupComplete_ && Clock::now() >= startupDeadline_) {
      throw std::runtime_error("TR256i did not produce radiometric data within "
                               "10 seconds (startup placeholder or no frames). "
                               "Reconnect the camera and close other capture applications.");
    }
    return status;
  }

private:
  CameraSessionInfo info_;
  CameraEmissivity emissivity_;
  VideoStream video_;
  Clock::time_point startupDeadline_;
  bool startupComplete_ = false;
};
}  // namespace
#endif

std::vector<CameraDevice> discoverTr256iCameras() {
  std::vector<CameraDevice> devices;
#ifdef __linux__
  const std::filesystem::path root("/sys/class/video4linux");
  std::error_code error;
  if (!std::filesystem::exists(root, error)) {
    if (error) {
      throw std::runtime_error("Cannot inspect V4L2 cameras: " + error.message());
    }
    return devices;
  }
  for (const auto &entry : std::filesystem::directory_iterator(root)) {
    auto device = identifyVideoNode(entry.path());
    if (!device) {
      continue;
    }
    FileDescriptor fd(device->devicePath, O_RDWR | O_NONBLOCK);
    if (isCaptureDevice(fd.get())) {
      devices.push_back(std::move(*device));
    }
  }
  std::sort(devices.begin(), devices.end(),
            [](const CameraDevice &left, const CameraDevice &right) {
              return left.devicePath < right.devicePath;
            });
#endif
  return devices;
}

std::unique_ptr<CameraSession> openTr256iCamera(const CameraDevice &device) {
#ifdef __linux__
  const auto node = std::filesystem::path("/sys/class/video4linux") /
                    std::filesystem::path(device.devicePath).filename();
  const auto current = identifyVideoNode(node);
  if (device.kind != CameraKind::MileseeyTr256i || !current ||
      current->id != device.id || current->devicePath != device.devicePath ||
      current->usbBus != device.usbBus || current->usbAddress != device.usbAddress) {
    throw std::runtime_error("Selected TR256i is no longer at its discovered "
                             "video/USB address; refresh cameras and select it again.");
  }
  return std::make_unique<Tr256iSession>(device);
#else
  (void)device;
  throw std::runtime_error("TR256i radiometric capture requires Linux V4L2 and usbdevfs.");
#endif
}
