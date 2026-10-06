# Capture throughput checks

These are host correctness checks and kernel timings, not phone FPS measurements.
No camera resolution, point density, confidence thresholds or JPEG quality are
reduced by this capture-speed pass.

The [Retango suite](retango_README.md) separately compares dense/sparse preparation
against a captured working-tree baseline, including exact point/mask/estimate
state and the disabled-estimator wall-preparation guard.

## Image conversion

```sh
python3 tests/capture_speed/image_run.py
```

Requires a host C++11 compiler with ASan/UBSan. This compiles the real image code
using the existing performance-suite GL/codec initialization boundaries. It checks
known bytes, 187 resampling cases, source-buffer preservation and rejection of
unsupported 4:2:0 dimensions. Supported capture layouts retain the existing
channel order, orientation, sampling and integer rounding. Output allocation is
the packed 4:2:0 size (1.5 bytes/pixel rather than the old 2 bytes/pixel).

For a same-session before/after comparison, snapshot before editing:

```sh
python3 tests/capture_speed/image_run.py --snapshot /path/to/before-image.cc
# Make the change, then:
python3 tests/capture_speed/image_run.py --baseline /path/to/before-image.cc
```

Both benchmark binaries use `-O2`; full output hashes must match. On this host the
measured medians were 499.618 → 439.884 microseconds for 360×640/1, and
717.347 → 466.148 microseconds for 1080×1920/3. These kernel results do not imply
the same improvement in end-to-end capture rate or on ARM hardware.

## Installed build

For the owned reconstruction backend's standard RGBA-to-limited-range-NV21
contract, run `python3 tests/capture_speed/image_run.py --modern`. This checks
modern primary-color bytes and the same resampling/bounds cases. Its channel
mapping intentionally corrects the legacy producer's R/B and chroma convention;
it is not a byte-identical legacy benchmark. Recorded JPEG-to-NV21 conversion
for offline texturing remains a separate full-range producer.

`Application.mk` enables `-O2` for native C and C++ even in debug APKs, overriding
ndk-build's earlier `-O0`. Debug symbols/assertions remain available, and fast-math
is not enabled. Verify the final compiler command when changing build settings;
a debug APK with unoptimized reconstruction/codecs is not representative of
optimized capture performance.

## Device-stage measurements

After 30 successfully committed frames, logcat emits one `CAPTURE_MS` aggregate.
Capture `adb logcat -s arcore_app:I '*:S'` and look for that marker:

- `prepare`: admitted GL frame through worker start, including image readback,
  depth acquisition and any pose correction.
- `point_yuv`: input validation, recovery if needed, calibration, point preparation
  and RGB-to-YUV conversion.
- `sdk`: reconstruction, segment extraction and configured component processing.
- `persist_merge`: JPEG/metadata/preview/cloud writes, state commit and mesh merge.
- `estimates`: subsequent sparse/hole estimators.

The subsequent [capture I/O pass](../capture_io/README.md) adds sub-stage timings
for image/metadata, preview sync, cloud/commit, render-lock wait and merge, plus
the current image dimensions and input point count.

Values are average milliseconds for accepted frames. They omit skipped frames,
waiting before admission and general preview rendering, so they are not FPS or
complete frame-time measurements. Compare like-for-like scenes/settings on the
same device and build type before selecting further pipeline changes.
