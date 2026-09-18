# ThermalSeek

ThermalSeek is a Linux desktop viewer for Seek Compact and Mileseey TR256i
thermal cameras, built with C++17 and Qt 6. It displays live thermal images
with temperature readings, spot measurements, and region-of-interest statistics.

![ThermalSeek live radiometric measurements](docs/images/thermalseek-live.png)

## Features

- Live thermal video with five palettes and a matching temperature scale.
- Automatic, locked, and manual display temperature ranges.
- Minimum, maximum, and center readings, pinned points, and ROI statistics.
- Configurable emissivity and reflected-background temperature.
- Freeze-frame inspection and PNG screenshots.
- Per-camera fixed-pattern correction for Seek Compact.

## Supported cameras

| Camera | USB ID | Status |
|---|---|---|
| Seek Compact / PIR206 | `289d:0010` | Radiometric capture and fixed-pattern correction |
| Mileseey Tools TR256i | `0bda:5840` | 256×192 radiometric capture at 25 fps; fixed-pattern calibration unavailable |

The first available camera opens automatically. Use **Camera → Select Camera**
to choose another. Switching cameras clears frozen frames and measurements.

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
./build/thermalseek
```

TR256i normally takes about five seconds to show a usable image. Close the app
normally before disconnecting it so its original emissivity setting can be
restored.

## Linux USB permissions

Both cameras need USB device access; TR256i also needs access to its
`/dev/video*` capture device.
If access is denied, create a udev rule such as
`/etc/udev/rules.d/99-thermalseek.rules`:

```udev
SUBSYSTEM=="usb", ATTR{idVendor}=="289d", ATTR{idProduct}=="0010", MODE="0660", TAG+="uaccess"
SUBSYSTEM=="usb", ATTR{idVendor}=="0bda", ATTR{idProduct}=="5840", MODE="0660", TAG+="uaccess"
SUBSYSTEM=="video4linux", ATTRS{idVendor}=="0bda", ATTRS{idProduct}=="5840", MODE="0660", TAG+="uaccess"
```

Reload the rules and reconnect the camera:

```sh
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Do not run the desktop application as root. `uaccess` grants access to active
desktop sessions; headless/SSH users need appropriate device-group permissions.

## Controls

| Key | Action |
|---|---|
| `F5` | Rescan supported cameras |
| `Ctrl+R` | Reconnect the selected camera |
| `P` | Cycle through thermal palettes |
| `A` | Use the automatic per-frame temperature range |
| `L` | Lock the currently displayed temperature range |
| `M` | Configure a manual temperature range |
| `H` | Toggle the hot and cold image markers |
| `R` | Configure emissivity and reflected-background temperature |
| `Space` | Freeze or resume the displayed thermal frame |
| `Delete` | Clear pinned points and the selected ROI |
| `G` | Save a screenshot of the ThermalSeek window |

Move the mouse over the image for a spot reading. Click to pin a measurement;
click the same point again to remove it. Drag with the left mouse button to
select an ROI.

Use **Measurement → Radiometric Settings** to adjust emissivity (default `1.00`)
and reflected-background temperature. Display and measurement settings persist
between sessions. Absolute-temperature accuracy has not been independently
verified against a reference target.

### Seek fixed-pattern correction

Place the lens against a smooth, matte, room-temperature surface with no gap,
then choose **Measurement → Calibrate Fixed-Pattern Correction**. Keep the
camera covered and still until calibration finishes.

Profiles are saved automatically for each camera and cannot be shared between
cameras. Use the **Measurement** menu to enable, disable, or delete a profile.

### Screenshots

Press **G** to save a PNG in the `ThermalSeek` subdirectory of your system's
Pictures folder. The status bar shows the saved path or any error.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for development setup, tests, architecture,
and the requirements for adding camera backends.

## License

ThermalSeek source code is available under the [MIT License](LICENSE).

Qt and libusb are dynamically linked third-party dependencies with their own
licenses. Binary distributors remain responsible for the applicable Qt and
libusb license notices and relinking requirements.
