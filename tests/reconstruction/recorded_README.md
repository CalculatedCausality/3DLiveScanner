# Fixed geometry-only dataset replay benchmark

## Current outcome

A real recorded baseline is now available from a **451-frame saved raw dataset**
in the connected Pixel's library. Acquisition verified a stable committed state,
point/pose inventory and remote/local hashes. It acquired only **903 geometry/
metadata files, 75,525,545 bytes, containing 4,707,444 input observations**. No
images, exported models or settings were acquired. The source files were not
modified. The device storing this recording does not establish its original
capture hardware or resolution.

The first attempt against the earlier temporary 43-frame capture correctly
aborted because its `state.txt` was missing. The subsequent library check found
the saved raw dataset; it was not replaced by synthetic data. The small synthetic
fixture remains a separately labelled validation fixture.

All payloads, manifests, generated build files, ordered mesh data and result JSON
stay outside the repository. No production engine, dataset reader, Java, worker,
storage or build file is changed by this harness.

For controlled engine comparisons, `run --core-source /path/to/snapshot/core.cc`
compiles a frozen source with its sibling private headers. Reports hash the source
and **all sibling headers**, including meshing, field-normal and ray-fusion
helpers, and recheck them after replay. Omit this option to test the working tree.
Keep the fixture and full configuration identical between variants; compare
geometry separately from timing and shading-normal changes.

### Real-recording baseline (2026-09-30)

Frozen input: `/tmp/opencode/recorded-pixel-451-geometry-20260930`.
Report: `/tmp/opencode/recorded-pixel-451-baseline-20260930/results.json`.

| Diagnostic | Result |
| --- | ---: |
| Accepted frames | 451 / 451 |
| Segments / vertices / triangles | 414 / 286,238 / 512,345 |
| Nonfinite output / exact degenerate triangles | 0 / 0 |
| Near-degenerate triangles, area ≤ 1e-12 m² | 6 |
| Nonmanifold / inconsistent-winding edges | 0 / 0 |
| Paired cross-chunk edges | 18,131 |
| Unpaired chunk-plane edge candidates | 1,326 |
| Sampled observations | 4,059 |
| Input-to-final-surface median / RMS / p95 | 18.34 / 48.74 / 101.46 mm |

These residuals measure self-consistency, **not physical accuracy**. Open edges
and chunk-plane edge candidates are not by themselves proof of reconstruction
defects. The fixed configuration below is experimental (including color disabled
and component filtering set to zero), not a reconstruction of unknown original
settings. This mesh's 512,345 triangles exceed the current texturer's 500,000-face
budget; that flags a future large-model/simplification check, not proof that a
different live configuration produces the same export size.

Two optimized repeats produced exactly the same ordered mesh, diagnostics and
residuals. Host replay times were 88.637 / 84.655 seconds for the full 451 frames;
analysis took 0.880 / 0.545 seconds. Replay peak RSS was about 72.7 MiB and overall
analysis peak RSS about 127.5 MiB in the first run. These are not phone FPS.
A full 451-frame ASan/UBSan/leak-check replay also passed and produced the same
ordered mesh digest. Its report is
`/tmp/opencode/recorded-pixel-451-sanitized-20260930/results.json`.

- Fixture SHA-256: `d1e255691f7982d18037d8e76603ca9ce157a07b496d54cf6cf9a6e890da5c4f`
- Ordered mesh SHA-256: `2ac8d23ebf4472afa2ec05e77fc62b40e5938b2d27130a02e6e7e01637f443e0`
- Core source SHA-256: `97a16e50220b76c3baf124071f9f2b6e323814d5208a43f83f8837f2eb093e71`

No legacy Tango comparison or texture-quality comparison has been performed.

## Files

- `recorded.py`: guarded acquisition, strict validation, fixture hashing, synthetic
  validation generator, host build/replay and deterministic-repeat checks.
- `recorded_replay.cc`: production Dataset reader integration, incremental mesh
  replacement, canonical ordered output, diagnostics and timing/RSS reporting.
- `recorded_report.py BEFORE AFTER`: checks identical fixture/configuration,
  paging budgets and frame acceptance before summarizing fusion/extraction time,
  geometry, residuals, memory and paging costs. It does not require identical
  output bytes when deliberately comparing different mesh algorithms.
- `recorded_geometry.h`: bounded triangle BVH and point-to-triangle distance queries,
  with analytic/brute-force cross-checks.
- `recorded_acquisition_test.py`: strict fake-ADB tests; never contacts a device.
- `recorded_device_test.py`: private `run-as` acquisition roundtrip using an
  isolated, generated six-frame fixture; never reads a real scan. All synthetic
  device/local payloads are removed afterwards. It passed on the Pixel with all
  13 geometry/metadata files byte-identical.

## Acquire only a stable committed prefix

Set `PIXEL_SERIAL` to the currently authorized paired serial. Use a new output
directory, and keep the capture paused while acquiring:

```sh
python3 tests/reconstruction/recorded.py acquire \
  --adb /tmp/opencode/pixelshare-sdk/platform-tools/adb \
  --serial "$PIXEL_SERIAL" \
  --remote '/storage/emulated/0/Documents/3D Live Scanner/dataset' \
  --output /tmp/opencode/pixel-geometry-new-snapshot \
  --expect-count 43 --expect-width 360 --expect-height 640
```

For a fresh capture using the modern private workspace, use the debug app's
existing `run-as` access (no permission changes):

```sh
python3 tests/reconstruction/recorded.py acquire \
  --adb /tmp/opencode/pixelshare-sdk/platform-tools/adb --serial "$PIXEL_SERIAL" \
  --run-as com.lvonasek.arcore3dscanner \
  --remote /data/user/0/com.lvonasek.arcore3dscanner/files/capture-dataset \
  --output /tmp/opencode/pixel-private-geometry-new-snapshot
```

This uses read-only `run-as` commands and streams the same explicit allowlist
through `exec-out`; ordinary ADB pull cannot open app-private files. Public and
private acquisition both run the same stability/hash/format checks.

Acquisition performs only remote `cat`, `stat`, `sha256sum`, and explicit per-file
ADB pulls (or private `run-as` reads). Its content allowlist is `state.txt` and exactly the numbered `.pcl` and
`.mat` files for frames `0..count-1`. It never pulls directories, `.jpg`, `.png`,
`.bin` previews, `.tms`, a saved-model library, or settings. It does not start/stop
capture or change app/device settings.

Guards:

1. Require valid commit state and optional expected count/dimensions.
2. Require every committed point/pose file and bounded sizes; refuse an outstanding
   `state.txt.tmp` commit marker.
3. Recheck state and the point/pose inventory after two seconds. Any observed
   change aborts before pulling.
4. Hash every allowlisted remote file, pull only those files, then compare state,
   inventory, remote hashes and state again. Compare all local bytes/hashes.
5. Strictly validate payloads and publish `manifest.json` only after all checks pass.

Stable files beyond the committed count may be old/uncommitted leftovers. Their
inventory is observed for changes, but their contents are **not hashed or pulled**;
the manifest records the ignored file count. Missing/changing/incomplete input
causes failure. A mid-pull failure may leave a partial **local** directory without
a valid manifest; it is not accepted as a frozen fixture, overwritten or resumed.
No remote cleanup is attempted. These checks establish observed byte stability,
not an atomic snapshot against an adversarial concurrent writer.

## Reader and pose semantics

The host binary links the **real `common/data/dataset.cc` readers**. It compiles
with `ANDROID` so point-cloud allocation and cleanup use the actual core C API.
Link-time section garbage collection removes unrelated preview/image operations.
Existing host GL/log declaration headers are used only to compile those includes;
no camera SDK, image codec or vendor reconstruction implementation is executed.

- File names come from `Dataset::GetFileName`: eight-digit frame indices.
- `.pcl`: little-endian uint32 count, then count native IEEE float32 XYZC vectors,
  16 bytes per point. No rescaling or reordered observations.
- `.mat`: three GLM float matrices, each four **columns** of four textual floats.
  `COLOR_CAMERA` is matrix zero. The screen matrix may be projective, not rigid.
- `Extract3DRPose`'s current production function body is extracted and compiled as
  a standalone adapter. It uses `glm::quat_cast(matrix)` and translation column
  three, preserving the float conversion and quaternion x/y/z/w order. Its body
  SHA-256 and full source SHA-256 are recorded. This is not a guessed transpose,
  inverse pose or independently invented decomposition.
- Core normalizes the nearly-unit quaternion as usual. Residual sample positions
  use the same normalized quaternion/translation in double precision.
- `.tms` is deliberately not acquired. `Dataset::ReadPointCloud` returns timestamp
  zero, which this benchmark preserves. Frame order is the numeric committed order;
  there is no capture-time/FPS inference.

Preflight is intentionally stricter than the bare Dataset readers: exact file
length/token counts, no trailing bytes, no symlinks, finite floats, positive depth
and confidence in `(0,1]`, rigid/nonreflected color-camera poses, valid state, complete
frame pairs and matching manifest hashes. The positive-confidence/depth requirement
matches the app's recording replay admission contract. Production `RigidPose` is
also checked in C++. Run through the Python wrapper so strict preflight and
before/after source/input integrity checks are applied.

## Fixed experimental configuration

The acquired file schema contains no explicit scan resolution. **0.04 m is an
experimental benchmark setting, not a claim about the captured setting.** A
different explicit `--resolution` creates a different benchmark configuration.
Compare quality only when both fixture digest and full config match.

All config fields are set explicitly and recorded:

| Setting | Value |
| --- | --- |
| resolution | 0.04 m by default |
| min/max axial depth | 0 / 15 m |
| generate_color | **false**; no image supplied |
| use_space_clearing | true |
| use_parallel_integration / clockwise winding | false / false |
| rectify_color_image | true; irrelevant without images |
| max_voxel_weight / min_num_vertices | 16383 / 0 |
| update_method | traversal (0) |
| max_chunks / max_update_chunks | 1024 / 256 |
| max_update_work | 32,000,000 |
| min_confidence / min_voxel_weight | 0 / 1 |

Explicit paging/RAM comparison runs may override the logical and per-frame chunk
budgets; `--max-update-chunks` defaults to 256 and is recorded in the full config.
The application-profile 512-chunk comparison is documented in
`docs/reconstruction-paging.md`. It does not change this harness's default config.

Each successful update replaces every dirty segment, including deletion of empty
segments. Rejected updates must return an empty dirty list; their statuses are
reported and prior meshes remain. There is no history replay/retry or hidden cap
increase. Check the status list before treating a final mesh as a complete replay.

## Output and metric definitions

`results.json` records fixture/source/GLM/compiler/adapter hashes, flags, exact
config, per-frame point counts/statuses/dirty counts, timings and Linux process
peak RSS. Each repeat runs a fresh process/context. Repeats must have identical
ordered mesh SHA-256, config, geometry diagnostics, residuals, statuses, point
counts and dirty counts; otherwise the run fails.

`ordered_mesh_N.bin` is an analysis artifact, not a Dataset `.bin` preview or model
export. On the required little-endian IEEE host it contains `RecordedMeshV1\0`, a
uint32 segment count, then lexicographically ordered chunk keys with timestamp,
counts/capacities/attribute flags and the original ordered vertex, normal, optional
color and face arrays. Pointer addresses and undefined struct padding are excluded.
The digest detects exact output changes, not geometrically equivalent remeshing.
Cross-toolchain floating-point differences may change it.

Diagnostics include:

- Segment/vertex/face counts, exact-position welded vertex count, bounds, area,
  finite data and valid face indices/capacities.
- Exact degenerates and separately faces with area ≤ `1e-12 m²`.
- Zero/nonunit normals and vertices outside their segment bounds.
- Edge incidence after **exact float-position welding** (signed zero normalized):
  boundary edges, nonmanifold edges, inconsistent two-face winding, paired
  cross-chunk edges and unpaired edges on chunk planes.

An unpaired edge on a chunk plane is a **seam candidate**, not proof of a crack:
observed surfaces can legitimately end there or contain holes. These diagnostics
do not certify watertightness. The harness does not weld or modify output geometry.
The seam/winding/degenerate diagnostics have explicit two-segment unit fixtures,
including the float-inexact 0.64 m boundary.

### Residuals are NOT ground-truth accuracy

At most 4096 input observations are selected deterministically, evenly by index
within each frame's depth-eligible points. Each is transformed by its replay pose
and queried against the **final reconstructed triangle surface** using a BVH.
Mean/RMS/upper-median/p95/max distances and counts within one/two voxels are
reported, separately for all selected inputs, accepted frames and rejected frames.
Median is sorted index `n/2`; p95 is sorted index `floor(.95*(n-1))`.

This is one-sided, sampled **input-to-surface self-consistency** on observations
also used to build the model. It is not independent sensor accuracy, ground-truth
surface error, Hausdorff distance, pose accuracy or proof against fabricated extra
surfaces. Filtering, selection bias and overfitting can affect it. Empty meshes
have unavailable residuals. No legacy Tango execution/comparison was performed.

## Bounds and timing scope

- At most 1000 committed frames, 1,000,000 points per file, 512 MiB aggregate raw
  fixture data; state ≤4 KiB and each pose file ≤16 KiB.
- Analysis cache ≤2,000,000 vertices and 1,000,000 faces. Exceeding this fails the
  benchmark instead of silently dropping geometry. Samples are capped at 4096.
- Replay streams one input cloud at a time. Core retains its existing resource
  bounds. The core context is destroyed before topology/BVH analysis.
- `read_validate_ms`: Dataset reads, pose conversion, input validation/selection.
- `update_ms`: the core update call.
- `extract_validate_replace_ms`: extraction, output validation and cache replacement.
- `replay_wall_ms`: setup and complete replay, excluding final analysis.
- `analysis_ms`: ordered output writing, geometry/topology analysis and residuals.
- RSS is the Linux child process high-water resident set, not isolated core heap
  usage. Sanitizer RSS/timing is instrumentation-dependent.

These are host timings, not phone FPS or active-capture persistence performance.
No storage durability or production fsync policy is changed.

## Verified synthetic fallback baseline (2026-09-30)

Frozen **synthetic-validation-only** fixture:
`/tmp/opencode/recorded-synthetic-validation-20260930`

- 6 frames, 48×48 state, 13,824 XYZC points.
- 13 raw files (state + six point/pose pairs), 223,866 bytes, plus manifest.
- Fixture SHA-256:
  `25dd59f4de59a6a8c76369d87b55e09b72980b9c55b0bfe6bfb38236487fd787`
- Core SHA-256 (unchanged):
  `97a16e50220b76c3baf124071f9f2b6e323814d5208a43f83f8837f2eb093e71`
- Ordered mesh SHA-256, equal in both optimized repeats and both sanitizer repeats:
  `3762ac1cc80e96403f896b0ea340df7f8a5160edae13974c8cf0771ea59ee358`

All six frames were accepted. Final output: **24 segments, 16,609 vertices,
30,299 faces**, 15,647 exact-welded positions, finite vertices/normals and valid
indices, zero exact degenerates/nonmanifold edges/inconsistent two-face winding,
and two near-degenerate faces at the stated area threshold. There were 930 paired
cross-chunk edges and 28 unpaired chunk-plane edge candidates, among 1001 boundary
edges. Bounds are recorded locally in the result JSON.

For 4092 sampled inputs, self-consistency RMS was **4.167 mm**, median **0.660 mm**,
p95 **6.652 mm**, maximum **39.010 mm**. These are not ground-truth accuracy claims.

Final optimized host runs (GCC 11.4, WSL2 x86-64): replay wall **201.55 / 206.07 ms**
for the six frames; final analysis **28.98 / 33.52 ms**; peak RSS **17,868 / 18,636 KiB**.
Per-frame timings and all source hashes are in:

- `/tmp/opencode/recorded-synthetic-final-20260930/results.json`
- `/tmp/opencode/recorded-synthetic-sanitized-20260930/results.json`

ASan/UBSan/leak-check replay passed with no sanitizer exclusions. The new C++
harness separately compiled with `-Wall -Wextra -Werror`. The complete build emits
existing ignored-`fscanf` warnings in unused Dataset distortion/yaw methods; those
production methods were not edited or exercised.

Validation tests rejected 14 malformed/incomplete/changed-input cases. Fake-ADB
tests passed for missing state, changing capture, incomplete committed inventory,
mid-pull mutation, and a stable committed prefix with ignored leftover files.
The fake tests explicitly do not establish that real-device acquisition succeeded.

## Commands

```sh
# Validate an acquired snapshot without contacting the device.
python3 tests/reconstruction/recorded.py validate --fixture /tmp/opencode/pixel-geometry-new-snapshot

# Replay a real snapshot once it is available; use a fresh output directory.
python3 tests/reconstruction/recorded.py run \
  --fixture /tmp/opencode/pixel-geometry-new-snapshot \
  --output /tmp/opencode/pixel-geometry-baseline --repeats 2 --resolution 0.04

# Explicit synthetic validation, never a silent fallback.
python3 tests/reconstruction/recorded.py synthetic --output /tmp/opencode/new-synthetic-fixture
python3 tests/reconstruction/recorded.py run \
  --fixture /tmp/opencode/new-synthetic-fixture \
  --output /tmp/opencode/new-synthetic-results --repeats 2 --sanitize

python3 tests/reconstruction/recorded.py self-test
python3 tests/reconstruction/recorded_acquisition_test.py
```

Keep captured input immutable. Reuse the exact fixture/config for future engine
comparisons; source and output digests identify each result. Main owns any new
recording, production persistence changes, full NDK build and device profiling.
