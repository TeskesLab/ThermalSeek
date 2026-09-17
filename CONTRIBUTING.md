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

The implementation separates these responsibilities:

1. `camera_session.*` owns discovery, selection, backend creation, and
   per-session dimensions, orientation, and learned-FPN capability.
2. `seek_compact_usb.*` owns selected-device Seek Compact USB controls and raw
   frame transport.
3. `thermal_processor.*` owns Seek Compact calibration and apparent-temperature
   conversion.
4. `tr256i_camera.*` owns TR256i V4L2 capture and temporary USB emissivity controls.
5. `tr256i_decoder.*` owns its temperature-plane decoding and validation.
6. `thermal_frame.hpp` defines the common immutable Celsius-field contract.
7. `fixed_pattern_correction.*` owns covered-lens profile training,
   validation, and detector-bias subtraction.
8. `fixed_pattern_profile_store.*` owns camera fingerprinting and atomic,
   checksummed profile persistence.
9. `radiometric_correction.*` owns emissivity/reflected-background
   compensation and recalculation of measurement statistics.
10. `thermal_renderer.*` owns palette selection, display ranges, orientation,
    and conversion of the corrected field to a display image.
11. `thermal_inspection.*` owns display/detector mapping and point/ROI
    statistics against the corrected measurement field.
12. `camera_thread.*` owns the worker lifecycle, pooled immutable frame
    ownership, profile lifecycle, rendering, and dispatch to Qt.
13. `thermal_image_widget.*` owns mouse interaction and measurement overlays.
14. `main_window.*` owns camera selection, measurement and presentation controls,
    persistent settings, status, and screenshots.

Despite its current name, `ThermalProcessor` is Seek Compact-specific. Do not
put another camera's frame layout, calibration branches, or USB commands into
that class.

New backends implement `CameraSession` and produce native-coordinate
`ThermalFrame` objects. `CameraFrameStatus` distinguishes usable frames,
calibration/control frames, verified shutter refreshes, and recoverable timeouts.
Keep reads bounded so the worker can stop. Carry the session's orientation into
both rendering and point/ROI mapping; Seek uses counterclockwise rotation,
whereas TR256i uses native landscape orientation.

An absent `fixedPatternFingerprint` means learned FPN is unsupported, not that
it should use a placeholder identity. Do not synthesize shutter events from
commands or elapsed time. Model-specific readiness/control completion belongs
in the backend; avoid compatibility shims and VID/PID branches in the UI.

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
- Displayable pixels are finite apparent temperatures in Celsius.
- `minimumCelsius` and `maximumCelsius` are the actual frame extrema.
- `centerCelsius` is the deterministic center detector sample.
- Extrema coordinates identify the pixels represented by the extrema values.

Radiometric correction is a separate, camera-independent measurement stage:

- Keep the apparent-temperature frame immutable so settings can be changed or
  replayed without accumulating correction error.
- Accept only finite settings with `0 < emissivity <= 1` and a reflected
  temperature at or above absolute zero.
- Emissivity `1` is an exact identity operation and must reuse the apparent
  frame rather than allocating or copying in the capture hot path.
- The corrected field must drive rendering, automatic range selection,
  extrema, spot readings, and ROI statistics consistently.
- If the camera already compensates for object emissivity, establish and verify
  an apparent-temperature mode before applying the host correction. Restore
  temporary camera settings on close; do not overwrite optical compensation
  or persist changes to flash merely to normalize the host pipeline.

For apparent and reflected temperatures expressed in Kelvin, the implemented
measurement model is:

```text
T_object = ((T_apparent^4 - (1 - emissivity) * T_reflected^4)
            / emissivity)^(1/4)
```

Clamp the radiance term to zero before taking the fourth root.

Raw detector values must not be labeled as temperatures. If a camera needs
factory calibration, shutter frames, gain maps, bad-pixel repair, or
temperature lookup tables, implement and validate those model-specific stages
before advertising radiometric support. Calibration/control frames must update
processor state and must not be displayed as normal images.

### Detector fixed-pattern correction

Fixed-pattern correction is a camera-independent stage after the
camera-specific processor and before emissivity/reflected-background
compensation. Preserve all three immutable frame identities: the original
camera-apparent frame, the fixed-pattern-corrected apparent frame, and the
final measurement frame.

Profile calibration must:

- Continue capture while the modeless UI reports progress or cancellation.
- Collect the original camera-apparent field, never a field corrected by an
  existing profile or by radiometric settings.
- Require at least 96 usable frames and four native shutter refreshes. If the
  frame target is reached first, keep reading frames and wait for the next
  required shutter instead of rejecting valid data.
- Subtract each frame's spatial median, calculate the temporal per-pixel
  median, remove its 5×5 spatial median, and zero-center the resulting bias.
  The spatial high-pass prevents a real low-frequency target gradient from
  being stored as detector bias.
- Reject global-median drift above 2.0 °C, robust low-frequency span above
  3.0 °C, temporal residual RMS above 0.5 °C, bias RMS above 1.5 °C, or an
  absolute bias above 5.0 °C.
- Recalculate corrected extrema, coordinates, and center temperature.
- Keep the previous valid profile active until a replacement passes
  validation and is committed successfully.

Profiles are camera-specific. Key storage by SHA-256 over the fixed-length
factory-calibration and device-information blobs, include the fingerprint and
detector dimensions in the payload, verify a payload checksum when loading,
and commit with `QSaveFile` without direct-write fallback. A missing or invalid
profile disables the stage; it must not stop camera capture. Never commit
factory blobs or learned per-camera profile data to the repository.

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

Fixed-pattern changes additionally require:

- A successful covered-lens calibration through the production UI.
- Evidence that held-out covered-target fixed-pattern residual decreases
  without a material median-temperature shift.
- A normal-scene check that edges remain sharp and the detector-fixed
  component decreases.
- Successful enable/disable control while live capture continues.
- Successful profile reload for the same camera and rejection for a different
  fingerprint or corrupted payload.

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
