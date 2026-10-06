# Experimental native depth runtime

`common/depth/experimental.cc` implements the shared `experimental.h` API. The
embedded `model_data.h` contains the original Apache-2.0 synthetic-trained
balanced-edge **WIDE_CONTEXT** DCLN0001 model (1,880 bytes):

```
76ef7a8c8376a1a921e2f055419d27b4eaeca425ac2b430dd3f14a89eae84be0
```

## Integration

- Compile `common/depth/experimental.cc` as C++11 with exceptions; link `dl` and
  Android `log`. Do **not** link `neuralnetworks`.
- `SetEnabled`, `Invalidate`, `Status` and `Fallback` never acquire the inference
  mutex or load/compile/execute NNAPI. `Apply` is worker-only. Each call holds a
  shared state reference; controls remain usable during compilation/execution.
- Capture `Generation()` with the owned native frame. The worker exclusively owns
  its points vector and frame while calling `Apply`. The frame's `pointCount`,
  link indices, baseline depths, and original points must match that exact cloud.
- `worldToCamera` is the inverse **COLOR_CAMERA** pose (+Z forward), with the
  reconstruction's axial `minDepth`/`maxDepth`. Each link's `minimumDepth` also
  preserves the producer's offset guard. A correction must preserve the original
  point's axial-range admission: admitted stays admitted; excluded stays excluded.
  The homogeneous input uses `w=1`, not the point's confidence. Output `point.w`
  is copied unchanged.
- A successful inference stages all changes and commits under the same short
  mutex used by generation changes. Stale/error/unavailable calls return false
  without editing points. `Stats.changed` counts committed point changes;
  `Stats.eligible` counts mapped mask-eligible originals with valid world/camera coordinates;
  `Stats.inferred` indicates successful inference, even if later invalidated.
  Milliseconds include preprocessing, lazy compilation, inference and staging.
- The backend explicitly selects only `google-edgetpu` accelerator and requires
  all three convolutions to be supported. There is no CPU/reference/default
  neural fallback. Failure statuses retain original depth.

## Bounded behavior

Native dimensions must be positive, at most 1,024 per axis and 262,144 pixels.
Depth/confidence must be finite, depth nonnegative (at most 10,000 m), confidence
in `[0,1]`; point count is bounded to 262,144. World coordinates and derivatives
are finite and bounded to absolute 1,000,000. Invalid world originals are kept.
Only measured linked eligible points are adjusted by at most ±0.02 m; holes,
unmapped points, invalid/admission-changing corrections and the four-pixel border
are retained. Corrected depth must exceed 0.05 m and the link's minimum.

The runtime caches **one** compilation by native W/H, replacing it on shape
changes. A failed shape is remembered for that generation; toggling off/on or
invalidating permits retry. Native compilation/execution is synchronous on the
worker; generation invalidation discards its result rather than cancelling a
driver call. Optional API30 timeout symbols are resolved independently: compilation
gets a 2-second hint before finish, and every execution gets a 100-ms hint before
compute. Both transient and persistent missed-deadline codes return original
points with an explicit `timeout` fallback status. A rejected timeout-setting call
also falls back. Missing optional symbols preserve operation on older NNAPI.
These are driver hints, not guaranteed wall-clock deadlines; there is no UI
polling, joining, or additional inference thread. Android activation/errors and aggregates every 30 successful frames
are logged with `EXPERIMENTAL` under `DepthTPU`.

## Checks

```sh
# Python with NumPy, g++, GLM from this repository
python3 tests/depth_runtime/run.py
SANITIZE=1 python3 tests/depth_runtime/run.py

# NDK 28.2 defaults to /tmp/opencode/android-sdk/ndk/28.2.13676358;
# override ANDROID_NDK_HOME if needed. Builds only, performs no adb commands.
python3 tests/depth_runtime/build_android.py
```

The host suite injects a private fake NNAPI table only under
`DEPTH_RUNTIME_TEST`. It verifies the real graph builder, arbitrary model scales,
OHWI weights/biases, explicit single-device selection, shape reuse, unavailable
device/library and unsupported graph fallbacks, compilation/execution errors,
optional timeout-symbol combinations, timeout units/order and transient/persistent
deadline failures (including resource cleanup and fresh-execution recovery),
point/array/link validation, positive/negative geometry corrections, range/mask
guards, stale generations during build/inference/commit, and controls while a
worker is blocked in inference. A separate NumPy oracle checks every input and
mask byte for 90 synthetic/boundary cases against `tests/depth_cleanup/data.py`.

For the order-preserving row-cache optimization, frozen-source comparisons,
generated-input host/native component benchmarks, and measured results, see
[PERFORMANCE.md](PERFORMANCE.md). `performance.py` adds 220 random/boundary/extreme
shape cases and compares every feature, mask, full output-point and deterministic
stats byte against the frozen implementation; it also supports ASan/UBSan.

The Android check compiles ABI layout/enum assertions against the actual NDK,
builds an arm64 API24 standalone generated-plane smoke executable and checks
that it has neither `DT_NEEDED libneuralnetworks.so` nor undefined NNAPI symbols.
`native_smoke.cc` runs five 160×120 synthetic frames; it does not use the camera
or app. It checks actual inference and bounded geometry changes, not scan quality.

Regenerate the embedded header only from the selected model (the script verifies
the expected hash before writing):

```sh
python3 tests/depth_runtime/embed_model.py /path/to/model.bin common/depth/model_data.h
```
