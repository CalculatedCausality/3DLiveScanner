# Live preview stability trial

## Report and diagnosis

The user reported large holes and fragmented surfaces in the **live scanning
preview**, not a confirmed failure of the processed model. A foreground-only
screen capture confirmed visibly broken surfaces. Performance work was paused.

The phone had 2 cm requested resolution, experimental 5 cm coverage preview,
TPU correction and free-space clearing enabled. Restoring requested-resolution
preview alone was not sufficient to establish a visual repair. The TPU model's
real-scene accuracy is also unvalidated; disabling it is an isolation measure,
not a finding that it caused the holes.

A stable geometry-only copy of the then-current 484-frame recording was acquired
without changing phone files. It contains 678,594 points; no photographs were
acquired. Fixture SHA-256:
`f10db463a0a58579b88aa40af3f3730da61697b7658ccd1dc5e3f9da17282803`.
The recording already contains the depth policy used during capture, so replay
does not constitute a corrected-versus-original TPU comparison.

## Same-input replay

Diagnostic configuration used the phone's 100 m depth limit and nine-vertex
component threshold, 96 MiB voxel residency and a 512-chunk frame budget.
All 484 frames were accepted in all three runs.

| Replay | Surface area | Faces | Open boundary edges | Sampled input residual p95 |
| --- | ---: | ---: | ---: | ---: |
| 5 cm, clearing on | 13.556 m² | 45,931 | 4,519 | 79.51 mm |
| 2 cm, clearing on | 10.798 m² | 230,961 | 26,635 | 58.57 mm |
| 2 cm, clearing off | 13.155 m² | 274,989 | 33,339 | 46.92 mm |

Clearing materially removes surface in this recording: disabling it at the same
2 cm resolution retains about 22% more area and improves agreement with sampled
inputs. **This does not prove physical accuracy or closed surfaces.** Open-edge
counts increased, and more retained area can include noise. Boundary counts
across different voxel resolutions are not directly comparable. The diagnostic
omits photographic color; it is a geometry comparison, not an exact live render.

The replay harness now accepts explicit `--max-depth`, `--min-vertices` and
`--no-clearing` options and records the actual configuration. Defaults retain
the preceding test configuration.

Reports: `/tmp/opencode/preview-holes-{coarse,fine,fine-no-clear}-20261001/results.json`.
Frozen input: `/tmp/opencode/recorded-preview-holes-20261001`.

## Stabilization configuration

- Full-detail live preview at the selected resolution; coarse preview defaults
  **off** and is explicitly labelled experimental in Settings.
- Experimental TPU correction **off** for this trial.
- Free-space clearing **off** for this trial, to avoid erasing previously observed
  surfaces while assessing continuity. This can also retain inconsistent geometry.
- Capture-first saving, captured data and existing original-depth backups remain.
- The geometry-equivalent capture-lock separation and voxel-pin reuse remain.

The debug-only shell receiver can set `space_clearing` for an explicitly requested
trial. The installer respects the artifact's declared settings rather than
unconditionally re-enabling coarse preview or TPU correction.

## Installed artifact and status

`artifacts/3DLiveScanner-modern-preview-stability-debug-2026-10-01.apk`

SHA-256: `b82c43b162a0bd364084b619301bf177740aaa3c3cda5920a6101d23cf451fae`

Installed after the user confirmed the current scan was saved. Installed bytes
matched, the capture commit-state check passed and startup reached FileManager.
The receiver verified `coverage_preview=false`, `space_clearing=false`, TPU off,
and `capture_first=true`. Native libraries are byte-identical to the preceding
fusion-cache build. Capture-policy checks, modern assembly, legacy Java,
static 16 KiB/dependency audit, zip alignment and signature checks passed.

**This settings-only trial did not resolve the report.** The user tested a fresh
scan and reported “Still badly fragmented.” It was followed by the
[partial-cell meshing repair](partial-cell-meshing.md), after which the user
reported a clear visual improvement. Toggling preferences cannot retroactively
rebuild an already-integrated preview.
