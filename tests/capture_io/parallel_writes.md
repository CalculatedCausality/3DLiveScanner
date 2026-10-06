# Bounded parallel frame persistence

## Motivation and phone-side synthetic evidence

The preceding real Pixel capture averaged 141.38 ms in reconstruction/extraction
and 136.98 ms in persistence/publication. `image_meta` included JPEG writing and
two metadata files; its 54.57 ms was not a measurement of JPEG encoding alone.

`device_io_probe.py` links the existing production ARM64 JPEG archive, with the
same 360×640 RGBA input convention, quality 85, 4:4:4, FASTDCT and BOTTOMUP flags.
It encodes a deterministic generated pattern and writes only its own isolated
scratch files. It never opens captured scan data. Payloads are a 95,136-byte JPEG,
1,312,004-byte preview substitute and 147,460-byte cloud substitute, plus small
pose, timestamp and temporary/renamed state files. The preview retains one fsync.
Scratch files/directories and the uploaded test binary are removed after use.

This runs as **adb shell**, not the app UID, and uses synthetic payloads. It does
not exercise the complete Dataset/AR/render pipeline, prove power-loss durability,
or predict complete scanner FPS. Shared-storage access checks, scheduling and
thermal conditions can differ from the app's actual workload.

Initial two alternating filesystem probes found:

- JPEG encoding: approximately 5–10 ms mean, depending on run.
- Pose/timestamp writes: approximately 18–21 ms on shared storage, versus about
  0.1 ms in `/data/local/tmp`.
- Publishing the small state file: approximately 18–21 ms on shared storage.

A follow-up compared serial I/O against one concurrent image/metadata writer,
joining it before state publication. Each sequence used 32 frames. Shared-storage
runs were ordered serial, parallel, parallel, serial:

| Mode | Mean total per synthetic frame | Median | p95 |
| --- | ---: | ---: | ---: |
| Serial 1 | 80.264 ms | 78.272 ms | 109.185 ms |
| Parallel 1 | 69.413 ms | 69.632 ms | 87.542 ms |
| Parallel 2 | 69.677 ms | 66.097 ms | 92.368 ms |
| Serial 2 | 81.874 ms | 83.301 ms | 96.526 ms |

The two means average **81.069 → 69.545 ms**, a **14.2% reduction** in this
synthetic persistence workload. Individual writes became slower under contention;
overlap nevertheless shortened the total. No whole-app speedup is inferred.

## Production change

For the modern build, `FrameWriter` overlaps JPEG/pose/timestamp saving with the
existing preview and point-cloud writes. The capture worker still owns the
binder, point cloud, image and frame metadata throughout. It joins the single
writer before freeing inputs, publishing state, merging geometry or releasing
the binder. There is no cross-frame queue or additional captured-image copy.

Publication requires success from both branches. A failed image task can leave
uncommitted preview/cloud files, but it cannot publish a partial frame or merge
its geometry. Write/exception failures retain the prior committed history and use
the existing pause/recovery path. The writer catches task exceptions and joins
on destruction. If a thread cannot be started due to allocation/thread-resource
failure, it falls back to the serial path. Legacy capture remains serial.

Resolution, point density, JPEG flags/quality, file formats, preview fsync and
state-last publication are unchanged.

### Timing interpretation

`CAPTURE_MS` now includes `io_parallel`, the percentage of its 30 committed frames
that used an asynchronous writer. `persist_merge` remains the actual elapsed
wall time of persistence and publication. With overlap, **do not sum its child
stages**: `image_meta` overlaps preview/cloud work, and `cloud_commit` includes any
remaining writer join wait and state publication. This replaces the old serial
substage-sum interpretation for the new modern build.

## Checks

```sh
python3 tests/capture_io/frame_writer_run.py
python3 tests/capture_io/cloud_run.py --modern
python3 tests/capture_io/cloud_run.py
python3 tests/texturing_flow/check_android.py
```

The writer's real code is tested with held-task synchronization, exactly-once
launch fallback, task exceptions and borrowed-input lifetime checks. Production
worker definitions cover 31 success/failure cases, nine input-validation cases,
and modern cases that hold the image task until preview work starts. These ensure
the writer is joined before cloud destruction and failed branches never commit.
SDK/image boundaries are labelled fakes; the actual cloud serializer is exercised
and successful payloads compared byte-for-byte.

To reproduce the separate phone-side probe, supply the local SDK and an already
built production JPEG archive:

```sh
python3 tests/capture_io/device_io_probe.py --serial DEVICE \
  --sdk /path/to/android-sdk --jpeg-archive /path/to/libjpeg-turbo.a
```

It requires NDK 28.2.13676358 and writable `/data/local/tmp` and shared `Download`.
Direct-path comparison motivates further investigation of working storage; no
library relocation or private-storage migration is part of this change.

## Installed-app short trial

The verified APK `2444df0c07967828f20002488e1a3deb7506f4a868b730c03dd9f66244605c14`
was installed on the Pixel after the user confirmed the previous scan was saved.
The installed `base.apk` hash matched. After a new user-operated capture, the
dataset state reported **43 committed frames, 360×640**, and UI inspection showed
**“Paused · Resume when ready.”** A bounded capture-log read retrieved this report
at 21:15:09.345 device time (September 30, 2026):

| Metric | Milliseconds |
| --- | ---: |
| Prepare | 25.70 |
| Point/YUV | 3.44 |
| Reconstruction/extraction | 109.79 |
| Persistence/publication | 122.05 |
| Image/metadata branch | 72.79 |
| Preview/sync | 20.44 |
| Cloud/join/commit | 73.93 |
| Render-lock wait | 14.87 |
| Merge | 0.44 |

The report covered 30 committed frames with **`io_parallel=100%`**. Its final
input point count was 4,332. Persistence/publication was about 10.9% lower than
the preceding trial's 136.98 ms mean, but that trial's final point counts were
7,886–12,897. This is an observed difference, not an isolated causal estimate.
Only one report was available for this capture, so no inter-report committed-FPS
estimate is given. The initial live observation ended during preceding save/library
activity; the capture timing was recovered with a later bounded log snapshot.
The post-pause process-specific query found no app-error, resource-limit or
AndroidRuntime error messages in the available bounded log window.

Library activity also logged Android `EACCES` errors opening some older model
resources. This is a separate observed file-access problem; the capture timing
does not establish historical-library readability or exported-model quality.
