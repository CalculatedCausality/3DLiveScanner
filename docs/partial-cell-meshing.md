# Observed partial-cell meshing repair

## User result

The settings-only stability trial did **not** resolve the report: the user still
found the preview badly fragmented. After installing this mesher change and
trying a fresh scan, the user reported **“Clearly improved”**—surfaces materially
more continuous and usable. This confirms a visual improvement, not complete
hole closure or ground-truth measurement accuracy.

## Defect and change

The extractor previously discarded an entire cube if any of its eight lattice
corners was missing or below the existing observation-weight threshold. That
also discarded fully observed tetrahedra contained within partially observed
cubes.

The repaired extractor keeps the existing full-cell reduction path when all
eight corners are observed. For other cells it visits the same consistent
Freudenthal subdivision and emits a tetrahedron's surface only when **all four
of that tetrahedron's corners meet the existing observation requirement**.
It does not lower the weight threshold, invent missing depth, bridge arbitrary
holes or change fusion arithmetic. Shared lattice-edge indexing and normal
fallback behavior remain in use.

Core SHA-256:
`cb18ca86172b296b7dbae0f480f8c16c375b4a144fe705a108065012c3b513a3`.

## Same-input evidence

The frozen 484-frame / 678,594-point geometry recording was replayed with the
same 2 cm resolution, nine-vertex component filter and clearing disabled.
All frames were accepted in both versions.

| Metric | Previous mesher | Partial-cell repair |
| --- | ---: | ---: |
| Surface area | 13.155 m² | 14.495 m² |
| Faces | 274,989 | 302,509 |
| Unpaired edges on chunk planes | 2,050 | 1,364 |
| All open boundary edges | 33,339 | 33,973 |
| Sampled input residual RMS | 23.21 mm | 21.46 mm |
| Sampled input residual p95 | 46.92 mm | 43.08 mm |
| Degenerate / nonmanifold / inconsistent-winding faces or edges | 0 / 0 / 0 | 0 / 0 / 0 |

This recovers roughly 10% more surface and reduces unpaired chunk-plane edges
by about 33%. Total open-edge count does not fall; the result remains an open
scan with additional observed boundary regions. These geometry-only replay
measurements are self-consistency evidence, not physical accuracy or a
watertightness guarantee. Photographic color is not included in the replay.

## Verification and tradeoffs

- `tests/reconstruction/partial_cells_run.py`: a known four-node plane produces
  its expected triangle despite unrelated unknown cube corners; removing a
  required node, dropping it below threshold or removing the sign crossing
  produces no triangle. Includes negative/positive chunk boundaries, RAM and
  the eight-chunk minimum pager under ASan/UBSan.
- Core sanitizer suite: analytic geometry, color, confidence, clearing, seams,
  incremental/full extraction agreement, invalid inputs, resource rejection,
  allocation failure and rollback/retry all passed.
- Paging sanitizer suite: disk faults/corruption, pin lifetime, capacity,
  transactional rollback and 1,100 logical chunks in eight resident chunks passed.
- Nine analytic 2 cm scenes: no new gap bridges or nonmanifold edges; existing
  coverage was retained. Adding boundary geometry modestly increased some
  analytic errors (e.g. oblique-plane RMS 0.551 → 0.598 mm and separated-panel
  RMS 6.227 → 6.429 mm). Sphere results were unchanged. Sanitizer timings are not
  a performance benchmark.
- Modern build, static 16 KiB/dependency audit, zipalign and signature passed.

Frozen candidate: `/tmp/opencode/recon-partial-cells-20261001`.
Replay result: `/tmp/opencode/preview-holes-partial-20261001/results.json`.
Analytic reports: `/tmp/opencode/partial-cells-quality-{before,after}-20261001`.
Private diagnostic projection: `/tmp/opencode/scanner-build-verification/partial-cells-topdown.png`.

## Installed build

`artifacts/3DLiveScanner-modern-partial-mesh-debug-2026-10-01.apk`

SHA-256: `5b95a748b16c98d387ceec38c655bcd47561ad01bb3946610022573d7e8e29fa`.

Installed in place after the user confirmed the test scan was saved. Installed
identity and capture commit-state preservation were verified; startup reached
FileManager. The scoped crash buffer for PID 9667 was empty at inspection.

Current settings: requested-resolution preview, TPU correction off, free-space
clearing off, capture-first saving on. Capture-lock separation and efficient
voxel-pin reuse remain. Subsequent speed work must preserve this visual result.
