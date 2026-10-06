# Owned reconstruction / 16 KiB migration

Status: **experimental, not accepted for live scan quality**. Moving/revisit
scans failed despite passing host and static checks. The default `quality` flavor
preserves the user-validated original native binaries in a separate application.
See [current acceptance status](../docs/quality-modernization.md) and
[rollback evidence](../docs/quality-rollback.md).

The implementation evidence below records historical modernization trials. A
short 98-frame capture and successful startup did not establish sustained scan
quality. The Pixel has 4 KiB pages; original vendor binaries are not claimed to
support a 16 KiB kernel. See [build variants](../docs/scanner-build-variants.md).

## Current implementation evidence

The owned core and CPU texturer have passed their host suites, including
allocation-failure cleanup, real image codecs and OBJ/MTL/PNG round trips. See
`tests/reconstruction/core_README.md` and `tests/reconstruction/texturing.md` for
the algorithms, measured fixture errors and explicit bounds. Main independently
reran both suites successfully. These results are not device-quality parity.

The modern dependency build uses ARCore 1.56.0 and NDK r28c. The first full modern
debug APK built successfully and passed `tools/check_modern_apk.py`, Android
`zipalign -c -P 16`, and APK signature verification. Its four packaged libraries
are scanner, ARCore C/JNI and libc++; no Tango/Huawei/GVR binary or load-time edge
remains. This is static evidence, not 16 KiB runtime or scan-quality validation.

The synthetic recorded-dataset integration passed with the real Dataset reader,
JPEG decoder, modern live-image conversion and owned reconstruction: 3,354
triangles, finite metric coordinates and zero measured plane-Z error for that
fixture. It does not establish behavior on arbitrary existing recordings.

Modern live image conversion explicitly uses standard RGBA to limited-range
NV21 for the new core. The legacy producer branch retains historical bytes;
offline recorded-JPEG texturing remains full-range YCbCr. Both conversion branches
passed golden-byte/resampling/bounds checks. Keep these producer conventions
distinct when adding integration coverage.

Modern texturing currently preserves geometry rather than implementing the old
simplification factor, so callers must select factor 1. Atlas memory and mesh
limits are intentional failure boundaries, not silent geometry reduction. The
core is a bounded serial TSDF/ray-splat implementation and relies on supplied
camera poses; it does not add a new global tracking/loop-closure optimizer.

The newer paging candidate adds an opt-in, bounded resident voxel cache with
checked private disk backing. App integration uses resolution-aware disk capacity,
memory-budgeted frame admission and rejection cooldown. Exact RAM/paged comparisons
and target ARM64 eviction/reload checks passed; this does not bound preview/export
memory or establish a live-camera FPS gain. See `docs/reconstruction-paging.md` and
`tests/reconstruction/paging_README.md` for budgets and acceptance boundaries.

## Boundary

The modern Pixel/ARCore build replaces the binary Tango dependency with owned
source linked into the scanner. The existing Tango3DR C declarations are used as
an adapter contract so `.pcl`, `.bin`, `.mat`, image and state files do not require
a destructive conversion. This is a new implementation, not Google's Tango code.

`core.cc` owns configuration getters/setters, point-cloud/mesh/grid-array ownership
and incremental reconstruction/extraction. `texturing.cc` owns calibrated image
projection/atlases and OBJ loading/saving. Configurations are shared through C API
getters rather than private handle layouts. Neither module retains caller-owned
input buffers after an update returns.

The modern build must not depend on the old Tango/Huawei/GVR shared libraries at
load time. A legacy build may remain for comparison; that does not establish its
16 KiB compatibility.

## Integration acceptance

- All scanner-used C entry points have real implementations and checked failure
  behavior; unsupported options do not return success while doing nothing.
- Destroy functions release members, not caller-owned wrapper structs. Repeated
  empty cleanup, allocation failure and rejected inputs are covered.
- Owned-core update rejection leaves the reconstruction unchanged, including
  resource/allocation failures. The wrapper skips that frame without replay.
  Failures after successful integration (extraction or persistence) still require
  recovery from committed history before another live frame.
- Analytic geometry fixtures establish metric scale, pose transform, normals,
  winding, chunk seams and a reported surface-error bound. Replay, clearing and
  extraction of emptied chunks must behave consistently.
- Texturing fixtures establish visibility/occlusion, image orientation, UV and
  atlas bounds, source lifetime, material references and actual texture content.
- Existing dataset/preview formats remain readable. A new live backend must not
  mix incompatible chunk keys into an old preview silently; replay/rewind and
  imported-dataset export require explicit integration tests.
- Frame-data/state publication order and failure-safe export remain intact.
  Processing failures must return to Java/UI rather than call `exit()`.
- Every packaged native library is audited for LOAD alignment/congruence,
  RELRO protection safety and APK mmap alignment; NEEDED dependencies must all
  be present and compatible. Compression or warning suppression is not an ELF fix.
- A modern APK must pass build, signature and static 16 KiB checks, then startup,
  capture, pause/resume, save/reopen and textured sharing tests on the Pixel.
  Runtime testing on the currently 4 KiB phone is separate from a true 16 KiB
  runtime test.

The old 2017 Tango binary's RELRO overlaps mutable data when rounded to 16 KiB.
It cannot be repaired by zip alignment or by changing the scanner's link flags.
Do not weaken RELRO, patch dependency headers or disable compatibility checks to
make an audit appear green.
