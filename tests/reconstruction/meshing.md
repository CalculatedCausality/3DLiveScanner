# Bounded extraction remeshing (2026-10-01)

Follow-up: [`meshing_normals.md`](meshing_normals.md) records the field-normal
quality/cost comparison and paged-only ray-batching integration. The geometry
reducer and measurements below are the accepted pre-normal-change baseline.

## Shipped algorithm

The production default is **conservative cell-local reduction of the existing
Freudenthal surface**, plus direct indexing of shared lattice-edge vertices.
`RECONSTRUCTION_LEGACY_MESH` retains the old extractor for compile-time A/B tests.
There is no new public setting or runtime switch.

The initial cube-edge/asymptotic-decider prototype passed the public core suite,
but randomized scalar-cell tests found inconsistent winding in its warped-saddle
Steiner fallback. That prototype and its fallback are **not shipped**. No Poisson
lookup tables, external dependencies, or third-party meshing code were copied.

For a cell with a strictly crossing body diagonal:

1. Form its original tetrahedral patch in bounded local arrays (19 possible
   intersections, at most 12 triangles).
2. Require one disk with exactly one removable interior vertex, and a simple
   projected boundary. Reject folded/multiple-loop/singular candidates.
3. Require all vertices to lie in a slab no wider than **0.01 voxel**. Both the
   original patch and a candidate boundary-vertex fan must be consistently
   oriented graphs over the same projected polygon. Every point of either graph
   has a counterpart on the other graph along the slab normal: this bounds their
   symmetric Hausdorff separation by the slab width.
4. Retain every boundary vertex and segment. Forbid new diagonals on any shared
   cell face. Check candidate triangles in actual public-float world coordinates
   against the same degeneracy threshold as `triangle()`.
5. Select the admissible fan with the best summed triangle shape score. For
   colored output, also require an affine projected RGB fit whose residual range
   is at most **one exported RGB unit per channel** over all old vertices. This
   bounds the change of interpolated color at corresponding projected points;
   high-contrast color-only features retain their original triangulation.
6. Otherwise emit the original tetrahedral patch. Emit only the selected
   vertices/faces.

The bound is **0.2 mm at 2 cm**, **0.4 mm at 4 cm**, relative to the previous
piecewise-linear surface, not a claim about real-world scan accuracy. Boundary
positions and interpolated colors do not move. Normals are recomputed from the
emitted oriented triangles. Removed interior vertices retain a unit of
component-filter evidence on a surviving vertex, preserving `min_num_vertices`
accept/reject decisions instead of deleting components because of remeshing.

The fixed vertex index covers the seven positive Freudenthal edge directions and
exact-zero nodes in the 17³ halo. It replaces per-vertex tree allocation/search;
it does not change voxel storage or paging. Its lazy allocation is 157,216 bytes
per nonempty extraction. `Vertex` remains 80 bytes on the tested host (the support
counter occupies former padding). Local triangulation scratch is bounded and
stack-only. The existing eight chunk pins remain alive throughout extraction.
The final optimized host stack-usage report gives 39,728 bytes for `chunk()`
(primarily the existing halo), 3,344 for `reducedCell()`, and 272 for the public
extraction caller. Vertex lookup uses a further 288-byte frame. There is no
recursive polygon triangulation or unbounded scratch allocation.

## Frozen baseline and artifacts

Baseline snapshot: `/tmp/opencode/meshing-before-20261001/` contains complete
`core.cc`, `paging.h`, and `paging_store.h` files.

Baseline core SHA-256:
`c185babd6be001dc1997475c7a93e228c63d7c7438b1a2c7cb0073ae87d8d0c6`.

Final measured core SHA-256:
`d82f5e7120cb83ae5707c64e8ca4f1fdca9fe129005ac3c4f7e13db365442409`.

Final meshing header SHA-256:
`9e56fa0023701ae689cc0278a0be4fedb052717257fd91c4f2f6441eca558344`.

`meshing_recorded.py` validates the frozen input, compiles the actual Dataset
reader and production pose-conversion body, and caches the fused volume in a
test-only raw file. A/B extraction reads **identical, SHA-256-matched voxels**;
fusion is not repeated during extraction experiments. Cache metadata binds the
fixture, resolution, and benchmark chunk limit. No device access is involved.
The frozen baseline's resulting 2 cm and 4 cm ordered mesh hashes also match the
previous recorded/paging benchmark artifacts exactly.

## Extraction/output measurements

Host `g++ -O2`, five final-volume extractions; times below are median wall-clock
milliseconds. They include public output allocation/destruction and exclude
fusion, file I/O, topology analysis, and residual queries. Host contention causes
timing variability; these are not phone FPS or full-pipeline speedups.
Bytes are positions + normals + faces (color disabled in these replay fixtures).

| Fixture / voxel size | Extract ms before → after | Vertices before → after | Faces before → after | Output bytes before → after |
| --- | ---: | ---: | ---: | ---: |
| Frozen real 451 / 2 cm | 982.263 → 481.362 | 1,290,898 → 1,284,154 | 2,296,835 → 2,283,347 | 58,543,572 → 58,219,860 |
| Frozen real 451 / 4 cm | 174.897 → 86.187 | 286,238 → 284,154 | 512,345 → 508,177 | 13,017,852 → 12,917,820 |
| Synthetic six views / 2 cm | 22.447 → 11.572 | 30,247 → 28,211 | 53,634 → 49,562 | 1,369,536 → 1,271,808 |
| Synthetic six views / 4 cm | 12.887 → 8.124 | 16,609 → 13,754 | 30,299 → 24,589 | 762,204 → 625,164 |

The real scan is noisy: conservative triangle reductions are only **0.59% / 0.81%**,
while its extraction time drops **51% / 51%** in these samples. The 4 cm process
CPU-time medians independently improve **190.672 → 92.361 ms**. Synthetic-view
triangle reductions are **7.59% / 18.85%**. This is not a claim of a several-fold
real-mesh size reduction; shared-face tessellation is deliberately preserved.

Reports are `results.json` in:

- `/tmp/opencode/meshing-recorded-before-full-2cm`
- `/tmp/opencode/meshing-recorded-shipped-2cm`
- `/tmp/opencode/meshing-recorded-cpu-before-4cm`
- `/tmp/opencode/meshing-recorded-shipped-4cm`
- `/tmp/opencode/meshing-synthetic-before-{2,4}cm`
- `/tmp/opencode/meshing-synthetic-shipped-{2,4}cm`

### Geometry and limits

- Real 2 cm input-to-surface RMS: **44.177139731 → 44.177134779 mm**.
- Real 4 cm input-to-surface RMS: **48.740114636 → 48.740217376 mm**.
- Real median, p95, maximum, and within-one/two-voxel counts are unchanged at both
  resolutions. These residuals are input self-consistency, not ground truth.
- Both real outputs have zero exact-degenerate faces, nonmanifold edges,
  inconsistent-winding edges, and zero-length normals. Maximum unit-normal error
  is below `5.1e-8`. Near-degenerate faces: 2 cm **57 → 47**, 4 cm **6 → 6**.
- Exact chunk-local open-edge geometry, incidence, and orientation match the old
  output in **1,641 segments at 2 cm / 414 at 4 cm**. Global paired cross-chunk
  edges remain **81,805 / 18,131**; unpaired chunk-plane counts remain **7,161 /
  1,326**. Existing observation-boundary openings are not filled or discarded.
- 4 cm accepted **451/451** frames. The 2 cm comparison uses the existing recorded
  RAM benchmark's `max_chunks=4096`, `max_update_chunks=256`, and work budget
  32,000,000; it accepted **443/451**, matching the old benchmark's eight frame-cap
  rejections and its exact mesh digest. Production defaults/ranges and export
  limits are unchanged. The 4 cm mesh still exceeds the existing 500,000-face
  texturing limit; this work does not conceal or raise that limit.

### Direct known-surface tests

All closed sphere, cube, and thin-box tests retain incidence two, consistent
winding, and Euler characteristic two, including negative chunk coordinates.
Errors below use area-weighted triangle-interior quadrature, not just vertices.

| Shape | 2 cm faces before → after | 4 cm faces before → after | Error result |
| --- | ---: | ---: | --- |
| Shifted plane | 34,848 → 26,136 | 9,248 → 6,936 | Zero to printed precision |
| Shifted/rotated plane | 41,644 → 32,234 | 11,052 → 8,556 | RMS ≈ 0.000004 mm; max < 0.000025 mm |
| Sphere | 56,784 → 47,184 | 14,136 → 12,666 | Max improves 0.330270 → 0.305210 mm / 1.315832 → 1.207245 mm |
| Cube | 79,704 → 60,978 | 19,352 → 15,280 | RMS 0.721827 → 0.721664 mm / unchanged 2.061306 mm |
| 2.2-voxel-thick box | 28,848 → 22,604 | 7,688 → 6,280 | RMS unchanged 1.063414 / 2.903137 mm |

Sphere RMS increases slightly: **0.163067 → 0.164039 mm** at 2 cm and
**0.651687 → 0.652673 mm** at 4 cm (about one micrometre). Maximum error improves.
Sharp/thin features are retained by refusing reductions outside the bound.

## Verification and reproduction

```sh
python3 tests/reconstruction/meshing_run.py --sanitize
python3 tests/reconstruction/meshing_run.py --core-source /tmp/opencode/meshing-before-20261001/core.cc
python3 tests/reconstruction/core_run.py
python3 tests/reconstruction/paging_run.py --sanitize
```

The new private suite checks 10,160 randomized nonzero sign configurations, all
6,561 ternary negative/zero/positive configurations, and 2,000 jittered planes.
It obtains 1,486 reductions in the jittered cases with maximum sampled symmetric
deviation **0.009504503 voxel**, below the 0.01 bound. It checks exact boundary
incidence/orientation against tetrahedra, colors, all-eight-corner evidence over
eight chunks, component-filter thresholds, and a recorded negative-coordinate
float-sliver regression. Constant/affine color fields permit reduction, while a
high-contrast color-only feature forces fallback. Singular exact-zero scalar fields are compared against
their original singular topology, not incorrectly asserted to be manifolds.

ASan/UBSan/leak checks pass. The unchanged public core suite passes all winding,
color, analytic/noise, replay, rollback, and allocation sweeps. The unchanged
paging suite passes RAM/paged byte equality, eight-neighbor pins, read/write/OOM
faults and rollback, strict budgets, and 1,100 logical chunks with eight resident.
No `core_test.cc` assertion changes are needed.

Example cached real A/B (use fresh output/cache paths):

```sh
python3 tests/reconstruction/meshing_recorded.py \
  --fixture /tmp/opencode/recorded-pixel-451-geometry-20260930 \
  --core-source /tmp/opencode/meshing-before-20261001/core.cc \
  --resolution .04 --cache /tmp/opencode/new-meshing.volume \
  --output /tmp/opencode/new-meshing-before
python3 tests/reconstruction/meshing_recorded.py \
  --fixture /tmp/opencode/recorded-pixel-451-geometry-20260930 \
  --resolution .04 --cache /tmp/opencode/new-meshing.volume \
  --output /tmp/opencode/new-meshing-after
python3 tests/reconstruction/meshing_boundary.py \
  /tmp/opencode/new-meshing-before/ordered_mesh.bin \
  /tmp/opencode/new-meshing-after/ordered_mesh.bin
```

The replay harness also records per-extraction process CPU time, allowing
repeat comparisons on a busy host. All benchmark cache/mesh/result files stay
outside the repository. Main owns the independent `generation_quality*`
benchmarks and build/target integration.
