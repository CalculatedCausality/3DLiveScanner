# Core hot-path follow-up: frozen-source correctness and timing

This follow-up changes only `reconstruction/core.cc` and `tests/reconstruction/core*`.
The modern wrapper's resource-error classification/UI and device profiling are
separate integration work. All production resolution, memory, work and chunk limits
remain unchanged. No geometry eviction, fusion redesign, ABI layout change,
build/toolchain adjustment, vendor implementation, or ELF patch is involved.

## Exact baseline

Before editing the core, `core_benchmark.py --capture` preserved the then-current
source bytes (including the preceding float-seam fix), public ABI header, and core
test/runner sources. It did not use an older Git revision as the baseline.

- Frozen before: `/tmp/opencode/core-before-hotpath-20260930/core.cc`
- Before SHA-256:
  `404284fdca6d96fc2f77b18a06160e15a7f579b8b235d7d248a02a2bd0602b04`
- Extraction-only intermediate: `/tmp/opencode/core-extraction-hotpath-20260930/core.cc`
- Final measured source: `/tmp/opencode/core-final-hotpath-20260930/core.cc`
- Final SHA-256:
  `97a16e50220b76c3baf124071f9f2b6e323814d5208a43f83f8837f2eb093e71`
- Shared C++ fixture SHA-256:
  `4c2ea8c1213b7b95176d1f697e557ec172514b4038abc597b62dfe07cf6021c4`

Manifests are beside the snapshots. The before/candidate builds use the same
fixture, compiler, flags, ABI header and repository GLM. The runner verifies the
baseline source/header digests before building and never overwrites capture or
output directories. Keep these local artifacts to reproduce this exact comparison.

## Implemented optimizations

1. **Empty owner chunk early return.** Every owned cell's minimum corner is in
   that chunk, so no cell can be observed when the owner is absent. This avoids
   scanning 4096 cells for conservative dirty-halo entries with no owner storage.
2. **Extraction neighborhood caching.** At most eight ordered-map lookups obtain
   the owner and positive neighbors. A fixed 17³ pointer halo applies the original
   minimum-weight condition once per node. Cell traversal reads cached pointers
   instead of doing up to 4096 × 8 map lookups per segment.
3. **Same-sign cell rejection.** A cell whose eight SDFs are all nonnegative or all
   negative cannot emit a zero crossing. Such cells skip six tetrahedron calls.
   The zero test remains `< 0`; zero-node welding and sign conventions are unchanged.
4. **Transaction-local chunk caching.** A 64-entry direct-mapped cache holds keys,
   pointers to stable `std::map` mapped values, and cloned/writable flags. It is
   shared by free-space support checks, reads, and writes. A chunk becomes writable
   only after the original clone, changed-set and dirty-set allocations succeed.
   Cache eviction falls back to the existing maps/sets. Nothing survives the call.
5. **Remove an unused surface-band read.** The old voxel was only consulted for
   free-space clearing; its map lookup is omitted inside the truncation band.

The actual field updates, ray-step phase, point/voxel processing order, work-counter
increments, cap checks/priority, weighted arithmetic, color projection, tetrahedron
order, vertex/face insertion, normal accumulation and component filtering remain
the same. No speculative sample dropping or coarser reconstruction is used.

The caches add no heap allocations or failure points. On this x86-64 host the
64-entry transaction cache is about 2 KiB and the extraction halo is 39,304 bytes.
GCC `-O2 -fPIC -fstack-usage` reported a 39,664-byte `Extractor::chunk` frame and a
2,496-byte `updateFromPointCloud` frame, excluding callees. These are fixed temporary
costs, not persistent volume growth; target compiler stack usage can differ.

## Workload and timing method

`core_benchmark.cc` raycasts **96 × 96 = 9,216 depth points per capture** from
translating/rotating camera poses into a scene containing a back wall, side walls,
floor/ceiling, a foreground box and sphere. It includes occlusion boundaries,
confidence variation, zero-confidence points, deterministic 1.5 mm depth noise,
independent color-camera translation, Brown calibration and padded RGB rows.
Every sequence starts with a small committed seed surface.

The performance run used 24 captures per sequence, five repetitions per binary,
and alternated before/after execution order. Measured children were pinned to
allowed logical CPU 6 on a WSL2 x86-64 host reporting Intel i9-13905H. Compiler:
GCC 11.4.0, C++11, `-O2 -g -Wall -Wextra -Werror`, without fast-math or sanitizers.

Timers cover the update API call and the sum of extraction API calls for each
frame. They exclude input generation, dump serialization/validation, returned-mesh
destruction and the final full-volume verification pass. This is **core-call host
timing**, not whole-app FPS, phone latency, real captured data, or Tango parity.

### Results (2026-09-30)

All frames in these two normal workloads were accepted. Medians/p95 are over
120 frame measurements per implementation per workload:

| Workload / operation | Before median | After median | Before p95 | After p95 |
| --- | ---: | ---: | ---: | ---: |
| 40 mm, clearing + color: update | 158.66 ms | 130.42 ms | 201.15 ms | 173.11 ms |
| 40 mm, clearing + color: extraction | 38.33 ms | 29.70 ms | 50.60 ms | 43.58 ms |
| 30 mm, no clearing/color, CW, conservative filtering: update | 40.67 ms | 25.78 ms | 51.61 ms | 32.67 ms |
| Same 30 mm case: extraction | 57.90 ms | 38.07 ms | 74.71 ms | 50.71 ms |

Median whole-sequence sum of update/extraction call times:

- Clearing/color: **4613.51 → 3789.08 ms**, **17.87% reduction / 1.218×**.
- No clearing/color: **2390.29 → 1553.05 ms**, **35.03% reduction / 1.539×**.

The clearing workload remains costly even after the improvement. These results
justify keeping the conservative caches, not a claim of adequate phone frame rate.
An earlier extraction-only intermediate also compared byte-identically, with
lower extraction times; the final pinned repeated comparison is the performance
result used above. Raw per-frame/per-run data and metadata are in
`/tmp/opencode/core-hotpath-final/results.json`; the intermediate is in
`/tmp/opencode/core-hotpath-extraction/results.json`.

## Differential correctness

Seven scenarios × 24 capture attempts were compared against the frozen source:

0. 40 mm resolution, clearing, calibrated color, ordinary component threshold.
1. 30 mm, clearing/color disabled, clockwise faces, `min_num_vertices=1000000`.
2. 20 mm and a **fixture-only** 160-chunk cap, to force a growing-volume failure.
3. Fixture-only 200,000-work cap.
4. Fixture-only four-changed-chunks-per-frame cap.
5. Oversized point count, rejected before reading the pointed-to buffer.
6. A nonfinite final input point, rejecting the complete frame.

**177,013,043 serialized bytes matched exactly.** Comparisons include status codes,
dirty index ordering, mesh timestamps, counts/capacities, optional-buffer presence,
ordered vertex/normal/color bytes, ordered face bytes, and the final accumulated
segments. They intentionally exclude pointer addresses and undefined struct padding.
On failures, previously accumulated meshes are extracted and compared, and a later
small valid update must succeed. Repetition checks also require identical statuses,
segment/face counts and serialized sizes across all five runs.

A second differential run compiled **both** versions with ASan/UBSan/leak detection,
using six views in all seven scenarios: **40,182,852 compared bytes matched** with
no sanitizer failures. Its output is `/tmp/opencode/core-hotpath-sanitized/`.

`python3 tests/reconstruction/core_run.py` also passed:

- Existing analytic planes/cube/pose/color/noise, seams, winding, clearing, replay
  and invalid-input tests, with their previously reported geometry errors unchanged.
- Every allocation position in the existing sweeps: 9 mesh initialization,
  23 transactional update and 101 extraction failures.
- A 25,921-point multi-chunk fixture forcing cache eviction, with injected failures
  at allocation positions 0, 150 and 300; old meshes remain byte-identical and a
  subsequent update succeeds.
- Each resource limit against a nonempty committed mesh, including unchanged
  timestamp/normals/colors and later success. The logging fixture produced four
  diagnostics for 32 rejections across eight contexts, verifying throttling also
  survives context replacement.
- Separate production `-O2 -fPIC -Wall -Wextra -Werror` compilation and inspection:
  exactly the same 21 Tango-prefixed API symbols, no test-only allocation hook.

These exact-byte results apply to the tested host builds. Main should validate
recorded datasets and the target NDK/device build as part of phone profiling.

## Named resource diagnostics and caller semantics

On Android, update resource failures log at warning level under
**`ScannerReconstruction`**, using the existing Android `liblog` dependency.
Hosts use stderr. Names are:

- `resource_limit=max_update_work`
- `resource_limit=max_chunks`
- `resource_limit=max_update_chunks`
- `resource_limit=max_points_per_frame` (fixed bound; not a new config key)

One observed fixture message:

```text
resource_limit=max_chunks requested=161 limit=160 work=1422533 touched_chunks=33 staged_chunks=160 committed_chunks=157 points=9216; frame rejected, volume unchanged
```

The attempted frame had staged three new chunks before hitting the limit; none
were published. The committed volume stayed at 157 chunks. Both versions accepted
the first two captures and rejected the same third and subsequent over-budget
captures. Smaller valid updates still succeeded in the same context.

Logs are throttled **per reason, process-wide, to one per five seconds**. A
contended log attempt is dropped instead of blocking. The guard uses fixed storage
and performs no reconstruction allocation. Throttling persists across context
destroy/create cycles. The first limit detected in the original check order is
reported; there is no new public ABI or diagnostic getter.

All four limits still return `TANGO_3DR_INSUFFICIENT_SPACE` (-2), with an empty dirty
output and unchanged volume/timestamp. This is a rejected frame, **not poisoned
reconstruction state**. The modern caller should keep its context and committed
preview rather than marking it for full-history replay. Replaying does not make
an over-budget frame fit. Allocation failures still return `TANGO_3DR_ERROR` with
rollback, and invalid input still returns `TANGO_3DR_INVALID` before mutation.

Default limits remain 1024 persistent chunks, 256 changed chunks per frame,
32,000,000 work visits and 1,000,000 input points. No limits were raised to make
the performance fixture pass; only named failure scenarios lower their own configs.

## Reproduction

```sh
# Existing exact snapshot; use a fresh output directory for each invocation.
python3 tests/reconstruction/core_benchmark.py \
  --baseline /tmp/opencode/core-before-hotpath-20260930 \
  --output /tmp/opencode/core-hotpath-repeat \
  --repeats 5 --frames 24 --cases 0 1 2 3 4 5 6 --cpu 6

python3 tests/reconstruction/core_benchmark.py \
  --verify-output /tmp/opencode/core-hotpath-final

python3 tests/reconstruction/core_run.py
```

Use an allowed CPU number on another host, or omit `--cpu`. Add `--sanitize
--repeats 1 --frames 6` for the sanitizer differential run. For a future optimization,
capture that change's own before source using `--capture` **before editing**;
capturing the current optimized file does not recreate this historical baseline.
