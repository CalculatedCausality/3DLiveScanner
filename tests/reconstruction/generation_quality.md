# Independent model-generation quality measurements

This suite tests actual depth-to-mesh generation through the public core API.
It complements recorded replay: generated scenes have known surfaces, whereas
point-to-mesh residuals on a real recording are only self-consistency measures.

## Fixtures and metrics

`generation_quality.cc` raycasts deterministic observations of nine scenes:

- Shifted flat and oblique planes, including a noisy oblique plane.
- Sphere and sub-voxel-shifted sphere.
- Axis-aligned and rotated cubes.
- A 7 cm-thick plate and separated thin panels with an explicit gap.

Tests run at 2 cm and 4 cm without changing point density between implementations.
Scene cameras, confidence, noise, input ordering and core configuration are fixed.
All input frames must integrate successfully. The core produces the actual mesh;
the fixture does not substitute a mesher, camera pose converter or voxel volume.

Metrics include generated triangles/vertices/payload, update and extraction-plus-
replacement time, area-weighted surface residual against the analytic shape,
independent true-surface-to-mesh coverage, face and interpolated shading-normal
angles, triangle shape/skinny area, exact degeneracy, nonmanifold/winding edges,
shared-position chunk-boundary normal disagreement and false gap-bridge faces.
The coverage checks matter: deleting difficult surfaces must not appear to improve
quality merely by lowering vertex error. Boundary counts alone do not establish
watertightness on finite/open observed surfaces. The seam-angle metric on sharp
objects includes genuine creases; smooth sphere/plane seams are the clearer test.

The noisy plane has deterministic ±5 mm range perturbations and occasional
lower-confidence ±35 mm outliers. Inputs are synthetic, not captured images.
The scene residuals do not establish a phone's sensor or tracking accuracy.

## Frozen references

Original paged engine:
`/tmp/opencode/meshing-before-20261001/core.cc`, SHA-256
`c185babd6be001dc1997475c7a93e228c63d7c7438b1a2c7cb0073ae87d8d0c6`.

Nine-scene baseline reports:
`/tmp/opencode/generation-quality-before-v2-{2cm,4cm}-20261001/results.json`.
The first conservative reducer was independently measured at
`/tmp/opencode/generation-quality-reducer-{2cm,4cm}-20261001/results.json`.
That comparison preserved coverage and topology while reducing extraction time
in eight of nine 2 cm scenes; the shifted plane was about 2.5% slower in that host
sample. Surface-RMS differences were sub-micrometre. This is not a claim that
sensor accuracy improved. The sphere still showed a roughly 13° p95 shared-chunk
normal difference, motivating the separate shading-normal investigation.

## Commands

```sh
python3 tests/reconstruction/generation_quality.py \
  --source /path/to/frozen/core.cc --resolution 0.02 --repeats 2 \
  --output /tmp/opencode/new-analytic-baseline
python3 tests/reconstruction/generation_quality.py --resolution 0.02 \
  --output /tmp/opencode/new-analytic-candidate
python3 tests/reconstruction/generation_report.py \
  /tmp/opencode/new-analytic-baseline/results.json \
  --after /tmp/opencode/new-analytic-candidate/results.json --brief
```

Use `--sanitize` for ASan/UBSan/leak checking. Geometry/quality results must be
repeat-invariant. Input sources and private headers are hashed before/after.
Results/builds stay outside the repository.

`generation_quality_android.py` builds standalone ARM64 binaries and runs an
alternating before/after/after/before comparison on generated data. It never
installs/restarts the scanner or reads actual scans. Default mode uses these nine
analytic scenes. `--workload-case 0` selects the existing 9,216-point moving-view
workload with clearing/color; `--paged` exercises the real paged access path.
`--compare-output` hashes complete generated mesh dumps for experiments expected
to be byte-identical. All remote binaries/dumps are removed after testing.

These are native generator measurements, not camera/render/save FPS. A declared
CPU pin does not change governor settings; otherwise Android schedules normally.

## Fusion experiments

`generation_fusion_experiment.py` generates isolated candidates from frozen
sources; it does not edit production. Per-ray batching preserves every original
weighted update in order for each voxel, while grouping repeated accesses.
Its host comparisons matched complete mesh bytes and passed analytic, allocation,
rollback, resource and paging-fault suites. The Pixel results determined scope:

| Variant/path | Pixel mean update time | Observation |
| --- | --- | --- |
| Ray batching, RAM-only | 60.41 → 62.75 ms | Regressed; do not enable this path |
| Ray batching, paged | 86.12 → 80.42 ms | 6.6% lower in generated workload |
| Axial-dot transform, RAM-only | 60.51 → 60.34 ms | Inconclusive; do not retain |

The paged batching sequence total was about 5.3% lower. Complete target output
hashes also matched in a separate dump-based comparison. Raw reports are under
`/tmp/opencode/fusion-ray-batch-*-pixel-20261001` and
`/tmp/opencode/fusion-ray-batch-paged-pixel-bytes-20261001`.
Host gains alone were not used to justify a Pixel optimization.
