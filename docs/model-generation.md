# Model-generation improvements

## Final refinement status

The performance work unit is complete. The retained change is a minimal
interior-write guard with byte-identical dirty arrays and full mesh output; no
new Pixel speedup is claimed. Core SHA-256:
`548b87ef7a4cdeab4587f38ce96cbe2895c48eefbcf3cd77510a4089b1a88842`.
It is included in the installed export-repair APK `32d9a456…`, with independent
core/paging/sanitizer checks and nine-scene quality validation. See
[the handoff](../tests/reconstruction/meshing_dirty_fastpath.md) and
[the export repair](large-model-texturing.md).

## Previously installed candidate

At the user's request, the first combined candidate was installed in place on the
Pixel 9 Pro XL as `3DLiveScanner-modern-generation-debug-2026-10-01.apk`, SHA-256
`b739face1d44b808d6c529e3cacdd616dd9ccd9bff6db7ec42f0cb036b495458`.
Installed bytes matched the audited artifact, the existing 501-frame capture
commit state was unchanged, and startup reached `main.Main`. The additional
normal-update overhead refinement was separate from that build and is now
included in the export repair described above.

## Implementation

The modern engine combines three changes:

- **Faster mesh extraction:** direct indexing of shared lattice edges replaces
  repeated tree allocation/search. Conservative cell-local reduction removes an
  interior vertex only when boundary geometry, topology and color bounds permit.
  Maximum surface displacement relative to the old triangulation is 0.01 voxel
  (0.2 mm at a 2 cm setting). This is not a sensor-accuracy bound.
- **Consistent shading normals:** normals use the same observed TSDF gradient at
  a world-lattice location across mesh segments. Missing gradient evidence falls
  back to oriented triangle normals. The added neighbor dependencies participate
  in dirty-segment invalidation, including positive neighbors. The live renderer
  consumes these normals directly; texturing also preserves supplied normals.
- **Paged-path fusion batching:** repeated writes along one ray are grouped while
  retaining every voxel's update order, weights and colors. RAM-only integration
  retains the original arithmetic path because the isolated target experiment
  found batching slower there.

The gradient halo streams pages before pinning the geometry halo; the minimum
eight-resident-chunk configuration remains supported. See the private normal and
paging tests for allocation failure, cache failure and pin-release checks.

## Measurement scope

Comparisons use a frozen pre-generation core, SHA-256
`c185babd6be001dc1997475c7a93e228c63d7c7438b1a2c7cb0073ae87d8d0c6`.
Input observations, resolution, confidence and budgets are held fixed. Synthetic
scenes test known-surface error and coverage; real-recording residuals measure
input self-consistency only. Neither establishes physical phone-scan accuracy.

The first combined candidate, core SHA-256
`7e84d10204d991e28b16f22f816566693580ef53696ac5ac34ace346e2e71702`,
was measured on the Pixel 9 Pro XL with alternating before/after/after/before
native runs. These timings exclude camera acquisition, rendering and persistence.

| Workload | Before | Combined candidate | Interpretation |
| --- | ---: | ---: | --- |
| 2 cm nine-scene extraction total | 640.93 ms | 449.54 ms | About 30% lower extraction time |
| 2 cm nine-scene fusion + extraction | 2,128.60 ms | 2,182.69 ms | About 2.5% slower overall; update overhead needs attention |
| 4 cm nine-scene extraction total | 173.62 ms | 129.32 ms | About 26% lower extraction time |
| 4 cm nine-scene fusion + extraction | 1,351.39 ms | 1,505.03 ms | About 11.4% slower overall |
| Paged, colored 12-frame generated sequence | 1,318.49 ms | 1,195.61 ms | About 9.3% lower total time |

These mixed results motivated a separate update-overhead optimization. The user
requested installation of this tested candidate before that refinement finished.
Extraction improvements alone must not be presented as an
equivalent capture-FPS improvement. Timings are sums within each fixed repeat,
then medians, rather than averages of scene percentage gains.

At 2 cm, the Pixel sphere test's average shading-normal error fell from **2.92° to
2.20°**, and p95 shared-chunk normal disagreement fell from **13.54° to about
0.02°** (the angle calculation includes float-normal rounding). Surface RMS stayed
at about 1.521 mm, and coverage stayed complete. All nine scenes preserved their
coverage fractions, with zero nonmanifold edges and no new gap bridges. The thin
panel scene's average normal error rose from 11.76° to 12.16°; consistent shading
is not uniformly more accurate near thin/sharp features. At 4 cm, pre-existing
thin-panel gap bridges remain; this change does not repair them.

Raw target reports:

- `/tmp/opencode/generation-combined-pixel-2cm-20261001/results.json`
- `/tmp/opencode/generation-combined-pixel-4cm-20261001/results.json`
- `/tmp/opencode/generation-combined-paged-pixel-20261001/results.json`

## Fixed 501-frame recording

Geometry-only input SHA-256:
`8e46d57f3fe3b901ebd4ec640cc1d22a5a46b54ff51c2e3ed111ec3a6496a032`.
Both variants accepted all 501 frames at an explicit 2 cm resolution, with 512
changed chunks allowed per frame, 96 MiB voxel residency and 1 GiB backing.

| Diagnostic | Before | First combined candidate |
| --- | ---: | ---: |
| Logical voxel chunks | 2,874 | 2,874 |
| Peak resident voxel payload | 96 MiB | 96 MiB |
| Nonempty mesh segments | 1,972 | 1,972 |
| Triangles | 2,862,083 | 2,846,879 |
| Vertices | 1,636,498 | 1,628,896 |
| Exact-degenerate / nonmanifold / winding errors | 0 / 0 / 0 | 0 / 0 / 0 |
| Paired cross-chunk edges | 95,585 | 95,585 |
| Sampled input-to-surface RMS | 44.181931 mm | 44.181920 mm |
| Sampled median / p95 residual | 14.506 / 95.136 mm | 14.506 / 95.136 mm |

Triangle reduction is only about **0.53%** on this noisy recording. The mesh still
exceeds the separate 500,000-face texturing limit. The 96 MiB limit covers voxel
payload, not retained preview meshes or total process memory. Single host replay
times were 82.79 s and 83.58 s under differing host contention; they do not show
a reliable full-replay speedup.

Reports live under `/tmp/opencode/generation-{before,combined}-recorded501-paged-20261001`.
Input geometry was read from a frozen local copy; the benchmark did not change
the phone's capture, saved library or installed app.

## Verification references

- [Conservative extraction algorithm and fixed-volume tests](../tests/reconstruction/meshing.md)
- [Independent analytic and target comparison commands](../tests/reconstruction/generation_quality.md)
- [Frozen geometry replay and provenance checks](../tests/reconstruction/recorded_README.md)
- [Paging integration and budget semantics](reconstruction-paging.md)
- [Pixel accelerator findings](pixel-accelerators.md)

The first combined candidate passed the independent nine-scene 2 cm/4 cm checks,
the 2 cm ASan/UBSan/leak-check run, real NDK modern/legacy caller compilation,
generated JNI contract, modern APK assembly, all-library static 16 KiB audit,
zip alignment and APK signature verification. Final build identity and any
subsequent optimization measurements belong in the artifact record.
