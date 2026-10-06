# Field-derived shading normals and paged ray batching (2026-10-01)

Follow-up: [`meshing_dirty_fastpath.md`](meshing_dirty_fastpath.md) documents
the interior-write guard, focused update timings, and Main's Pixel quality/time
tradeoff. The normal algorithm and paged fusion body below remain unchanged.

## Status and baseline

Both changes are enabled by default. The accepted bounded reducer itself is
unchanged. Before editing, the complete accepted `core.cc`, `meshing.h`,
`paging.h`, and `paging_store.h` were frozen in:

`/tmp/opencode/meshing-reducer-before-normals-20261001`

Baseline core SHA-256:
`d82f5e7120cb83ae5707c64e8ca4f1fdca9fe129005ac3c4f7e13db365442409`.

Production hashes:

| File | SHA-256 |
| --- | --- |
| `core.cc` | `7e84d10204d991e28b16f22f816566693580ef53696ac5ac34ace346e2e71702` |
| `field_normals.h` | `546e5b7edf8cb166bdc222fe603aa9a3f74fa6fcefbc6ac32cb1c7d67ccbadd7` |
| `ray_fusion.h` | `a14e3d7a6332e78d690ae739bac08d0dfd222279009f9352b866142321ce6b53` |

## Normal definition and ownership

- Copy a 19³ halo of **SDF and weight only**. The 54,872-byte sample array is
  stack-local; no persistent voxel data, output attribute, or scratch heap
  allocation is added.
- Copy at most 20 relevant chunks (eight positive geometry chunks plus twelve
  negative-face stencil chunks) with **one pager pin at a time**, before taking
  the existing eight geometry pins. Double-negative source chunks cannot supply
  an axial difference. Copying the positive eight last warms the geometry halo.
- At a world lattice node, use centered differences when both neighbors meet
  `min_voxel_weight`, otherwise a one-sided difference when one is observed.
  Missing axis evidence or a zero/stationary interpolated gradient explicitly
  falls back to the existing oriented geometric accumulation.
- Interpolate unnormalized gradients on the canonical edge, then normalize on
  export. Exact-zero vertices use their lattice node alone, independently of the
  first incident edge requesting the welded vertex.
- The direction is increasing TSDF (outward). Clockwise winding still changes
  face order only. Gradient normals are never blended with segment-local face
  normal sums.

The ordinary field path is independent of which chunk requests the vertex. The
undefined-gradient fallback can retain geometric-normal seams; it is explicit,
and was not needed for any matched seam normal in the frozen recording.

### Dirty invalidation

The original eight negative-neighbor dirty segments remain. A write at **local
coordinate 15** can additionally affect a positive neighbor's node-zero
gradient. In the other axes, a preceding owner is needed only when the written
node's coordinate is zero. These specific positive-neighbor combinations are
marked, with a per-cache-slot bit mask suppressing repeated insertions.

An interior write adds no new segment; a simple face-boundary write produces
nine rather than eight dirty segments. A fully modified chunk can require at
most twenty owners, rather than blindly invalidating all 27. Allocation failures
still roll back the transaction before dirty publication or volume commit.

## Measured quality

Main's unchanged independent `generation_quality.cc/.py` was run for all nine
scenes at both resolutions, with two deterministic repeats. Positions/coverage,
counts, surface errors, face-normal errors, triangle quality, skinny area,
topology, and gap-bridge counts are unchanged.

Area-weighted shading angular error against analytic truth, degrees:

| Scene | 2 cm before → after | 4 cm before → after |
| --- | ---: | ---: |
| Shifted plane | 0.2311 → 0.1695 | **0.4169 → 0.4295** |
| Oblique plane | 0.7085 → 0.6364 | 1.4620 → 1.4269 |
| Noisy oblique plane | 1.4073 → 1.0270 | 1.5282 → 1.4592 |
| Sphere | 2.9086 → 2.1972 | 3.1458 → 2.5049 |
| Shifted sphere | 3.0229 → 2.2437 | 3.1704 → 2.4949 |
| Cube | 9.3330 → 9.0950 | 16.7050 → 14.9330 |
| Rotated cube | 7.6409 → 7.4884 | 13.1568 → 13.0459 |
| Thin plate | 10.8107 → 8.9572 | 16.0252 → 16.0042 |
| Separated thin panels | **11.7547 → 12.1412** | **21.5185 → 22.0315** |

Fifteen of eighteen angular means improve. The three bold entries regress;
this is not a claim that all shading/detail metrics improve. Geometry is
unchanged even on sharp/thin cases. No boundary averaging or geometry smoothing
is used.

At 2 cm, the integrated sphere's seam max/p95 falls from **38.128° / 13.291°**
to **0.0233° / 0.0182°** in Main's metric. That metric applies `acos` directly to
float-normal dot products: its residual is the float unit-length precision
floor. The private tests compare the exported float vectors directly and find
**zero nonidentical shared-position normals**.

Direct sampled-SDF sphere vertex-normal errors independently improve:

| Resolution | Mean before → after | Max before → after |
| --- | ---: | ---: |
| 2 cm | 1.538845° → 0.058787° | 4.526749° → 0.192075° |
| 4 cm | 3.207724° → 0.233590° | 9.006507° → 0.765745° |

### Frozen real recording

The cached identical volumes from the 451-frame recording were extracted with
both sources. **All positions, faces, colors and segment metadata are
byte-identical** in 1,641 segments at 2 cm and 414 at 4 cm. Only normals change.
Surface residuals, boundaries, degeneracy and winding counts are identical.

| Resolution | Matched seam positions | Nonidentical normal pairs before → after | Max seam angle before → after | p95 before → after |
| --- | ---: | ---: | ---: | ---: |
| 2 cm | 85,347 | 85,347 → **0** | 174.643927° → < 0.000002° | 57.630989° → < 0.000002° |
| 4 cm | 18,868 | 18,868 → **0** | 161.154301° → < 0.000002° | 46.761873° → < 0.000002° |

Real-recording angles are continuity measurements, not physical ground-truth
accuracy. Accepted/rejected frames remain the cached baseline's 443/8 at 2 cm
and 451/0 at 4 cm; existing benchmark limits are unchanged.

## Costs and tradeoffs

Five-run median final-volume **RAM extraction CPU time** on the same frozen
voxels, excluding fusion and analysis:

| Resolution | Accepted reducer → field normals | Change |
| --- | ---: | ---: |
| 2 cm | 305.117 → 345.839 ms | +13.3% |
| 4 cm | 66.232 → 82.435 ms | +24.5% |

Corresponding wall medians: 279.703 → 317.015 ms and 60.711 → 75.562 ms.
Small analytic-scene timings were noticeably noisy under host contention; these
larger cached-volume measurements are the primary extraction-cost comparison.

The deliberate minimum-budget stress fixture (27 chunks, eight resident) shows
the I/O cost clearly:

- Cold extraction reads: **61 → 178** chunks; both perform eight initial writes.
- Nine-repeat warm RAM extraction CPU median: **3.243 → 3.549 ms**.
- Nine-repeat warm paged extraction CPU median: **9.386 → 21.472 ms**.
- Peak resident payload remains exactly **786,432 bytes**, the eight-chunk cap.

Thus the normal halo can be substantially more expensive when the cache is
limited to eight chunks. It is a visible-quality improvement with a measured
CPU/I/O cost, not a claim of another extraction speedup or a combined Pixel FPS
improvement.

Optimized host stack-usage measurements:

- `Extractor::chunk`: **39,728 → 94,640 bytes** (+54,912).
- `FieldNormals` constructor: 208-byte additional call frame.
- `reducedCell`: 3,344 bytes, unchanged.
- Original `integrateDirect`: 368 bytes; paged batching wrapper/body: 1,744 bytes.
- Private `Vertex` remains 80 bytes; the normal-source boolean uses existing
  padding. The normal scratch adds no owning pointers or persistent heap data.

## Paged-only fusion integration

`ray_fusion.h` contains Main's validated per-ray batching body, unchanged except
for the required early RAM dispatch to `integrateDirect`. The original fusion
body is retained under that name. A test extracts and compares the complete
batched function body against `generation_fusion.cc.in`, ignoring only the RAM
dispatch line. No axial-dot change is included.

The current combined implementation passes full RAM/paged mesh-byte equality,
including normals and colors. Main's earlier Pixel batching measurements are
not presented as measurements of the new combined normal/extraction pipeline;
no device operation was performed in this work.

## Verification and reproduction

Passed:

- Existing unchanged `core_run.py`: analytic/color/noise, outward/clockwise,
  incremental dirty-cache/full-rebuild equality, resource limits and allocation
  rollback checks.
- Existing unchanged sanitized `paging_run.py`: RAM/paged bytes, minimum-budget
  pins, disk/OOM faults, rollback/retry and 1,100 logical chunks with eight
  resident.
- Sanitized `meshing_run.py`: the accepted reducer's geometry/color tests.
- New `meshing_normals.py`, optimized and ASan/UBSan/leak-checked: observed and
  one-sided stencils, missing/stationary fallback, exact-zero normals, all 4,096
  local node dirty dependencies, positive and negative-coordinate opposite-chunk
  normal refresh without geometry changes, shared-position normal bytes, pager
  equivalence, 53 allocation failure positions and an injected normal-halo read
  failure with all pins released.
- Production `-O2 -Wall -Wextra -Werror` compilation and `-fstack-usage` report.

No existing `core_test.cc` or paging assertions were changed or require changes.

```sh
python3 tests/reconstruction/meshing_normals.py --sanitize
python3 tests/reconstruction/meshing_normals.py --before \
  --source /tmp/opencode/meshing-reducer-before-normals-20261001/core.cc
python3 tests/reconstruction/meshing_quality_compare.py \
  /tmp/opencode/meshing-normal-paired-before-2cm \
  /tmp/opencode/meshing-normal-paired-after-2cm
python3 tests/reconstruction/meshing_geometry_compare.py \
  /tmp/opencode/meshing-normal-real-before-2cm/ordered_mesh.bin \
  /tmp/opencode/meshing-normal-real-after-2cm/ordered_mesh.bin
```

Independent analytic reports: `/tmp/opencode/meshing-normal-paired-{before,after}-{2,4}cm/results.json`.
Real reports: `/tmp/opencode/meshing-normal-real-{before,after}-{2,4}cm/results.json`.
The real reports include source hashes, identical cache hashes, seam-vector
identity counts, CPU/wall timings and complete geometry diagnostics.

`RECONSTRUCTION_LEGACY_MESH` selects legacy topology/edge indexing in current
source; current-source shading is still field-derived. Use the frozen accepted
source above for an exact before-normals comparison. Main owns the independent
generation-quality sources, build integration and Pixel accelerator documents.
