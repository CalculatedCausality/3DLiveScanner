# Capture-first speed update

## Changes

1. **Exact TPU preprocessing optimization.** A seven-row cache reuses horizontal
   sums, counts and extrema without changing float addition order, quantized
   features, eligibility masks or corrected points. The measured native 160x90
   preprocessing component fell from 8.10 to 3.71 ms in a short ABBA run; that
   baseline was variable. See [the complete measurements](../tests/depth_runtime/PERFORMANCE.md).
2. **Separate coverage preview from model resolution.** Modern live capture uses
   a 5 cm proxy volume when the requested resolution is finer. The camera's depth
   filtering policy and Retango's recorded-point resolution stay at the requested
   value. Point-cloud, RGB and pose recording formats and contents are retained.
   Pose-correction or hole-filling modes retain detailed preview because those
   paths can consume reconstructed geometry when preparing later measurements.
3. **Raw save no longer serializes an unnecessary OBJ.** A synchronized native
   capture barrier checks committed state and the final frame before the existing
   verified dataset publication runs. The model is generated when processing is
   requested. Dataset thumbnails already use recorded JPEGs rather than an OBJ.
4. **Full-detail export cannot accidentally use the proxy.** Opening a recorded
   dataset configures the requested processing resolution. Direct model save from
   a coverage capture first reconstructs at the requested resolution; failure
   retains the recording and returns an error, never exporting the coarse proxy.
5. **Geometry-only modern replay avoids dummy image work.** It supplies no image
   to fusion; real photographs are used in the later texture pass. User undo and
   coloured point-cloud reconstruction still read images.

The coverage option applies to new scans. The installed experiment's TPU toggle,
input backups and original-depth fallback remain available. This update
also includes the farther preview distance and removal of the startup privacy
popup; privacy information remains available manually in Settings.

## What was measured

The production reconstruction core processed the same 12 generated frames and
9,216 points per frame at fine versus coverage-preview resolution. ABBA ordering
used fresh processes. On the Pixel, 1 cm to 5 cm changed:

| Component | Fine preview | Coverage preview |
| --- | ---: | ---: |
| Update total | 1,802.157 ms | 1,049.144 ms |
| Extraction total | 612.517 ms | 33.605 ms |
| Combined | 2,414.674 ms | 1,082.749 ms |
| Preview faces | 179,564 | 6,996 |
| Peak core chunk payload | 28,803,072 bytes | 1,572,864 bytes |

Combined preview work was **55.16% lower (2.23x)**. This is not a whole-app FPS
claim and does not include all camera, JPEG, backup and storage costs. Preview
triangles are intentionally coarser; recorded inputs are not reduced. Fine replay
with/without the previous coarse-preview context produced **4,897,731 identical
mesh bytes** at 1 cm. Every frame was accepted, and complete input sequences were
unchanged. See [the benchmark](../tests/capture_preview/README.md).

## Integration checks

- `tests/capture_policy/run.py`: requested/preview resolution policy; raw-save
  synchronization; no OBJ generation during captured-data save; publication
  failures preserve source; failed detailed replay cannot export a coarse proxy.
- `tests/capture_io/cloud_run.py --modern`: production worker ownership and
  persisted-cloud byte checks, including a coarse preview resolution paired with
  the original recording resolution.
- `tests/texturing_flow/check_android.py`: actual NDK headers and generated
  capture-first JNI contracts.
- Existing lifecycle, control/save and texturing-flow tests continue to cover the
  surrounding capture/export controls.

The final app's end-to-end gain needs comparable live scans. These component
measurements should not be summed or presented as a guaranteed capture-FPS gain.

## Installed-device verification

Installed in place on the Pixel 9 Pro XL on 2026-10-01 after the user confirmed
the previous capture was saved. The artifact is
`artifacts/3DLiveScanner-modern-capture-first-debug-2026-10-01.apk`, SHA-256
`b4e938a73fb9fdf05ba1cb93dee2e7e398120429bc7bd0a9b43cae95bbd5941e`.
The installed APK digest matched, the capture commit-state comparison passed,
and startup reached FileManager. The debug receiver confirmed
`coverage_preview=true`, `capture_first=true`, and TPU cleanup enabled.

The subsequent user-started capture logged `google-edgetpu active native 160x90`
in PID 18223. The scoped crash buffer was empty at inspection. Early 30-frame
TPU-stage summaries ranged from 23.14 ms during initial operation to about
5.53–7.11 ms afterward; later summaries were about 3.43–5.79 ms with fewer
changed points. These include runtime preparation/staging but exclude backup
writes, and workloads varied. They demonstrate live execution, not a controlled
speedup. Capture timing logs also confirmed continued frame persistence and
parallel image/metadata writes.

End-to-end save, reopen and full-detail processing of a new coverage capture
still require a completed recording. The active capture was left running.
