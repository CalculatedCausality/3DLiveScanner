# Exact normal-cache performance follow-up

Baseline: the visually accepted partial-cell mesher, core SHA-256
`cb18ca86172b296b7dbae0f480f8c16c375b4a144fe705a108065012c3b513a3`.

## Changes and numerical contract

- Each extraction caches exact double-precision field gradients at its 17³
  possible lattice nodes. Neighboring edges no longer recompute the same
  gradient repeatedly. Failed support queries are cached too. The field snapshot
  is immutable throughout that extraction; nothing persists into a later update.
- Cells missing either endpoint of the subdivision's shared 0–7 diagonal are
  skipped early. Every one of its six tetrahedra needs those endpoints, so this
  does not undo the partial-cell repair.
- The extra bounded scratch payload is 122,825 bytes per active extraction:
  4,913 × three doubles plus 4,913 status bytes, excluding structure padding.
  It is stack storage, not retained per-chunk memory or extra pager residency.

The endpoint-only candidate showed essentially no extraction gain on the Pixel
and was not promoted alone. The measured combination includes both changes.

## Pixel comparison

ABBA order, 24 generated colored moving-view frames, 2 cm resolution, paging,
clearing disabled, CPU 5. This matches the accepted resolution/clearing policy;
it is a native component benchmark rather than a camera/FPS test.

| Sequence total | Before | After |
| --- | ---: | ---: |
| Fusion | 988.732 ms | 986.535 ms |
| Mesh extraction | 1,685.542 ms | 1,612.260 ms |
| Combined | 2,674.274 ms | 2,598.795 ms |

Extraction time fell **4.35%**, with **2.82%** less combined work. All
**186,582,666 output bytes**—including vertices, normals, colors and faces—matched
in all four runs. Output SHA-256:
`411a8123f9a1a233b4d78d8064b92a4db2ff95c26fdceb2a1b836a0a0f2b368b`.

Evidence: `/tmp/opencode/partial-mesh-normal-cache-pixel-20261001`.
Frozen variants: `/tmp/opencode/partial-mesh-fastpath-20261001`.

The partial-cell regression now covers all six tetrahedral orientations, missing
and weak nodes, positive/negative chunk boundaries, RAM and minimum-budget paging.
The update also contains the independently verified NNAPI library-lifetime fix
described in [tpu-capture-acceleration.md](tpu-capture-acceleration.md).

## Verification and installed build

All six partial-cell orientations, core geometry and paging failure suites pass
under sanitizers. The 484-frame / 678,594-point recording produces the same mesh
and normals as the visually accepted partial-cell mesher, with ordered output
SHA-256 `578908ed6c48bd48693c9496af6f41a23f79b502a2607253dc3619b271133f1c`.
Report: `/tmp/opencode/normal-cache-recorded-20261001/results.json`.

Production source hashes:

- `reconstruction/core.cc`: `eea8cc7fa48e3a92415233775bd2f0c80ac01f4853230cb82b0f1f4ee9476b32`
- `reconstruction/field_normals.h`: `6ede4d244ff46a2a497cdacf8639f2424d1552e97bccd2fc4c7645afce2d310a`
- `common/depth/experimental.cc`: `9342cd440f86af707e2c5bd819cac09d2265cb8339c74776df8ca83a6c80633a`

Installed artifact:
`artifacts/3DLiveScanner-modern-normal-cache-debug-2026-10-01.apk`, SHA-256
`df460256bb3aeb04e01d4378c3f08ad2d0a1741eadd9cf8abdd81c11e06f370d`.
Installed after the user's saved-scan confirmation; APK identity and capture
commit-state preservation passed, startup reached FileManager, and the scoped
crash buffer for PID 27034 was empty. Modern assembly, static 16 KiB/dependency
audit, alignment and signing checks passed.

The accepted profile remains: full-detail preview, TPU correction off, clearing
off, capture-first saving on. No new neural model or mesh offload is enabled.
