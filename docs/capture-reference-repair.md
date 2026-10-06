# Capture reference and revisit investigation

## Current user findings

The early partial-cell change improved local mesh coverage, but later moving
scans still morphed and changed elevation. Numerical replay equivalence did not
establish real motion stability.

The measured-depth candidate (`e0c2a311…`) made the result worse: the user reported
streaky tracks. It removed reference-anchor compensation under the incorrect
assumption that ARCore world coordinates formed an immutable capture frame.
ARCore explicitly updates world-space estimates over time. Raw-only selection
also left blank walls and the white appliance almost entirely unmeasured.

The subsequent reference-depth build (`f4d6eeed…`) restored an anchored frame and
dense support. The user reported that **new areas scanned really well**, but
backtracking over existing areas still mangled the result. That remains the
specific acceptance criterion for the current work.

## Preserved evidence

- Initial motion failure: 481 frames / 2,677,836 points, fixture
  `/tmp/opencode/recorded-motion-drift-20261001`, SHA-256
  `160aae13cf6c8625e74fde1ea2893596eb5bff63aa431db3aa621120f1f90b2a`.
  Recorded confidences were all 1. The recording spans 76.57 seconds; large
  between-recorded-frame motion must be interpreted with its timing gaps.
- Streaking regression: 434 frames / 888,147 points, fixture
  `/tmp/opencode/recorded-streaking-20261001`, SHA-256
  `9bada743de8cb929db303e70115ed1901c55178929a58c861ceedbc6f10b7ee7`.
  Measured confidence values range from 129/255 to 1. Median points/frame: 1,373.
- Twenty selected saved photographs were copied only for local alignment
  diagnosis, outside the repository:
  `/tmp/opencode/streaking-alignment-images-20261001`. Feature/depth association
  is not independent ground truth. SIFT had too few depth-supported matches;
  forward/backward optical flow yielded useful matches in some pairs, with
  typical world discrepancies of about 1–5 cm and occasional larger outliers.
- The source overlay showed dense-enough raw patches on textured floor regions
  but almost no raw measurements on blank walls or the white appliance.
- The later backtracking path is preserved in
  `/tmp/opencode/backtrack-poses-20261001`: 416 frames, maximum displacement
  8.516 m from the start and about 49.72 m of sampled camera travel. It crosses
  the 4 m reference hand-off threshold.

Shared-XZ-cell height changes alone were not treated as proof of tracking error:
different surfaces and occlusions can occupy the same horizontal cell.

## Capture behavior retained from the first-pass improvement

- Camera and depth use one session-anchor-relative model frame. Session anchors
  avoid attaching the entire scan to a fitted depth-hit surface.
- A nearby reference is established beyond 4 m. Coordinate continuity is
  preserved at creation. Capture is refused if no usable reference is available
  within the bounded range.
- Reliable raw depth samples remain unchanged and retain measured confidence.
- Missing raw coverage can use ARCore's dense estimate at fusion weight **0.1**,
  bounded to 0.15–8 m. This weight is a heuristic, not a calibrated confidence
  probability. Dense estimates remain less certain than raw measurements.
- No arbitrary geometry offset, custom wall extrusion or raw-to-smoothed
  replacement is applied in the modern path. TPU correction and clearing remain
  off. The calibrated projection and native-dimension UV cache checks remain.

## Revisit-specific correction

The single-reference implementation detached the preceding anchor at each
hand-off. On returning to an old area, its original local reference was gone;
a newly created reference inherited the current region's correction.

The correction keeps a bounded history of **32 session references** and their
original model-space poses. It reuses a closer previous reference on return,
with distance hysteresis to avoid oscillation. If the required local reference
is not tracked, capture is rejected rather than continuing with a different
region's correction. Old references are not silently evicted at capacity.
All retained references are released when the scan/session is cleared.

Read-only diagnostics expose the active reference, retained count and reuse
count, so a live return trip can verify that the intended path was exercised.
This is reference reuse, **not full geometric ICP or pose-graph optimization**;
real revisit quality still needs to be checked.

## Tests and limitations

`tests/depth_capture/motion_run.py` exercises production source methods with SDK
fixtures and the real reconstruction core:

- Metric depth projection under moving cameras and clipping-plane changes.
- Raw-sample preservation, lower-weight dense selection, bounded estimates,
  missing-data exclusion and reference-based frame admission.
- Rotated/cropped UV mapping across depth-image dimension changes.
- Coherent SDK world rebases, nearby hand-offs, deferred first-anchor tracking,
  untracked/far-reference rejection and ownership cleanup.
- Revisit of an earlier region after another reference is independently shifted:
  the original reference must be reused without adding a new one.
- A Pixel-sampling synthetic floor/wall scene: raw-only input leaves the wall
  absent; dense support reconstructs the tested visible wall and floor. This is
  simulated input, not a claim of real phone accuracy.

An earlier undersampled 64×48 fixture produced only 32.9% wall coverage even
with dense support. The device-matched test uses the recorded sampling density
and focal terms; this does not eliminate the reconstruction's limitations when
depth sampling is too coarse for the selected voxel size.

The live calibration diagnostic on the Pixel reported texture intrinsics
`fx=117.3185`, `fy=117.1800` at 160×90 and a maximum display-ray discrepancy of
**0.000463 m at 2 m** versus direct texture-intrinsics unprojection. This checks
coordinate mapping, not sensor-depth accuracy.

References:
- https://developers.google.com/ar/develop/anchors
- https://developers.google.com/ar/develop/c/depth/raw-depth
- https://codelabs.developers.google.com/codelabs/arcore-rawdepthapi
