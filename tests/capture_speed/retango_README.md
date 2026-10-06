# Retango lazy projection regression and host benchmark

`retango_run.py` compiles the **actual production Retango implementation** twice:
the captured pre-edit working-tree version and the current version. It never reads
`git HEAD`, which predates the geometry fixes relevant to this optimization.
The same harness, GLM, Delaunay and geometry validation headers are used for both.
The only host substitution is an owned-pixel `Image` constructor/destructor;
OpenGL uses the existing `tests/geometry/include/GL/gl.h` declaration shim.

```sh
python3 tests/capture_speed/retango_run.py \
  --baseline /tmp/opencode/retango-lazy-baseline-20260929 \
  --ndk /tmp/opencode/pixelshare-sdk/ndk/25.1.8937393
```

The baseline directory must contain `retango.cc` and `retango.h` saved **before**
the optimization. For this change their SHA-256 hashes are:

```text
ef606149c7c5fad05a1d2d9fca870fb359c38a06c495bea335950e574d3a47cf  retango.cc
e3c31d1505571204befbde8a88a3d72510c81ba223f8f377e28053fcacbfa0a2  retango.h
```

Use `--checks-only` to run differential/sanitizer/optional Android compilation
without collecting benchmark timings.

## Checks

- Byte-for-byte comparisons of public `GET()` merged points, camera-space
  `output` values consumed by Android `PCL()`, accepted input, estimated points,
  mask dimensions, and every `mask`/`finished` element at each checkpoint.
- Dense accepted frames with invalid samples mixed in, changing poses and image
  sizes. The optimized cache must stay empty without `UPD`.
- Sparse Delaunay and pair estimates must actually generate points, which become
  public on a subsequent `ADD`. Optional wall estimation is exercised too.
- Different `ADD` and `UPD` poses, repeated `UPD`, and replacement of both built
  and still-pending projection caches. Materialized projections are compared
  exactly against the baseline and checked against the original `ADD` pose.
- Hole-filling's production `AddVoxel` kernel before any `UPD`, duplicate
  suppression, component wall additions, image-size changes and next-frame merge.
  The public component-hole path is also compared. Its existing flatness loop
  rejects the square-hole fixture (its first cross product is zero), so the
  direct insertion-kernel check supplies positive hole-cache coverage.
- Empty/malformed input, malformed/singular pose, null/invalid image, invalid
  first frame, and recovery with pending estimates, including an intervening
  `UPD`. Invalid frames keep their existing estimate/mask behavior.
- ASan/UBSan for both implementations, plus optional Android arm64 compilation of
  the production translation unit using NDK 25.1. This is not an APK build.

The host build intentionally leaves assertions enabled, including for `-O3`.
No fast-math flags are used. The optimized and baseline benchmark binaries have
identical compiler optimization flags; `RETANGO_LAZY` only enables test-side
assertions about the new private cache fields.

## Results

The runner prints a unique results directory under `/tmp/opencode`, or accepts
`--output PATH`. That directory retains:

- `results.json`: compiler/version, host, source hashes, differential hashes and
  sizes, every timing sample, and per-workload medians.
- `commands.log`: exact compiler/test commands and their output.
- `before/tango/` and `after/tango/`: the exact production source snapshots.
- Optimized and sanitized binaries and their compared `.bin` state streams.
- `retango-arm64.o` when `--ndk` is supplied.

The benchmark warms each workload eight times, then measures repeated `ADD`
(dense) or `ADD` + `UPD` (sparse) with unchanged inputs. Dense workloads use
19,200/76,800 input points and 640×480/1920×1080 masks. Seven independent process
runs per version alternate execution order; `--rounds` changes that count. The
default `--benchmark-scale 10` times 2,500/1,000/1,000/30,000 iterations respectively
to avoid relying on millisecond-long samples. Both wall-clock and process-CPU
times are recorded; the latter excludes time this single-threaded harness is
descheduled on a busy host.
Times are **host microseconds per call**, not device FPS or end-to-end capture
latency. Allocation warm-up is excluded; the normal accepted-point validation,
output creation and mask clearing remain inside the timed calls.

## Recorded verification — 29 September 2026

Host: WSL2 x86-64, Intel Core i9-13905H, GCC 11.4.0, identical `-O3` builds.
Both implementations passed 68 checkpoints with **44,113,680 bytes of identical
state**: merged/output/input/estimated points, materialized projections, and
mask state. ASan/UBSan passed for both; the optimized Android arm64 translation
unit compiled with NDK 25.1.

Seven longer-loop runs per version produced these medians (microseconds/call):

| Workload | Wall before → after | CPU before → after | CPU reduction |
| --- | ---: | ---: | ---: |
| Dense ADD, 19,200 points, 640×480 masks | 301.280 → 185.802 | 260.139 → 178.770 | 31.28% |
| Dense ADD, 76,800 points, 640×480 masks | 1,134.702 → 707.223 | 1,042.865 → 704.660 | 32.43% |
| Dense ADD, 76,800 points, 1920×1080 masks | 1,434.643 → 963.738 | 1,255.877 → 889.859 | 29.14% |
| Sparse ADD + UPD, 25 points | 42.593 → 37.362 | 39.215 → 36.850 | 6.03% |

Artifacts:

- Pre-edit sources: `/tmp/opencode/retango-lazy-baseline-20260929/`.
- Sanitizers, Android object, and initial optimized differential:
  `/tmp/opencode/retango-lazy-results-20260929/`.
- Longer-loop timing samples and repeated optimized differential:
  `/tmp/opencode/retango-lazy-results-20260929-long/`.

Each results directory contains `results.json` and `commands.log`. The initial
short timing run was noisy (including an apparent sparse slowdown); its samples
are retained for transparency. The table uses the subsequent longer-loop run,
which also records CPU time. The longer run skipped already-passed sanitizer and
Android checks; both directories contain the same production-source hashes.

### Follow-up: skip disabled component-wall preparation

`ADD(components, pose, false)` now skips both boundary scans and population of
the two `top` vectors. The enabled estimator retains its original iteration
order; sorting, copying, camera inversion and hole insertion stay in place.

The incremental comparison against the captured post-lazy-cache source passed
all 68 checkpoints in optimized and ASan/UBSan builds, with the same state hash
and byte count recorded above. This includes component calls with estimators
disabled and enabled, sparse updates, and positive hole-insertion/deduplication
coverage. NDK 25.1 arm64 compilation passed too.

Results: `/tmp/opencode/retango-wall-guard-results-20260929/` (`results.json`,
`commands.log`, exact before/after sources, binaries and Android object).
The earlier timing table predates this small follow-up; no additional speed
claim is made for the component guard.

```sh
python3 tests/capture_speed/retango_run.py \
  --baseline /tmp/opencode/retango-lazy-results-20260929-long/after/tango \
  --checks-only \
  --ndk /tmp/opencode/pixelshare-sdk/ndk/25.1.8937393
```
