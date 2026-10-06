# Live capture-loop throughput

## Observed bottleneck

On the capture-first build `b4e938a7…`, consecutive live 30-frame reports were
about four seconds apart (roughly 7.3 committed captures/s in the observed
window). Each capture spent about 15–27 ms waiting for `render_mutex_` before
publishing committed preview meshes, although the merge itself took only
0.07–0.11 ms. The display held that mutex across blocking ARCore camera update,
image readback, scene rendering and depth-overlay rendering.

This wait also held the frame-admission binder. A display iteration could finish
its camera update, fail to admit the next capture because the worker still held
the binder, then release the render lock the worker needed to finish. The
capture therefore waited for a further display iteration.

## Change

- The worker publishes committed preview meshes under a separate `scan_mutex_`.
- `App::OnDrawScan` holds that mutex only while looking up and drawing mesh
  buffers. Cached pointers are invalidated by the existing mesh revision.
- `GetScanSize` takes both scene and mesh locks because it is a concurrent reader.
- Setup also excludes drawing while resetting a previous preview context.
- Other destructive operations retain binder + scene exclusion. The lock order
  is binder → scene → mesh where those locks are combined; the live publication
  worker takes binder → mesh and does not wait for the camera/display lock.
- `CAPTURE_MS` now includes `interval` and `capture_hz`, calculated from 29
  start-to-start intervals across 30 committed captures. Pauses/tracking gaps
  inside the window are included; this is not display FPS or raw sensor FPS.

There is no change to recorded resolution, point filtering, RGB encoding,
original-depth backups, storage format or commit-before-publication ordering.
The preceding 5 cm coverage-preview setting is retained, not further reduced.

## Verification

- `tests/capture_io/cloud_run.py --modern` and the legacy branch: 33 production
  worker failure/success cases, nine borrowed-cloud validation cases, exact
  persisted-cloud bytes and cleanup/commit ordering.
- Additional production-worker concurrency cases prove publication finishes
  while the camera/display lock is held, but waits for an active mesh reader.
- `tests/capture_io/preview_lock_run.py` runs production draw/count functions with
  a blocked fake GL call: mesh arrays remain alive throughout drawing, the
  publisher waits for the reader, stale cached pointers are invalidated, count
  queries wait for publication, and early returns release the locks.
- These host tests run with address/undefined-behavior sanitizers. They are
  correctness checks, not Android GPU or throughput simulations.
- Existing lifecycle, capture-first save/replay and real NDK/JNI checks passed.
- Modern APK assembly, dependency/static 16 KiB audit, zipalign and signature
  verification passed.

## Installed build

`artifacts/3DLiveScanner-modern-scan-loop-debug-2026-10-01.apk`

SHA-256: `774723b75df88daf335a26691f600289a835f418bbf8c0cfe8234584c5333653`

Installed in place after the user confirmed the prior scan was saved. Installed
bytes matched; the capture commit-state comparison passed; startup reached
FileManager. Capture-first, coverage preview and experimental TPU cleanup remain
enabled. Subsequent capture throughput must be read from this build's live
telemetry, rather than inferred from the earlier preview-core benchmark.

## Live result after installation

The user's next scan (PID 16739) confirmed requested resolution **2 cm**, coverage
preview **5 cm**, and `google-edgetpu active native 160x90`. Eight 30-committed-frame
reports gave:

| Window | Capture Hz | Merge wait (ms/frame) | Reconstruction (ms/frame) |
| --- | ---: | ---: | ---: |
| 1 | 8.06 | 0.00 | 28.36 |
| 2 | 12.64 | 0.00 | 18.53 |
| 3 | 10.34 | 0.00 | 34.33 |
| 4, includes a long gap | 0.72 | 0.00 | 29.51 |
| 5 | 8.19 | 0.00 | 6.13 |
| 6 | 13.99 | 0.01 | 11.71 |
| 7 | 6.80 | 0.01 | 77.94 |
| 8 | 9.26 | 0.00 | 40.92 |

The measured lock wait fell from about **15–27 ms to 0–0.01 ms**. Persistence plus
publication was **10.22–12.95 ms/frame** in these reports. Ordinary windows ranged
from **6.80 to 13.99 captured frames/s**; one window included roughly 40 seconds
between reports. Its cause was not determined and it remains in the table.

This is an uncontrolled before/after observation: scene, movement, input points,
tracking and device load differed. It supports elimination of the observed
publication wait and confirms live operation, not a fixed percentage FPS gain.
Reconstruction itself varied from 6.13 to 77.94 ms/frame and still limits heavier
frames. No crashes appeared in the scoped process crash buffer. The observer
neither started nor stopped the camera.

Raw bounded telemetry: `/tmp/opencode/scanner-build-verification/scan-loop-live-20261001.json`.
