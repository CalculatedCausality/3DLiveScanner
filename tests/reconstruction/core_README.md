# Independent reconstruction core

Optional bounded voxel paging is documented in
[`paging_README.md`](paging_README.md), with the public C extension in
`reconstruction/paging.h`. Paging is disabled by default; the RAM-only limits
below remain unchanged. Explicit paging can retain up to 32768 logical chunks
under separate caller-supplied resident/backing budgets.

Run `python3 tests/reconstruction/core_run.py` from the repository root. It uses
the checked-in compatibility header and GLM, C++11, ASan (including leak checking),
UBSan, and `-Wall -Wextra -Werror`. No vendor engine, Android SDK, GPU, or device
is involved. The runner builds in a temporary directory.

## Integration and ownership

Compile `reconstruction/core.cc` as C++11 with exceptions enabled and the include
paths `third_party/glm` and `third_party/tango_3d_reconstruction/include`. Link its
object into the modern static reconstruction library and then `lib3dscanner`.
Do not link the vendor reconstruction implementation in the same binary. The
legacy build may continue to use the vendor library. No on-disk dataset structs
or public ABI declarations have changed. Existing `.pcl`, `.bin`, and `.mat`
readers remain the responsibility of the caller; this core consumes their decoded
point arrays, poses, and mesh buffers.

Implemented public symbols:

- `Tango3DR_Config_create`, `destroy`, `setBool`, `setDouble`, `setInt32`,
  `getBool`, `getDouble`, `getInt32`.
- `Tango3DR_PointCloud_init`, `initEmpty`, `destroy`.
- `Tango3DR_Mesh_init`, `destroy`.
- `Tango3DR_GridIndexArray_destroy`.
- `Tango3DR_ReconstructionContext_create`, `destroy`, `setColorCalibration`,
  `setDepthCalibration`.
- `Tango3DR_clear`, `Tango3DR_updateFromPointCloud`,
  `Tango3DR_extractMeshSegment`.

Texturing and OBJ symbols are supplied separately by `texturing.cc`. Other
declarations in the much larger vendor header are **not implemented by this core**
(including depth-image/projective integration, whole-volume extraction,
floorplans, trajectories, PLY/image IO, and matrix/int64 config accessors).

All input arrays/images are borrowed for the duration of a call. Calibration,
configuration, and fused values are copied. No input pointer is retained.
Initializers and allocating output APIs accept an uninitialized wrapper, clear
it first, and leave a destroy-safe zero output on failure. **Release an existing
output's members before reusing it**; these calls cannot safely detect previous
ownership in an uninitialized C wrapper. Destroy functions free malloc-owned
members, reset the wrapper, and never free the wrapper itself; repeat destruction
of the same initialized point/mesh/index wrapper is safe. The caller may use
`new` or `malloc` for the wrapper and must release it separately. Context/config
handles are ordinary owning handles: destroy once and clear the caller's handle.

Mesh initialization allocates requested optional arrays and all texture-capacity
slots, even when `num_textures` is zero. Textures are RGBA8888, `stride=4*width`;
texture IDs start at -1. Mesh destruction frees all `max_num_textures` allocated
slots. Do not attach borrowed, `new[]`-allocated, or aliased members to an owned
mesh. Capacities must accurately describe its owned allocations.

Serialize access to each context/config and its lifetime externally. Independent
contexts can be used on independent threads. All allocating entry points contain
exceptions; no C++ exception escapes the C ABI. Keep exceptions enabled when
compiling this translation unit. `RECONSTRUCTION_CORE_TESTING` is a test-only
build flag enabling an allocation failpoint; omit it from production.

## Algorithm and bounds

The volume is a sparse map of 16³-node chunks. Node positions are integer lattice
coordinates times the metric `resolution`. Camera poses use x/y/z/w quaternions,
camera-forward +Z, and world-from-camera transforms. Nearly unit quaternions
(squared norm within 0.01 of one) are normalized; nonrigid/invalid poses fail.

Each accepted XYZC point traverses a +/-3-voxel ray-distance truncation band at
half-voxel steps. Trilinear node splats fuse locally projective signed distances
using confidence-weighted running means and capped historical weights. Positive
TSDF is observed free space. Axial depth, not Euclidean range, controls the
configured depth limits. This is a point-cloud ray-splat estimator, not the
vendor's projective-depth algorithm. It does not estimate pose, infer normals
from unorganized points, or fill unobserved holes. Very sparse rays therefore
produce missing surfaces rather than arbitrary cube geometry.

Space clearing traverses towards each observation and fuses positive evidence
into **already observed** free-space nodes; it skips empty chunks up to the next
interpolation-support boundary and does not allocate the entire camera frustum.
It can erase stale surfaces after repeated contrary evidence.
No automatic eviction discards accepted geometry. `clear` releases the volume
and timestamp while preserving config/calibration; callers clear cached meshes
themselves because that API has no dirty-list output.

Extraction uses a consistent six-tetrahedron subdivision of every lattice cell,
linear zero crossings, welded edge vertices, oriented triangles, and area-weighted
vertex normals. A cell is emitted only when all eight corners have at least
`min_voxel_weight` fused evidence (default 1), avoiding barely observed fringes.
Each cell belongs to exactly one chunk by its minimum corner, and extraction
reads the positive neighbor halo. Dirty results include changed chunks and their
negative neighbors, so clients must replace **every returned segment**, including
empty segments representing deletion. Segment boundary positions agree; normals
are accumulated locally and can have visible shading seams. Component filtering
is segment-local and conservatively retains boundary-touching components, whose
global size is not known. Seam membership is derived from integer lattice edges,
so rounding world positions to float cannot make a seam component appear interior.
This is intentionally not global Tango component parity.

Extraction caches the eight positive-neighbor chunk pointers and a fixed 17³
read-only voxel-pointer halo, immediately returns empty for an absent owner chunk,
and skips same-sign cells before tetrahedron traversal. Updates use a 64-entry
transaction-local chunk cache, retaining the original copy-on-write/commit order.
These changes preserve the fusion arithmetic and observable mesh ordering;
[`core_performance.md`](core_performance.md) records frozen-source byte comparisons
and host timing evidence.

A subsequent bounded local-Z memoization investigation retained **no engine
change**: both tested caches lacked a meaningful measured gain. See
[`core_localz.md`](core_localz.md) for timings, correctness/stack checks, and the
verified restored source hash.

Color uses world-to-color-camera projection with independent camera pose,
intrinsics, bilinear sampling, and the header's pinhole, Brown polynomial (2/3/5),
or FOV distortion models. RGB888/RGBA8888 use byte strides. NV21 requires even
width/height/stride, a Y plane followed by interleaved VU rows at the same stride,
and BT.601 limited-range conversion. The image must match calibration dimensions.
The ABI carries no byte-buffer length, so callers must provide storage for those
declared rows. Unprojectable samples do not overwrite prior color; uncolored
vertices default to opaque white. This is vertex coloring, without independent
color-camera occlusion testing or photometric correction.

Bounds per context/call:

| Resource | Default | Hard allowed bound |
| --- | ---: | ---: |
| Persistent chunks (`max_chunks`) | 1024 | 4096 |
| Changed chunks/frame (`max_update_chunks`) | 256 | 1024 |
| Point/step/node visits (`max_update_work`) | 32,000,000 | 64,000,000 |
| Input points/frame | 1,000,000 | 1,000,000 |
| Voxel payload/chunk | 96 KiB | 96 KiB |
| Mesh-init total owned buffers | 256 MiB | 256 MiB |
| Image/calibration dimensions | 8192 each | 8192 each |
| Input ray length | 200 m | 200 m |

The default persistent voxel payload is at most 96 MiB plus at most 24 MiB of
staged changed chunks. Configured maxima permit 384 MiB persistent plus 96 MiB
staged payload. Map/shared-ownership metadata and extraction/output buffers are
additional. Transactions copy the bounded chunk map, not all voxel payloads.
The hot-path caches add about 2 KiB of transaction-local storage and 39 KiB of
fixed extraction scratch on a 64-bit build; they add no persistent volume cache
or heap allocation/failure points.
At most eight dirty indices are generated per changed chunk before deduplication.
Extraction examines 4096 cells with at most 12 triangles each (49,152 triangles)
per segment. Coordinates are bounded to +/-998,976 lattice units at input,
leaving arithmetic/truncation headroom; float output precision degrades far from
the origin. Metric accuracy is tested near the origin, not at those hard limits.
Bounds are per context; callers must also bound the number of simultaneous contexts
and the lifetime of extracted meshes. This implementation is serial; the parallel
flag is accepted as a scheduling hint, not a promise of parallel execution.

Updates stage changes and allocate dirty output before a no-throw commit. Invalid
input returns `TANGO_3DR_INVALID`; resource/work bounds return
`TANGO_3DR_INSUFFICIENT_SPACE`; allocation failures and missing color calibration
return `TANGO_3DR_ERROR`. All failure classes leave the old volume and timestamp
intact and the output empty. A next good frame can proceed. Extraction failure
also leaves the volume unchanged. For this modern backend, a resource-limit
rejection is **not a corrupt context**: callers should retain the context and
committed preview instead of replaying the full history in response to
`INSUFFICIENT_SPACE`. Replaying cannot make an over-budget frame fit. Replaying
the same ordered observations/configuration reproduces the
volume within a given build; replay is not deduplication, and re-submitting a frame
adds evidence again.

Update resource failures emit `ScannerReconstruction` warning diagnostics on
Android (stderr on hosts), naming `max_update_work`, `max_chunks`,
`max_update_chunks`, or `max_points_per_frame`. The last name describes the fixed
point-count bound, not a configurable key. Logs include requested/capacity values,
work, touched/staged/committed chunk counts and input point count. Logging is
throttled process-wide to at most one message per reason per five seconds, also
across context replacements; a contended logging attempt is dropped. Diagnostics
do not allocate reconstruction storage, alter status codes, or change bounds.

## Configuration

Unknown keys, wrong types/config family, unsupported values, and nonfinite values
return `INVALID` without modifying the config or getter output. No opaque config
definition needs to be shared with texturing; all getters are public C functions.
Setters allow temporarily inconsistent min/max depth so callers can set them in
either order; context creation rejects `min_depth >= max_depth` and a
`min_voxel_weight` greater than `max_voxel_weight`.

Reconstruction keys (default; range):

- double `resolution` (.03; .001–1 m), `min_depth` (.60; 0–100 m),
  `max_depth` (3.50; .001–100 m), `min_confidence` (0; 0–1),
  `min_voxel_weight` (1; .001–65535).
  Confidence must be strictly greater than the threshold; zero is always ignored.
- bool `generate_color` (true), `use_space_clearing` (false),
  `use_parallel_integration` (false, scheduling hint),
  `use_clockwise_winding_order` (false), `rectify_color_image` (true).
  Rectification is performed through distorted-coordinate sampling; false assumes
  the supplied image is already rectified. Clockwise changes faces, not outward normals.
- int32 `max_voxel_weight` (16383; 1–65535), `min_num_vertices` (1; 0–1,000,000),
  `update_method` (0; **only traversal 0 accepted**), and the three bounded-resource
  keys in the table (minimum 1).

Texturing-family storage/getters (actual interpretation is texturing's responsibility):

- int32 `texturing_backend` (0; CPU 0 only), `mesh_simplification_factor` (3; 1–1000),
  `texture_size` (2048; 1–8192), `max_num_textures` (0; 0–256),
  `downsample` (1; 1–1,000,000).
- double `bevel` (3; 0–256), `min_resolution` (0; 0–100).

The current CPU texturer validates a narrower subset at context creation. In
particular, modern callers must explicitly set `mesh_simplification_factor=1`
despite the config object's historical default of 3. Its accepted sizes/counts,
gutter bounds, aggregate atlas limit, and output ownership are documented in
[`texturing.md`](texturing.md). The core's RGBA allocation/free contract matches
both atlas extraction and the OBJ adapter's variable-size replacement buffers.

NV21's format enum does not specify color range: core sampling uses limited-range
BT.601, whereas the CPU texturer uses full-range JPEG YCbCr. The existing producers
also differ: `Image::ExtractYUVDownscaled` uses limited-range coefficients, while
`Image::JPG2YUV` obtains JPEG planes. Interchange fixtures must check each producer
against its receiving backend rather than assuming identical NV21 bytes have the
same RGB interpretation in both paths.

Unsupported keys such as floorplan options and external transform matrices are
not silently accepted. Depth calibration can be copied/validated, but traversal
of already metric XYZ points does not use it; it does not enable projective mode.

## Validation scope

The analytic tests measure plane/cube surface errors, triangle area, normals,
interior edge incidence across negative and positive chunk boundaries, independent
color-camera projection, padded image layouts, confidence/noise fusion, clearing,
incremental updates/replay, input rejection, resource limits, and allocation-failure
rollback/cleanup. The runner prints measured errors and allocation-sweep counts.
Synthetic host tests do not demonstrate Tango parity, watertight arbitrary scenes,
tracking robustness, phone quality, sustained mobile latency, or 16 KB ELF/device
compatibility. Main integration must still validate library linkage/page alignment,
recorded datasets, full save/export/recovery, and Pixel behavior and performance.

### Measured host result (2026-09-30)

The C++11 ASan/UBSan/leak-check run passed, as did the separate optimized production
compile with warnings as errors. Symbol inspection found exactly the 21 public
Tango-prefixed APIs listed above and no test failpoint in the production object.

These are **vertex-to-analytic-surface** distances, not a Hausdorff bound or a
measurement on captured phone data:

| Synthetic input | Voxel size | RMS error | Maximum error |
| --- | ---: | ---: | ---: |
| Fronto-parallel plane | 40 mm | 0.000029 mm | 0.000029 mm |
| Rotated/translated plane | 40 mm | 0.000032 mm | 0.000096 mm |
| Sloped finite plane | 40 mm | 0.540 mm | 4.534 mm |
| Six-view 400 mm cube | 25 mm | 10.396 mm | 23.559 mm |
| Eight noisy plane frames, input sigma 8 mm | 40 mm | 0.711 mm | 2.575 mm |

The plane produced 12,800 triangles and 2.56 m² area; all 14,840 checked interior
edges, including chunk seams, had incidence two. The cube produced 7,680 triangles
and 1.017549 m² area versus its analytic 0.96 m²: edge rounding/extension remains
a material accuracy limitation. Calibrated gradient-color maximum channel error
was 6.523/255; constant-color RGB/RGBA/NV21 cases also passed.

Allocation failure sweeps covered every allocation position before success for
mesh initialization (9), transactional update (23), and segment extraction (101).
Each rejected update left existing geometry byte-identical, did not publish new
chunks, returned an empty output, and allowed subsequent success. Other checks
passed for dirty-cache versus full extraction, deterministic ordered replay,
clear/reuse, reversed-camera negative-boundary clearing, sparse empty-space work
budgets, 1 km translated coordinates, confidence/depth filtering, invalid input,
optional buffers/winding, and member-only repeated destruction.

An additional regression exposed and fixed float-inexact boundary classification
in component filtering. A connected plane spanning positive/negative 0.64 m chunk
seams now remains byte-identical even with `min_num_vertices=1000000`;
seam-connected pieces are conservatively retained based on lattice topology rather
than a floating-position tolerance. The full sanitizer suite and production API
check passed again after this correction; the measurements above are unchanged.
