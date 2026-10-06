# Capture I/O and ownership checks

Latest follow-up: [bounded parallel frame persistence](parallel_writes.md), with
phone-side synthetic measurements, failure checks and updated timing semantics.

## Device baseline

Wireless ADB identified the reported phone as a Pixel 9 Pro XL running Android 17
(4 KiB pages). Its installed APK matched the prior capture-speed APK SHA-256
`5a8aedba5ca9e0608916c547202335427d1c26c0dce5452d587d6f4da74ef4a0`.
The user performed a normal scan with unchanged settings. No scene images were
downloaded for profiling.

Six `CAPTURE_MS` windows on September 30, 2026 each represented 30 committed
frames. Average stage times were:

| Stage | Average milliseconds |
| --- | ---: |
| Prepare | 26.37 |
| Point/YUV preparation | 24.25 |
| SDK reconstruction/extraction | 89.39 |
| Persistence + mesh publication | 126.45 |
| Subsequent estimators | 0.00 |

Five successive window intervals covered 150 committed frames in 43.605 seconds,
approximately **3.44 committed frames/second**. This is not camera/preview FPS.
Scene complexity and movement were not laboratory-controlled, and these windows
do not identify how much of persistence was encoding, writing, sync or lock wait.

## Changes

- Preview serialization coalesces the existing small headers/arrays through a
  256 KiB owned stdio buffer. The byte format and final `fflush`/`fsync`/`fclose`
  checks are unchanged. Allocation failure falls back to ordinary stdio.
- One independently owned Tango point cloud is used for SDK integration and
  persistence, then released before commit/failure handling and binder release.
  See [cloud checks](cloud_README.md).
- The timing line now separates `image_meta`, `preview_sync`, `cloud_commit`,
  `merge_wait` and `merge`. Those sub-stages sum to `persist_merge`. Current image
  dimensions and input point count are included to interpret workload changes.

## Reproduce host checks

```sh
python3 tests/capture_io/cloud_run.py
python3 tests/capture_io/cloud_run.py --modern
python3 tests/capture_io/preview_run.py
```

The preview test compiles real Dataset writing/reading code, verifies bytes and
readback for empty, deleted, small and large segment lists, tests repeated writes
and a final flush failure using `/dev/full`, and runs ASan/UBSan. If `strace` is
available, it counts actual host write/fsync calls for a 64-segment fixture.

The cloud runner covers 31 worker failure/success cases and nine validation cases
in each backend mode. Modern mode additionally rejects repeated atomic resource
and SDK errors, then accepts the next valid frame without replaying history.
Extraction and persistence failures following a successful SDK update still
require recovery. These are production wrapper/worker tests with an instrumented
SDK boundary; the real owned core's rollback checks live in `tests/reconstruction`.

For a before/after syscall comparison, save the working source before editing:

```sh
python3 tests/capture_io/preview_run.py --snapshot /path/to/before-dataset.cc
# Apply the optimization, then:
python3 tests/capture_io/preview_run.py --baseline /path/to/before-dataset.cc
```

The captured baseline for this pass is
`/tmp/opencode/capture-preview-before-20260930.cc`. The 1,312,004-byte fixture used
**321 → 6 file-write syscalls**, with **one fsync in both versions** and identical
serialized data. Android filesystem behavior and end-to-end speed must be
measured on-device; host syscall counts alone are not a phone speedup claim.

## Second device run

The I/O/cloud-reuse APK (`3f7685a3a9a49f262c93bdf13ae32fadb690c32a7895dc0acec056799c2230a0`)
was signature-verified, installed in place through wireless ADB, and exercised by
the user. Eleven windows reported 360×640 images. Last-frame input point counts
varied from 3,055 to 13,385; they are not window-average point counts.

Average milliseconds across those windows:

| Stage | Milliseconds |
| --- | ---: |
| Prepare | 37.61 |
| Point/YUV preparation | 26.91 |
| SDK | 52.03 |
| Persistence + publication | 113.15 |
| — image/metadata | 43.86 |
| — preview/sync | 15.09 |
| — cloud/commit | 37.72 |
| — render-lock wait | 16.17 |
| — merge | 0.30 |

The ten intervals covered 300 committed frames in 89.596 seconds (about **3.35
committed frames/second**). Two windows had large preparation delays. The scene
and admission pattern changed, so this is **not evidence of an overall FPS
improvement**. Persistence was lower in this run, but an exact causal speedup
cannot be assigned from these uncontrolled captures.

Repeated color-calibration setup was identified as a further candidate. The
source now caches identical calibration values per context, invalidates on
replacement (including handle reuse), retries failed setup and logs calibration
time separately. `calibration_run.py` tests the real cache with an instrumented
SDK boundary. This additional cache has not yet been measured on the phone.

During this run Android showed the native 16 KiB compatibility warning. The
phone reports a 4 KiB kernel; no compatibility warning was suppressed. The user
prioritized a full backend/toolchain migration, documented in
`reconstruction/MIGRATION.md`, before further runtime claims.

## Modern repair device check

Installed repair APK SHA-256:
`9332601242df7cc6e1698b6c5bcdc748104ecd02ff19df430550c82d1f54a90d`.
The device's installed `base.apk` hash matched. The user operated the camera and
then paused; UI inspection showed **“Paused · Resume when ready”**, and the
dataset's committed `state.txt` reported **98 frames, 360×640**. Only that small
state metadata was read; no captured images were downloaded.

On September 30, 2026, the scoped live log observation included capture from
20:35:08 through 20:35:42 device time. It contained no reconstruction rejection,
extraction failure, resource-limit diagnostic, or AndroidRuntime crash. Three
30-committed-frame windows averaged:

| Stage | Milliseconds |
| --- | ---: |
| Prepare | 30.28 |
| Point/YUV | 4.06 |
| Reconstruction + extraction | 141.38 |
| Persistence + publication | 136.98 |
| — image/metadata | 54.57 |
| — preview/sync | 21.03 |
| — cloud/commit | 42.39 |
| — render-lock wait | 18.60 |
| — merge | 0.41 |

Reports at 20:35:18.227 and 20:35:38.884 bracket 60 committed frames in 20.657
seconds: **2.90 committed frames/s**, not preview FPS. Input point counts at each
report were 9,443, 7,886 and 12,897. Post-pause PSS was approximately 497 MiB.

This short trial did not reproduce the recovery stall. It does not prove sustained
large-scan capacity, device sleep/wake recovery, exported quality, or a speedup
over the earlier legacy runs: the inputs were uncontrolled, and the owned engine
still spends substantial time in reconstruction and persistence. Host exact-byte
performance comparisons are separately documented in
`tests/reconstruction/core_performance.md`.
