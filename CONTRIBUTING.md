# Contributing to ThermalSeek

ThermalSeek welcomes fixes and support for additional radiometric thermal
cameras. Camera support is load-bearing hardware work: a picture that looks
plausible is not sufficient evidence that temperatures, calibration, USB
lifecycle, or failure handling are correct.

## Development setup

Install the dependencies listed in [README.md](README.md), then configure and
build the project:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

GCC and Clang builds enable `-Wall`, `-Wextra`, and `-Wpedantic`. Contributions
must build without new warnings.

## Project boundaries

The current implementation has six distinct responsibilities:

1. `seek_compact_usb.*` owns Seek Compact discovery, USB controls, and raw
   frame transport.
2. `thermal_processor.*` owns Seek Compact calibration and radiometric
   conversion.
3. `ThermalFrame` is the common radiometric output.
4. `thermal_renderer.*` owns palette selection, display ranges, orientation,
   and conversion to a display image.
5. `seek_camera_thread.*` owns the worker-thread lifecycle and dispatches
   rendered frames to Qt.
6. `main_window.*` owns presentation controls, extrema overlays, status, and
   screenshots.

Despite its current name, `ThermalProcessor` is Seek Compact-specific. Do not
put another camera's frame layout, calibration branches, or USB commands into
that class.

The first contribution adding another backend should also introduce the
smallest common camera-session boundary needed to select exactly one supported
camera and produce `ThermalFrame` objects. Migrate the existing Seek Compact
path to that boundary in the same change. Avoid compatibility shims and avoid
scattering VID/PID checks through the UI or processing code.

## Before implementing a camera

Open a proposal describing:

- Manufacturer and exact model.
- USB VID/PID or the non-USB discovery identity.
- Tested firmware or hardware revisions.
- Transport endpoints and frame dimensions.
- Whether frames contain temperatures or raw detector samples.
- Where calibration data comes from.
- Available protocol documentation, captures, or reverse-engineering evidence.
- License and provenance of every source used to understand the protocol.

Do not copy code from GPL, AGPL, proprietary SDK, decompiled application, or
another source that cannot be contributed under MIT. Protocol facts can be
reimplemented, but the submitted implementation must be original and its
evidence trail must be documented.

## Backend design requirements

Keep each camera backend model-specific until two implementations have proven
that a shared operation has the same command, payload, transfer length,
framing, lifecycle state, and validation rules. Similar command names are not
equivalence evidence.

A backend must:

- Own its transport resources through RAII.
- Keep the transport context alive until all device handles are released.
- Claim and release only the required interfaces.
- Put the camera into a safe stopped state during normal shutdown and errors.
- Verify exact control-transfer and frame lengths.
- Accumulate valid short transfers when the protocol permits them.
- Distinguish recoverable timeouts from disconnects and protocol failures.
- Keep blocking I/O on the capture thread, never the Qt UI thread.
- Reuse frame and calibration buffers instead of allocating in the hot path.
- Report actionable errors without silently substituting data.

Do not add sleeps, USB resets, retries, or endpoint clearing as unexplained
workarounds. If timing is part of the device protocol, document the evidence
and bound it explicitly.

## Radiometric processing requirements

A new processor must produce `ThermalFrame` using these invariants:

- `width` and `height` are nonzero detector dimensions.
- `celsius.size() == width * height`.
- Pixels are row-major in the camera's native detector orientation.
- Display rotation or mirroring remains a rendering concern.
- Displayable pixels are finite Celsius values.
- `minimumCelsius` and `maximumCelsius` are the actual frame extrema.
- `centerCelsius` is the deterministic center detector sample.

Raw detector values must not be labeled as temperatures. If a camera needs
factory calibration, shutter frames, gain maps, bad-pixel repair, emissivity
correction, or temperature lookup tables, implement and validate those stages
before advertising radiometric support. Calibration/control frames must update
processor state and must not be displayed as normal images.

If only non-radiometric imaging is understood, describe the limitation in the
proposal instead of inventing or approximating temperatures.

## Adding a camera

A complete camera contribution normally includes:

1. A model-specific transport class under `src/`.
2. A model-specific processor when the camera does not directly return
   calibrated Celsius pixels.
3. Discovery and selection through the common capture boundary.
4. Conversion to the `ThermalFrame` contract.
5. CMake source and dependency updates.
6. The new model and USB ID in the README support table.
7. Protocol and calibration evidence in a tracked technical document.
8. Linux permission guidance when the model uses a new USB ID.

New dependencies require a concrete need and a license compatible with an MIT
application. Prefer stable system libraries over vendored source. Do not
commit proprietary SDK binaries, firmware, generated build output, or captured
factory data from a user's camera.

## Hardware verification

Test the exact camera revision named in the contribution. At minimum, record
these checks in the change description:

- Detection from a cold USB connection.
- Successful interface claim and initialization.
- Completion of every required calibration stage.
- At least 60 seconds of continuous display-frame capture.
- Correct frame dimensions and orientation.
- Finite pixel values and internally consistent min/max/center statistics.
- Clean application close followed by a successful reopen.
- Graceful handling of startup without the camera.
- Graceful handling of physical disconnect during capture.
- A screenshot created with `G` containing the image, temperature scale, and
  live statistics.

Radiometric claims also need accuracy evidence. Describe the reference target
or instrument, tested temperature range, emissivity assumptions, ambient
conditions, and observed error. A visually convincing palette is not an
accuracy test.

## Code quality

- Use C++17 and the existing two-space indentation.
- Prefer explicit state and deterministic parsing over implicit fallbacks.
- Keep model constants named and scoped to their backend.
- Validate byte offsets and sizes before reading buffers.
- Avoid per-pixel allocations, repeated copies, and avoidable floating-point
  work in capture loops.
- Remove obsolete callers and paths when changing an interface.
- Keep UI, transport, and radiometric processing separate.

Run the release build before submitting:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

## Change description checklist

Include the following with a camera-support change:

- [ ] Exact camera model, revision, firmware, and discovery IDs.
- [ ] Protocol and implementation provenance.
- [ ] Calibration and temperature-conversion description.
- [ ] Frame dimensions, encoding, units, and orientation.
- [ ] USB/resource lifecycle and timeout behavior.
- [ ] Hardware verification results.
- [ ] Radiometric accuracy evidence, or an explicit non-radiometric label.
- [ ] README and technical documentation updates.
- [ ] Confirmation that submitted code and data may be distributed under MIT.

## License

By contributing, you confirm that you have the right to submit the work under
the project's [MIT License](LICENSE). Do not include third-party code or data
whose terms prevent that distribution.
