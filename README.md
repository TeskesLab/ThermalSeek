# ThermalSeek

ThermalSeek is a C++17 and Qt 6 desktop viewer for radiometric thermal
cameras. It discovers a supported USB camera, performs the camera-specific
calibration and temperature conversion, and displays a live colorized image
with minimum, maximum, and center temperatures in Celsius.

The project currently supports one camera family and has been tested on
Linux. Camera support is implemented in-tree; ThermalSeek does not build or
link libseekthermal.

![ThermalSeek live view showing a colorized Seek Compact image, Celsius scale, and temperature readings](docs/images/thermalseek-live.png)

## Features

- Live capture on a dedicated worker thread.
- Camera-derived radiometric calibration and Celsius output.
- Color thermal lookup table with an external temperature scale.
- Live minimum, maximum, and center temperature readings.
- Resizable Qt Widgets interface.
- Single-key PNG screenshots containing the thermal view, scale, and status.
- Direct libusb transport with deterministic cleanup and timeout handling.

## Supported cameras

| Camera | USB ID | Status |
|---|---|---|
| Seek Compact / PIR206 | `289d:0010` | Live capture and radiometric temperatures |

ThermalSeek currently opens the first supported camera it finds. See
[CONTRIBUTING.md](CONTRIBUTING.md) before adding another model: USB transport,
calibration, frame layout, and temperature conversion must remain
model-specific until their equivalence is demonstrated.

## Requirements

- CMake 3.21 or newer
- C++17 compiler
- Qt 6.4 or newer with the Widgets component
- pkg-config
- libusb 1.0 development files

On Ubuntu, the required development packages can be installed with:

```sh
sudo apt install build-essential cmake pkg-config qt6-base-dev libusb-1.0-0-dev
```

## Build

```sh
git clone ssh://git@git.teske.zip:30009/TeskesLabOrg/ThermalSeek.git
cd ThermalSeek
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Run the application with:

```sh
./build/thermalseek
```

The camera can take a short time to provide all required calibration frames.
The status bar displays `Calibrating` until a radiometric display frame is
available.

## Linux USB permissions

ThermalSeek needs permission to claim the camera's USB interface. If the
camera is detected but cannot be opened, create a udev rule such as
`/etc/udev/rules.d/99-thermalseek.rules`:

```udev
SUBSYSTEM=="usb", ATTR{idVendor}=="289d", ATTR{idProduct}=="0010", MODE="0660", TAG+="uaccess"
```

Reload the rules and reconnect the camera:

```sh
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Do not run the desktop application as root.

## Controls

| Key | Action |
|---|---|
| `G` | Save a screenshot of the ThermalSeek window |

Screenshots are written as PNG files to the platform Pictures directory under
a `ThermalSeek` subdirectory. The filename format is:

```text
ThermalSeek_yyyyMMdd_HHmmss_zzz.png
```

The status bar reports the saved path or the reason a screenshot failed.

## Architecture

```text
Seek Compact USB
       |
       v
seek_compact_usb   device discovery, controls, calibration blobs, raw frames
       |
       v
thermal_processor  calibration frames, bad pixels, radiometric conversion
       |
       v
ThermalFrame       row-major Celsius pixels plus min/max/center
       |
       v
thermal_palette -> main_window
```

Important source files:

- `src/seek_compact_usb.*` — direct libusb transport for `289d:0010`.
- `src/thermal_processor.*` — Seek Compact calibration and thermography.
- `src/seek_camera_thread.*` — capture lifecycle and UI dispatch.
- `src/thermal_palette.*` — shared 256-color lookup table.
- `src/main_window.*` — live viewer, temperature scale, and screenshots.
- `REVTMP.md` — reverse-engineering evidence and recovered thermography details.

## Contributing

Camera backends need hardware evidence, documented protocol provenance, safe
USB lifecycle handling, and a verified radiometric conversion. Read
[CONTRIBUTING.md](CONTRIBUTING.md) for the required design and validation
workflow.

## License

ThermalSeek source code is available under the [MIT License](LICENSE).

Qt and libusb are dynamically linked third-party dependencies with their own
licenses. Binary distributors remain responsible for the applicable Qt and
libusb license notices and relinking requirements.
