# Installed live TPU test mode

At the user's explicit request, the trained model is now available as an
**experimental live-scanning mode**, rather than requiring production approval
before phone testing.

Installed APK: `3DLiveScanner-modern-tpu-test-debug-2026-10-01.apk`

SHA-256: `393530bb7bb74229af3257c9a0d285ad720232fc278073e55fc9a111022811cc`

Model SHA-256: `76ef7a8c8376a1a921e2f055419d27b4eaeca425ac2b430dd3f14a89eae84be0`

## Using it

**Settings → Realtime mode → Scan parameters → Experimental TPU depth cleanup**.
The switch is enabled on the connected Pixel. It applies to new live depth
measurements, not re-exporting an older captured dataset. Use separate fresh scans
for an on/off comparison; switching off does not undo already-fused corrections.

Live status shows one of:

- `TPU test: initializing`
- `TPU test: active WIDTHxHEIGHT N points Xms`
- `TPU test: original depth (reason)`
- `TPU test: off`

The label is experimental because real-world quality is still being assessed.
The prior synthetic detail gates passed, but the aggregate RMSE-advantage gate
was not met; enabling testing does not reclassify that result as production approval.

## Verified real operation

The in-place install and installed APK hash were verified, capture/dataset state
was preserved across the update, and startup reached FileManager. The protected
debug control returned `enabled; TPU test: initializing`.

The subsequent live scan reported `google-edgetpu active native 160x90` and
repeated 30-frame summaries. One report had **80,926 changed points**; later
summaries had about **178k–188k changed points per 30 frames**, with roughly
**10.6–14.6 ms mean runtime work** per frame. These timings include preprocessing
and staging and are distinct from the earlier sub-millisecond prepared-input
NNAPI microbenchmark.

A read-only check found **1,248 committed frames** and a committed `.tpu` backup
for frame 1,247: native 160x90 input, 12,187 original/corrected world points,
8,307 measured point links, and the expected model hash. Only the fixed metadata
header was read, not photos or the point payload. This confirms the live path and
backups; it is not a ground-truth scan-accuracy measurement.

## Implementation

`ARCore::UpdateFeaturePoints()` copies validated native depth/confidence planes
only when test mode is enabled. Row and pixel strides are honored. No resizing
is used. The runtime constructs and caches a graph for the actual native W/H;
the Pixel's live 160x90 plane is not reinterpreted as the 160x120 research fixture.

The existing producer performs its normal admission, fill, secondary-depth
filtering and sampling. Only high-confidence measured samples receive links to
their original world points. Filled/wall points are not linked. The model sees
the existing effective baseline depth at linked samples, so its residual is
applied to the depth it actually evaluated.

`ProcessReconstruction()` executes inference on its existing worker, before
Retango/fusion, while owning the admitted frame's RGB/pose/cloud through the
binder. There is no NNAPI compilation or inference on GL/UI. Each link contains
the captured world-space depth derivative, allowing a bounded axial residual to
move that exact point without reusing a later camera pose.

Pause, scan-stop, clear, session replacement and tracking loss invalidate the
generation. Stale, unsupported or failed inference keeps original points.
Point counts/confidence, holes and core axial-range admission are retained;
changes are bounded to ±20 mm of axial depth. Runtime controls and status use a
short separate lock, not the compilation/execution mutex.

NNAPI is loaded dynamically and selects only `google-edgetpu`. The APK has no
mandatory `libneuralnetworks.so` dependency and keeps minSDK24 compatibility.
The switch is unavailable below API29 or in the legacy flavor. Missing library,
symbols, device, shape support or successful execution uses original depth.
Compilation/execution timeouts are optional driver hints (2 s / 100 ms), not
guaranteed wall-clock cancellation.

## Original-input backups

Every frame with committed point changes first writes `NNNNNNNN.tpu` beside its
capture files. Failure to save that backup rolls the point cloud back to original
depth before fusion. The normal capture-state commit remains last. Existing
capture formats are unchanged; extra sidecars are for comparison/recovery tools,
not a new automatic undo UI.

The little-endian `DPTR0001` layout is:

1. 8-byte magic, 64-byte ASCII model SHA-256.
2. Four uint32 values: native width, height, world-point count, link count.
3. Three 64-bit values: camera timestamp, depth timestamp, generation.
4. Sixteen float64 column-major world-to-colour-camera matrix values, then min/max
   axial-depth float64 bounds. The fixed header totals 256 bytes.
5. `W*H` float32 model-input depths in metres, then the same number of confidence
   float32 values.
6. Original and corrected world-point arrays, each four float32 values per point.
7. Link pairs `(point index, native pixel index)`, two uint32 values each.

This is the model's effective input plus original world measurements, not an
unmodified dump of all raw/secondary ARCore image buffers. Backups consume extra
storage and I/O in test mode. They do not overwrite any existing saved scan.

## Checks

- Runtime ASan/UBSan and fault/generation tests; 90 byte-exact preprocessing cases.
- Padded/unaligned planes, ownership, exact producer filtering cutoffs, world-ray
  correction, backup publication and worker rollback tests.
- Camera lifecycle, original capture pipeline and real NDK/JNI checks.
- Modern APK build and legacy Java/resource compilation.
- Static four-library 16 KiB audit, zipalign and APK signature verification.
- Actual live Pixel activation and committed original-input backup inspection.

The debug-only `TpuTestReceiver` is protected by `android.permission.DUMP` for
ADB control, not an unrestricted external toggle. It can also request a generated
app-UID runtime test on a background thread without camera or capture access.
