# ThermalSeek

ThermalSeek is a C++17 and Qt 6 desktop viewer for radiometric thermal
cameras. It discovers a supported USB camera, performs the camera-specific
calibration and temperature conversion, and displays a live colorized image
with minimum, maximum, and center temperatures in Celsius.

The project currently supports one camera family and has been tested on
Linux. Camera support is implemented in-tree; ThermalSeek does not build or
link libseekthermal.

![ThermalSeek live radiometric measurements](docs/images/thermalseek-live.png)

## Features

- Live capture on a dedicated worker thread.
- Camera-derived apparent temperatures with configurable emissivity and
  reflected-background compensation.
- Five selectable thermal palettes with a matching temperature scale.
- Automatic, locked, and manually configured display temperature ranges.
- Live minimum, maximum, and center readings with hot/cold image markers.
- Mouse spot readings, pinned measurement points, and ROI statistics.
- Freeze-frame inspection while camera capture continues in the background.
- Resizable Qt Widgets interface.
- Single-key PNG screenshots containing the thermal view, scale, status,
  overlays, and active radiometric parameters.
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

Run the focused radiometry, display, and inspection tests with:

```sh
ctest --test-dir build --output-on-failure
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
| `P` | Cycle through thermal palettes |
| `A` | Use the automatic per-frame temperature range |
| `L` | Lock the currently displayed temperature range |
| `M` | Configure a manual temperature range |
| `H` | Toggle the hot and cold image markers |
| `R` | Configure emissivity and reflected-background temperature |
| `Space` | Freeze or resume the displayed thermal frame |
| `Delete` | Clear pinned points and the selected ROI |
| `G` | Save a screenshot of the ThermalSeek window |

The **Display** menu exposes the same palette, range, marker, freeze, and clear
controls. The **Measurement** menu contains the radiometric correction
settings.

Move the mouse over the thermal image for an exact spot reading. Click to pin
a measurement point; click the same detector pixel again to remove it. Drag
with the left mouse button to select an ROI with minimum, maximum, average,
and center temperatures.

Radiometric correction defaults to emissivity `1.00`, which preserves the
camera's apparent temperatures exactly. For lower-emissivity targets, open
**Measurement → Radiometric Settings** and provide the target emissivity and
the reflected-background temperature. The corrected temperature field drives
the image, scale, MIN/MAX markers, spot readings, and ROI statistics. These
measurement settings, along with the palette, display range, markers, and
window geometry, persist between sessions.

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
seek_compact_usb       discovery, controls, calibration blobs, raw frames
       |
       v
thermal_processor      calibration frames, bad pixels, apparent temperatures
       |
       v
ThermalFrame           immutable row-major apparent Celsius pixels
       |
       v
radiometric_correction emissivity and reflected-background compensation
       |
       v
ThermalFrame           measurement Celsius plus min/max/center and extrema
       |
       v
thermal_renderer       palette/range mapping plus both radiometric frames
       |
       v
thermal_image_widget
       |               display/detector mapping, spot points, ROI, overlays
       v
main_window            measurement controls, scale, status, screenshots
```

Important source files:

- `src/seek_compact_usb.*` — direct libusb transport for `289d:0010`.
- `src/thermal_processor.*` — Seek Compact calibration and thermography.
- `src/radiometric_correction.*` — emissivity/reflected-background
  compensation and corrected statistics.
- `src/seek_camera_thread.*` — capture lifecycle, pooled frame ownership, and
  UI delivery.
- `src/thermal_palette.*` — selectable 256-color palette registry.
- `src/thermal_renderer.*` — display-range mapping and frame orientation.
- `src/thermal_inspection.*` — display/detector mapping and radiometric statistics.
- `src/thermal_image_widget.*` — mouse interaction and measurement overlays.
- `src/main_window.*` — live viewer, scale, controls, status, and screenshots.

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
