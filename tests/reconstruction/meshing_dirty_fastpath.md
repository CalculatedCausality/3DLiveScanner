# Selective normal invalidation: interior-write fast path

## Decision

Retain the **minimal guard-only optimization**. Both `writable()` paths call
`normalDirty()` only when at least one coordinate has floor-modulo-16 value 15.
Unsigned low bits implement that test for positive and negative coordinates.

The original `normalDirty()` body, insertion order, mask lifetime, allocation
timing for affected writes, and transaction commit sequence are unchanged. No
dirty owner is removed: without a coordinate at 15 the original helper did
nothing. No new state, heap allocation, I/O, deferred work or public switch is
introduced. Field normals and paged ray fusion are unchanged.

Broader mask-table and cold-helper variants were tested and discarded because
their full-update results were mixed. The retained patch has only two guards
and an explanatory comment in production code.

Frozen comparison source and its dependencies:
`/tmp/opencode/meshing-before-dirty-fastpath-20261001`.

- Before core SHA-256: `7e84d10204d991e28b16f22f816566693580ef53696ac5ac34ace346e2e71702`
- Guard-only core SHA-256: `548b87ef7a4cdeab4587f38ce96cbe2895c48eefbcf3cd77510a4089b1a88842`

## Focused host measurements

`meshing_dirty_benchmark.py` reuses the existing generated moving
room/box/sphere capture, but times **updates only**, with final mesh extraction
outside the timed region. It alternates before/after execution order over five
repeats, eight RGB frames per run, at 2/4 cm in RAM and 96 MiB paged contexts.
This is separate from Main's nine-scene and device benchmarks.

Median CPU time for the eight-update sequence:

| Case | Before → guard-only | Change |
| --- | ---: | ---: |
| 2 cm RAM | 935.297 → 914.875 ms | -2.18% |
| 2 cm paged | 1002.445 → 971.534 ms | -3.08% |
| 4 cm RAM | 917.775 → 909.311 ms | -0.92% |
| 4 cm paged | 977.697 → 987.799 ms | +1.03% |

The isolated warm RAM `writable()` probe (4,194,304 writes over all 4,096 node
positions of a negative-coordinate chunk) improves **8–14%** across the same
runs. The larger update gains are modest; there is no demonstrated universal
workload speedup or elimination of the Pixel update penalty.

Every before/after/repeat emits **identical complete dirty arrays, status data
and final meshes, including normals and colors**. RAM and paged outputs also
match. Output SHA-256:

- 2 cm: `1d73370a242d046efd12cffb76af92689a679011b7e5f2b5c329d61a271ea189`
- 4 cm: `3c694ce6a52c865a4c37d7014d69be576e8763fb5945d97facad644f3b5b314a`

Primary report:
`/tmp/opencode/meshing-dirty-guard-only-host-20261001/results.json`.

## Main's Pixel evidence and remaining tradeoff

Main's combined **pre-guard** Pixel 2 cm suite reported:

- Update: **1487.66 → 1733.16 ms** (+16.5%).
- Extraction: **640.93 → 449.54 ms**.
- Total: **2128.60 → 2182.69 ms** (+2.5%).
- Sphere seam p95: **13.54° → 0.02°**; mean shading error **2.92° → 2.20°**.
- Thin-panel mean shading error: **11.76° → 12.16°**, a regression to retain in
  the quality assessment.

These are Main's measurements from
`/tmp/opencode/generation-combined-pixel-2cm-20261001/results.json`, not a new
device run. The guard-only source still needs Main's target timing before
claiming any recovery of the Pixel overhead. No device was changed here.

Additional independent evidence supplied by Main, also for the combined
**pre-guard core `7e84d102…`**:

| Workload | Before → combined pre-guard |
| --- | ---: |
| Pixel paged colored 12-frame sequence | **1318.49 → 1195.61 ms (-9.3%)** |
| Pixel paged mean update | **88.88 → 85.78 ms** |
| Pixel 4 cm RAM suite, update | **1177.77 → 1375.71 ms** |
| Pixel 4 cm RAM suite, extraction | **173.62 → 129.32 ms** |
| Pixel 4 cm RAM suite, total | **1351.39 → 1505.03 ms (+11.4%)** |

The performance tradeoff is workload-dependent: the paged sequence improves,
while the 4 cm RAM suite regresses overall despite faster extraction. These
results do not measure the final guard-only patch.

Main's full real 501-frame, 2 cm paged comparison accepted **501/501** frames in
both variants, with **2,874 chunks** and **96 MiB peak resident payload** in both.
Faces decreased **2,862,083 → 2,846,879**. Exact open-edge geometry/counts matched
across all **1,972 segments**; input-to-surface RMS was **44.181931 → 44.181920 mm**,
with zero exact-degenerate faces, nonmanifold edges or winding errors. This
combined comparison includes the reducer's changed interior triangulation; it
is not a claim of whole-mesh byte identity.

Main also completed native JNI, modern/legacy compilation, sanitized nine-scene
tests, and the full APK/16 KiB/signature audit on **`7e84d102…`**. The final
**`548b87ef…`** source has the host and arm64 compilation checks below; its target
benchmark, APK rebuild and packaging audit remain Main's next step.

## Verification

- Unchanged core and sanitized paging suites pass, including dirty-cache/full
  extraction parity, byte-identical RAM/paged meshes, allocation rollback and
  read/write/checksum faults.
- Sanitized normal tests pass all 4,096 selective ownership cases and existing
  opposite-chunk normal refresh checks at positive and negative coordinates.
- New focused checks fail each of four corner-stencil insertions, verify only
  the successful mask prefix is recorded, retry to completion, then force a
  direct-cache collision and revisit. No owner is lost or incorrectly skipped.
- The NDK 28.2 arm64/API-24 compiler accepts the final core with
  `-std=c++11 -O2 -Wall -Wextra -Werror -fstack-usage`.
- No existing `core_test.cc` or paging-test assertions were changed.

```sh
python3 tests/reconstruction/meshing_dirty_benchmark.py \
  --before /tmp/opencode/meshing-before-dirty-fastpath-20261001/core.cc \
  --output /tmp/opencode/new-dirty-guard-comparison --repeats 5
python3 tests/reconstruction/meshing_normals.py --sanitize
python3 tests/reconstruction/core_run.py
python3 tests/reconstruction/paging_run.py --sanitize
```
