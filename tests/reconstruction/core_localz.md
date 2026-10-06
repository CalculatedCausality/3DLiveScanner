# Bounded local-Z memoization experiment — not retained

## Decision

Two closely related per-update caches were tested. Neither demonstrated a
meaningful, consistent host timing improvement, so **both were removed** and
`reconstruction/core.cc` was restored byte-for-byte to its pre-edit state.
There is no new production engine change from this experiment.

Exact final/core-before SHA-256:

```text
97a16e50220b76c3baf124071f9f2b6e323814d5208a43f83f8837f2eb093e71
```

The current source was captured before editing at
`/tmp/opencode/core-before-localz-20260930/core.cc`. The verified restored copy
and manifest are at `/tmp/opencode/core-localz-final-restored-20260930/`.
The earlier chunk/halo optimizations and resource diagnostics remain intact.

## Candidates

Both candidates stored the exact result of:

```cpp
camera.local(position(node) * resolution).z
```

in a 256-entry cache owned by the transaction. The pose and resolution are fixed
for the entire update. Each entry retained all three integer lattice coordinates
and a `double`; collisions replaced cached computations, never aliased voxels.
An out-of-range integer sentinel marked initially invalid entries. Neither
candidate expanded/reassociated the transform, used fast-math, dropped observations,
changed fusion or output order, altered work counters/caps, or added heap allocation.

1. **Hash256:** multiply/XOR integer-coordinate hash, direct mapped.
   Snapshot: `/tmp/opencode/core-localz-hash256-20260930/core.cc`.
   SHA-256: `cce23c14af8f6f361c8f88753cd4e89b361f8afbbeae1b2ae6f70187ac5b8343`.
2. **Spatial256:** the same entry format, with a cheaper 8×4×8 toroidal bit index
   and full-coordinate tags.
   Snapshot: `/tmp/opencode/core-localz-spatial256-20260930/core.cc`.
   SHA-256: `9f5d968438b3256f314e773d0e1be8dad46ad0b474faca245a0a6be49a0f4d26`.

No third candidate or broader optimization search was attempted.

## Modest timing screen

The existing deterministic room/box/sphere fixture supplied **9,216 points per
capture**, changing camera poses, calibrated color, confidence variation and
depth noise. Each screen used 12 views, two alternating before/after repetitions,
and logical CPU 6. Host: i9-13905H, WSL2 x86-64, GCC 11.4.0, C++11 `-O2`.
Both candidates were compared against the freshly captured `97a16e...` source.

Median update call time, over 24 frame samples per implementation/workload:

| Candidate | Workload | Before | Candidate |
| --- | --- | ---: | ---: |
| Hash256 | 40 mm, clearing + color | 118.85 ms | 114.80 ms |
| Hash256 | 30 mm, no clearing/color | 23.83 ms | 24.14 ms |
| Spatial256 | 40 mm, clearing + color | 127.18 ms | 124.11 ms |
| Spatial256 | 30 mm, no clearing/color | 25.51 ms | 25.86 ms |

Median 12-view sequence sums of update + extraction call times:

| Candidate | Workload | Before | Candidate | Elapsed-time change |
| --- | --- | ---: | ---: | ---: |
| Hash256 | Clearing + color | 1700.63 ms | 1670.86 ms | −1.75% |
| Hash256 | No clearing/color | 651.18 ms | 670.23 ms | +2.93% |
| Spatial256 | Clearing + color | 1857.84 ms | 1821.47 ms | −1.96% |
| Spatial256 | No clearing/color | 724.02 ms | 724.09 ms | +0.01% |

This was a screen, not a statistically precise small-effect study. The small
clearing gains, slight no-clearing update regressions, and host variation do not
justify retaining the additional cache/branches. No full five-repeat performance
campaign or device benchmark was run for these rejected candidates. No phone FPS
improvement is asserted; these measurements exclude persistence and other app work.

Raw per-frame timings and serialized traces:

- `/tmp/opencode/core-localz-hash256-screen/results.json`
- `/tmp/opencode/core-localz-spatial256-screen/results.json`

## Correctness checks

- Each candidate matched **64,676,922 serialized bytes** in its normal-workload
  screen, including ordered geometry, normals, colors, faces, timestamps, optional
  attributes, capacities, dirty indices and status codes. Repeated output counts
  and statuses were invariant.
- Spatial256 additionally passed the seven-scenario ASan/UBSan differential suite
  with six views per scenario: **40,182,852 bytes matched exactly**, including
  resource-limit rejection, preserved committed meshes and subsequent valid input.
  Results: `/tmp/opencode/core-localz-spatial256-correctness/`.
- Both Spatial256 and the restored production core passed the full existing
  analytic/recovery suite under ASan/UBSan/leak detection: plane/cube/pose/color/
  noise, negative/positive seams, clearing, replay and invalid input.
- Allocation sweeps remained **9 mesh-init, 23 update, 101 extraction positions**.
  The multi-chunk early/mid/late allocation rollback test also passed.
- Resource-limit tests preserved nonempty committed meshes and timestamps, accepted
  later valid updates, and emitted four rate-limited messages for 32 failures
  across eight contexts.

The runner now accepts `--core-source PATH`, allowing frozen candidates to be
tested without placing them back into the working implementation.

## Memory and stack budget

Each experimental cache was **256 × 24 = 6,144 bytes** on the host, with no extra
heap or persistent-volume allocation. GCC `-O2 -fPIC -fstack-usage` reported:

| Function frame, excluding callees | Original/restored | Each candidate |
| --- | ---: | ---: |
| `Tango3DR_updateFromPointCloud` | 2,496 bytes | 8,640 bytes |
| `Extractor::chunk` | 39,664 bytes | 39,664 bytes |

Reports: `/tmp/opencode/core-localz-hash256.su`,
`/tmp/opencode/core-localz-spatial256.su`; original in
`/tmp/opencode/core-hotpath-prod.su`. These are host compiler frame sizes, not a
claim about target NDK stack layout. **Final production overhead from this pass:
zero bytes**, because neither cache remains.

## Reproduction

```sh
python3 tests/reconstruction/core_benchmark.py \
  --baseline /tmp/opencode/core-before-localz-20260930 \
  --candidate /tmp/opencode/core-localz-spatial256-20260930/core.cc \
  --output /tmp/opencode/core-localz-repeat \
  --repeats 2 --frames 12 --cases 0 1 --cpu 6

python3 tests/reconstruction/core_run.py \
  --core-source /tmp/opencode/core-localz-spatial256-20260930/core.cc

python3 tests/reconstruction/core_run.py
```

Use a fresh output directory and an allowed CPU (or omit `--cpu`). Frozen sources
and reports are retained in `/tmp/opencode`; they are experimental artifacts,
not production replacements. Main owns persistence/worker changes, the full NDK
build and any device integration/profiling.
