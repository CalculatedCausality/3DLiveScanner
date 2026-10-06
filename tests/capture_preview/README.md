# Capture coverage-preview benchmark

This measures **native preview fusion + dirty-segment extraction**, comparing a
requested 1 cm (or 2 cm) voxel size with a 5 cm coverage-preview voxel size. It
links the actual `reconstruction/core.cc`, without test-core substitutes or changes
to production code. It does not measure whole-app FPS, camera acquisition, depth
inference, rendering, recording I/O, final texturing, or output quality.

## Workload

- Exactly 12 frames by default; the runner refuses more than 12.
- Reuses `capture()` from `tests/reconstruction/core_benchmark.cc`: moving poses,
  room walls, box, sphere, deterministic depth noise/confidence, color-camera
  extrinsic offset, distorted calibration, and padded RGB rows.
- The scene, depth points and translations are uniformly scaled to 0.35 of the
  original size **once, before either variant**. This bounds the 1 cm workload.
  All 9,216 four-component points/frame remain, including confidence values;
  RGB stays 96×96 with 296-byte row stride. Intrinsics and orientations are shared.
- Both variants use the same binary, scene, frame timestamps, points, RGB, poses,
  calibration, min/max depth (0.1/5 m), color and space clearing. Only the core
  resolution changes. Default production work/capacity limits remain enabled.
- RAM contexts are used, not the application's paged-volume integration.

## Correctness guards

Every update must succeed; a dropped frame is a failure, never a speedup. Every
extracted mesh must have finite vertices/normals, valid distinct face indices,
and nonzero triangle area; the final accumulated mesh must be nonempty.

Field-wise input serialization includes all point bytes, RGB **including row
padding**, poses, camera calibration, timestamp, dimensions, stride and format.
Every frame checks the complete input sequence against an immutable byte copy.
The runner also compares saved input dumps exactly and records SHA-256; the
native harness records before/after FNV-1a checksums.

Two separate replay processes check requested-resolution reconstruction with:

1. A fresh fine context and no prior coverage run.
2. A coarse preview context, its destruction, and then a **new fine context**,
   consuming the same original frames.

Replay extracts only at the end. Complete sorted mesh snapshots (including
metadata, vertices, normals, colors and faces) must be byte-identical between
these two paths and the final fine live-preview mesh. Repeated coarse snapshots
must also match. Comparisons are within a platform/build; libm differences mean
host and Android generated inputs need not have the same hashes.

These are native isolation guards. App checks that recorded PCL/RGB/pose data,
camera/Retango requested resolution, and subsequent processing configuration stay
unchanged belong to the integration tests. The 5 cm mesh is coverage feedback,
not an assertion about final output quality.

## Timing and memory

The measured order is **fine, coverage, coverage, fine (ABBA)**, with a fresh
process/context each time. There is no untimed warm-up. `steady_clock` measures
each update call and each returned dirty-segment extraction separately. Geometry
validation, input equality checks, output serialization, mesh destruction and
final full-volume snapshot are outside these timers. They still affect caches
and allocator state; this is a controlled checked workload, not an app trace.

Reports include individual frame times, frame median/p95, median 12-frame sums,
combined totals, all statuses, dirty counts, accumulated final mesh counts,
aggregate final mesh payload, core resident/peak chunk payload and process peak
RSS. RSS includes the fixture, immutable inputs, validation buffers and allocator
overhead; it is not app RAM. Replay-after-coarse RSS includes the earlier context
and is excluded from the preview memory comparison. Final snapshot extraction is
reported separately and excluded from preview totals. No speedup threshold is
asserted: regressions should remain visible, and two repetitions are not a
statistical confidence interval.

## Commands

Use new output directories under `/tmp/opencode`; generated reports and binaries
never go into the repository. Run longer commands through the daemon tool.

```sh
python3 tests/capture_preview/run.py \
  --output /tmp/opencode/capture-preview-host-1cm --cpu 2

python3 tests/capture_preview/run.py \
  --output /tmp/opencode/capture-preview-host-2cm --cpu 2 --fine .02

python3 tests/capture_preview/run.py \
  --output /tmp/opencode/capture-preview-sanitized --sanitize --cpu 2

python3 tests/capture_preview/run.py \
  --output /tmp/opencode/capture-preview-phone-1cm \
  --sdk /tmp/opencode/android-sdk --serial 192.168.1.114:33123
```

Android builds use NDK `28.2.13676358`, ARM64/API24, static C++ runtime and 16 KiB
link alignment. They push only a standalone synthetic executable to a unique
`/data/local/tmp/capture-preview-*` directory, run at nice level 10 with default
CPU scheduling, pull synthetic dumps, then remove that directory. No app install,
restart, settings changes, scan access or camera access is involved. Concurrent
phone use/thermal/scheduling variation can affect results.

`manifest.json` records compilation arguments, compiler and source SHA-256 values
before building. `results.json` adds raw runs, guards, summaries, gains/regressions,
device metadata and a post-run source-hash check. Every run also retains its own
JSON/stderr, input dump and final mesh dump. Sanitized times are diagnostic only.
