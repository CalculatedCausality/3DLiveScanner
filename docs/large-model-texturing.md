# Full-geometry captured-dataset OBJ texturing

## Installed repair and real-device result

The export repair is installed as
`artifacts/3DLiveScanner-modern-export-fix-debug-2026-10-01.apk`, SHA-256
`32d9a4561ce864aa7dc0cee49702c9104e193afe1327d5e3391d59511a0f8040`.
Installed identity matched; the private capture state and the saved dataset's
commit state and source OBJ fingerprints matched across the in-place update.
Startup reached FileManager and the new process's crash-buffer query was empty.

The real 263-frame Pixel recording was processed by the production native backend
on the phone, using private temporary output/scratch. Captured images remained on
the phone and temporary test exports were removed. The source was unchanged.

| Diagnostic | Final result |
| --- | ---: |
| Input OBJ size | 724,468,693 bytes |
| Input / output triangles | 2,519,369 / 2,519,369 |
| Photo-textured triangles | 1,858,112 (about 73.8%) |
| Photo-textured / total surface area | 20.704 / 29.189 m² (about 70.9%) |
| Native texturing/export time | 74.751 seconds |
| Native process peak RSS | 57,688 KiB (56.34 MiB) |
| Geometry scratch | 262,014,376 bytes |
| Atlas / photo-tile interior | 4 × 2048² / 182 × 323 |
| Retained photo scale | about 0.504 of native linear resolution |
| Output material runs | 5 (four photo pages plus neutral) |

This does not claim every surface has photographic coverage or that total app
export takes 75 seconds: reconstruction, raw OBJ saving, final orientation and
library publication are additional work. Unobserved or sample-occluded faces stay
neutral and retain their geometry. The photo resampling is explicit atlas-budget
packing, not a change to scan resolution. Full UI export acceptance remains a
user retry; the actual native export and caller/failure behavior were tested.

Real testing caught a defect absent from the initial coarse planar fixtures:
conservative whole-pixel depth rasterization assigned photos to only 74,324 faces
(about 3%). Simply clamping extrapolated depth made coverage worse and was rejected.
Actual triangle samples, adaptive supersampling and subpixel depth-variation bounds
replace that approach. A new unoccluded dense tilted-plane regression improved
from 8,952/20,000 to 20,000/20,000 textured faces; 2 mm parallel-layer separation
and front/back/partial-occlusion checks still pass.

Evidence: `/tmp/opencode/real-dataset-export-final-20261001/results.json`.
The final exporter source SHA-256 is
`05b2aa92db63fd669d5b0a951b8cb27971de3f693947d3c0a312ec373ad29687`.
Final host ASan/UBSan, 600k-face codec/geometry/visibility tests, modern/legacy
caller tests, Activity camera-admission tests, real NDK/JNI checks, both Java
flavors, APK linking, static 16 KiB/dependency audit, zipalign and signature checks
passed. App integration uses private cache scratch and releases the render lock
during offline texturing while suppressing camera resume.

## Integration contract

`reconstruction/dataset_texturing.h` exports this synchronous C++11 API in `oc`:

```cpp
DatasetTextureSettings settings;
settings.textureSize = 2048;
settings.textureCount = 4;
settings.scratchDirectory = privateCacheWorkDirectory; // optional; existing directory
DatasetTextureReport report;
bool ok = ExportDatasetTexturedObj(
    canonicalInputObj, separateOutputObj, dataset, settings,
    [](const std::string& stage, double fraction) {
        // Monotonic overall fraction in [0,1]. Return false to cancel.
        return true;
    }, report);
```

`report.faces` and `report.observedFaces` give full versus observed face counts.
`report.error` explains failure; `report.artifacts` is populated **only on success**
and lists the complete unique PNG/MTL resources plus the output OBJ. Copy/move/share
all these files together. Do not assume the MTL or texture has the OBJ basename:
resource names intentionally differ between exports to make replacement safe.
The OBJ and MTL use relative, whitespace-free resource basenames.
If the caller's subsequent reorientation fails, these artifact paths identify the
new export set to remove; the original input and its resources remain untouched.

`settings.scratchDirectory` optionally selects an **existing writable parent**
for disposable geometry scratch. Main can create `cacheDirectory/scanner-work`
and pass its absolute path, avoiding emulated-storage/FUSE random-access I/O.
The exporter creates an exclusive `.dataset-texture-XXXXXX` child there containing
only `vertices.bin` and `faces.bin`, and removes that child on success, failure or
cancellation. The supplied parent and unrelated contents are retained. An invalid
explicit directory fails with a path-specific diagnostic; it never silently falls
back to FUSE. The default empty string retains destination-side geometry scratch.
There is no hardcoded `/tmp` default on Android or any other platform.

OBJ, MTL and PNG staging/publication always remains beside the output OBJ, including
when geometry scratch is on a different filesystem. `report.artifacts` contains
only the published export set, never scratch paths. Scratch root selection does
not change the bounded-memory algorithm, geometry, visibility or atlas settings.

Call this instead of `TangoTexturize::Init/ApplyFrames/Process` for modern captured
datasets when `!poisson && !twoPass`. Main owns retaining the specialized paths with
explicit limitation diagnostics, storing options in `App::SetTextureParams`, and
JNI/UI error handling. This exporter neither calls `File3d::ReadModel` nor the
small-mesh Tango texturer. `reconstruction/Android.mk` includes the implementation
in `scanner_reconstruction` and exports `LOCAL_PATH` via `LOCAL_EXPORT_C_INCLUDES`,
so dependent Android modules can use `#include "dataset_texturing.h"`.
The host harness includes the repository root and uses
`#include "reconstruction/dataset_texturing.h"`.

Input is the canonical scanner mesh, **before** the outer `App::Texturize`
reorientation. Camera-to-world poses are `Dataset::ReadPose()[COLOR_CAMERA]`, with
the same translation and rotation used by `TangoTexturize::Extract3DRPose`.
Camera coordinates are +X right, +Y down, +Z forward. No pose inversion of the
stored dataset, model rotation, scale conversion, depth adjustment, or simplification
is performed. Existing outer reorientation remains the caller's responsibility.
Positions and normals are copied verbatim; each face's winding and separate
position/normal indices survive. Complete faces are regrouped by output material
so the app's OBJ reader does not create a render mesh at every best-view switch.
Old UVs/material selections are replaced.

The caller must keep the dataset/model immutable during export and serialize with
other legacy `Image` PNG operations (that codec has shared global state). The
callback runs on the calling thread and may throw; throwing is a failed export.
An empty callback is valid. Returning false cancels. The final callback with
fraction `1.0` occurs immediately **before** atomic publication, so UI success
must depend on the returned bool.

## Scalable path

1. A bounded line parser validates the triangular OBJ and copies all `v` and `vn`
   records to a staged output. Positions go to private disk scratch.
2. A second pass resolves positive, negative and forward indices using a fixed
   3 MiB direct-mapped vertex cache. The face scratch contains compact original
   indices, three float positions, a best-frame ID and score: **68 bytes/face**.
   Batches contain 8,192 faces. Only each batch's world-space bounds stay resident.
3. For each committed camera, batch-frustum culling avoids reading irrelevant
   batches. A first pass conservatively rasterizes all potentially visible
    geometry into one supersampled reciprocal-depth buffer. Near-plane
   crossing occluders are clipped; viewport-crossing occluders still participate.
    A second pass tests all covered grid samples of a candidate against that
    depth buffer. Grid samples lie inside the triangle; a triangle smaller than
    a grid cell uses its projected centroid with a reciprocal-depth uncertainty
    bound derived from the actual plane gradient. Geometry on either side of a
    triangle participates in occlusion. Projected area times view cosine selects
    its best accepted source frame.
4. Every original face is emitted. Observed faces get three projected UVs into
   their selected photo tile. Unobserved/partly occluded/degenerate faces retain
   geometry and normals with opaque neutral material. An entirely unobserved
   mesh is an error, not a successful grey export.
5. A bounded pass collects source frames selected by at least one face. Only these
   photos receive atlas tiles; unselected views cannot force unnecessary resampling.
   Source photos share an atlas grid. There are no per-triangle texture islands:
   three million faces can reference the same photo tile. Atlases are written one
    page at a time, with two-pixel extruded gutters and alpha 255 throughout.
6. Faces are streamed in one group per atlas page plus neutral material. Additional
   bounded scratch passes avoid storing a whole-model sort index and prevent
   thousands or millions of draw batches when reopening the exported OBJ.

This is a disk-streaming algorithm, not a whole-model mmap whose resident set can
grow to the size of the geometry. Scratch requires `12*vertices + 68*faces` bytes
(312 MB for nine million vertices/three million faces), in addition to the staged
OBJ and PNG output. It is created under the configured scratch parent, or beside
the output by default, and removed on all normal success/failure/cancellation
returns. Face count and input-file size do not set
arbitrary resource rejection thresholds. 32-bit source indices support up to
4,294,967,295 positions and normals; face/UV counters and file offsets are 64-bit
on the supported 64-bit Android/host targets.

Resident working storage consists of a 3 MiB vertex cache during indexing, a
544 KiB face batch, a float depth buffer during visibility, one decoded NV21
frame, codec scratch, one atlas page during PNG writing, and roughly 48 bytes per
8,192-face batch plus 160 bytes per camera. Default atlas creation uses one 16 MiB
page at a time, even though the total requested atlas budget is 64 MiB. JPEG input
size is bounded by `8*width*height + 1 MiB` before the real decoder allocates it.
OS filesystem page cache is reclaimable and is separate from process heap/RSS.
Visibility uses up to 4x sampling per axis, reduced for larger source images to
target at most 4,194,304 depth samples (16 MiB). If the native source grid already
exceeds that count, its full resolution is retained, up to the existing 64 MiB
depth-buffer maximum. A 360x640 recording uses a 1440x2560 visibility grid.

Worst-case visibility work is two geometry passes per frame, plus conservative
covered-pixel work; frustum culling helps spatially ordered scanner meshes. This
trades sequential scratch I/O for bounded RAM. On a completely overlapping scene,
263 views of three million faces read approximately 107 GB of scratch. Export can
therefore take minutes on a phone and needs sufficient free storage. It does not
silently reduce geometry, ignore views, or lower user texture settings to hide cost.

## Texture budget and quality

Defaults are exactly four 2048-square RGBA PNG pages (64 MiB uncompressed in total).
Explicit size/count settings are honored: size 16..4096, count 1..8, total at most
16,777,216 pixels. Count 0 explicitly requests the maximum page count within that
budget, capped at eight. An unsupported explicit request fails rather than clamps.

Every selected photo gets a grid tile with aspect ratio preserved up to integer
pixel rounding and the minimum two-pixel interior on each axis. Grid dimensions maximize
the minimum photo scale in the requested pages, without upsampling above native
photo resolution. All committed cameras participate in best-view selection at
native source resolution; no pre-decimation of images or visibility buffers occurs.
`frames` versus `usedFrames` reports input/selected photo counts. `sourceWidth`,
`sourceHeight`, `tileWidth`, `tileHeight`, `photoScale` and `atlasPages` report the
packing result. `photoScaleX=(tileWidth-1)/(sourceWidth-1)` and the corresponding
`photoScaleY` report the actual retained texel density after integer rounding.
Two-pixel gutters are included in packing. If even 2x2 photo interiors
cannot fit, export explains that the texture budget is insufficient. Resampling
uses bilinear filtering. The caller should show this resolution information rather
than suggest the output retains all native photo detail.

Visibility is a sampled test, not an exact continuous ray-triangle solution.
Reciprocal depth allows a relative 1e-5 floating-point tolerance plus 1e-7 absolute
tolerance. Subpixel centroids also use the projected plane's depth-variation bound
between that centroid and the grid sample; this is not a fixed metric-depth
tolerance. A regression checks that parallel layers only 2 mm apart remain
separated. Subpixel silhouettes and nearly coincident layers remain ambiguous.
Whole-face assignment keeps triangles neutral when a tested sample is occluded,
rather than subdividing or deleting geometry.
OBJ UVs are affine on each triangle; projected-camera texture mapping is an
approximation on large triangles with substantial depth variation, and is most
accurate for the scanner's small reconstructed triangles. There is no multiview seam
blending or exposure equalization. These affect colour fidelity, not retained geometry.
The camera is pinhole, matching the modern capture path's default disabled distortion.

## Codec orientation and validation

The exporter uses the real `Image::JPG2YUV`, which decodes dataset 4:4:4 YCbCr JPEGs
in top-down order. It does **not** use `Image(path)`'s bottom-up JPEG buffer as if it
were top-down. All declared frames must have a readable, finite rigid camera pose
and a correctly sized real JPEG; missing, malformed or truncated resources fail.
Calibration dimensions are even, at most 8192 per axis and 16,777,216 pixels in
total; finite positive focal lengths must not exceed 1e9 pixels.
The PNG writer preserves rows. Consequently output OBJ V is `1 - pngY/textureSize`,
with half-texel centre alignment. The test's red-top/blue-bottom image is generated
through real `Image::Write`, decoded through both real paths, exported, and sampled
from the real PNG using the actual exported UVs.

OBJ validation rejects nonfinite/out-of-float-range numbers, integer overflow,
zero/out-of-range indices, embedded NULs, truncated face records, polygons, unsupported geometry
directives and records exceeding 4,095 bytes. Blank lines, comments, groups,
object names, smoothing/material directives, optional RGB(A) vertex values and
independent normal indices are supported. Geometry validation never discards faces.

Source `mtllib`/`usemtl` records are accepted as metadata and replaced by the new
photo/neutral assignments. Their number is not limited; source MTLs and their
resources are not loaded or modified. In particular this path does not call the
old `texture_obj::materialFile` preflight with its small material-count limit.

Main's read-only on-device inventory of the actual 724,468,693-byte failing OBJ
reported **7,558,107 positions, 7,558,107 normals, zero UVs, 2,519,369 triangular
faces and 1,462 material runs**. These are reported source facts, not a replay
performed by this suite. Their geometry scratch requirement is **262,014,376
bytes**, plus staged output. The host material regression independently generates
1,462 neutral segments using flat position/normal triples and no UVs, verifies
that every face survives, and checks the source OBJ/MTL remain byte-identical.
Main owns the planned standalone native replay with a unique output under
`/data/local/tmp`; no source images need to be transferred to the host.

## Publication and failure behavior

The source is opened read-only; input/output aliases (including symlinks/hard links)
are rejected. Output is prepared in an exclusive sibling directory; disposable
geometry may instead use a separate exclusive child of `scratchDirectory`.
Files are flushed, checked and synced. Complete PNG/MTL files receive unique
exclusive-reserved names next to the destination. The OBJ is then atomically renamed
over the destination as the commit point. Any ordinary failure before that point
removes the newly created resources and scratch, leaving the previous OBJ and its
resources intact. No returned success can refer to a half-written texture set.
An unexpected process kill can leave orphan staging/resources, but cannot expose
a newly published OBJ referring to resources still being generated. This is not a
cross-filesystem transaction or a power-loss durability guarantee. Old successful
resource sets are not deleted automatically because other OBJ files may reference
them; the caller owns their lifecycle.

## Host verification

```sh
python3 tests/dataset_texturing/run.py --sanitize --faces 600000
python3 tests/dataset_texturing/run.py --faces 3000000 --frames 3
python3 tests/dataset_texturing/run.py --faces 4500000 --frames 263
```

The suite builds the actual vendored JPEG/PNG codecs, `Image`, `Dataset`, and this
exporter at C++11 `-O2`. Only GL/platform headers come from the host harness. Tests
cover front/back and partial occlusion, JPEG/PNG orientation, opaque alpha, unchanged
positions/normals/face indices, missing/truncated resources, malformed indices and
geometry, source aliases, cancellation, previous-output preservation, indexed/
negative/forward references, RGB vertex records alongside an untouched existing
MTL, 1,462 neutral source-material runs with zero source UVs, selected-only atlas
packing, necessary explicit-budget resampling, configurable scratch placement
with separate publication/cleanup and preserved caller-owned files, and a
>32 MiB / >500,000-face scanner-style flat mesh.
Scale verification streams independent hashes over every position/normal record
and order-independent checksums of each face's ordered position/normal index
triple, allowing material batching without missing faces or changed winding.
The harness prints input bytes, complete
observed count, elapsed export time, face throughput, peak RSS, scratch bytes, and
actual tile dimensions. `--keep` retains generated fixtures under `/tmp/opencode`.
No device or the user's private captured dataset is required.

### Initial host measurements (2026-10-01, before device visibility repair)

Linux x86-64/WSL host, Intel Core i9-13905H, C++11 `-O2`, scalar vendored JPEG
fallback, fixtures and scratch on `/tmp/opencode`. Export is single-threaded;
timings are host measurements, not phone forecasts. Peak RSS includes the test
process and independent streamed output verification, but not compiler processes
or reclaimable kernel filesystem cache.

| Input OBJ bytes | Faces | Positions / normals | Frames | Export seconds | Faces/s | Peak RSS |
|---:|---:|---:|---:|---:|---:|---:|
| 506,726,092 | 3,001,250 | 9,003,750 each | 3 | 16.489 | 182,015 | 24,484 KiB |
| 724,948,794 | 4,500,000 | 13,500,000 each | 263 | 106.914 | 42,090 | 24,652 KiB |

Every face in both planar scale scenes was observed and exported. The larger OBJ
is slightly larger than the reported failing 724,468,693-byte source. Its 263
360x640 frames were all evaluated; **six source photos won faces**. Packing only
those six preserves **360x640 native interiors**, reported scale **1.0**, in
exactly four 2048-square atlas pages. Unused views no longer impose avoidable
resampling. Scratch was **468,000,000 bytes**. The planar benchmark
cycles three distinct camera translations and really processes all 263 views; no
pose deduplication, shortcut or special benchmark path is used. Synthetic scenes
provide exact expected counts and colours; the user's actual capture was not
copied from the device or replayed here.

ASan/UBSan additionally passed the >32 MiB / >500k regression with a
99,838,408-byte mesh, 600,608 faces and 1,801,824 positions/normals each, plus all
failure/orientation/occlusion fixtures. Sanitizer peak RSS is around 717 MiB due
to instrumentation/quarantined test allocations; it is **not** the release memory
budget. Vendored JPEG's historical signed-shift instrumentation is excluded, as
in the existing real-codec harness; all other ASan/UBSan checks remain enabled.

Exact final measured commands (codec objects were reused from initial builds
with matching compiler/sanitizer flags):

```sh
python3 tests/dataset_texturing/run.py --sanitize --faces 600000 --keep --reuse-codecs /tmp/opencode/dataset-texturing-jltxj7b4
python3 tests/dataset_texturing/run.py --faces 4500000 --frames 263 --keep --reuse-codecs /tmp/opencode/dataset-texturing-jgdl4gvc
```

Both runs additionally cover extreme image aspect ratios, minimum two-pixel tile
interiors, and twenty disjoint winning views in a small atlas that necessarily
resamples them. Omit `--reuse-codecs` to rebuild everything independently. The retained large
fixture/build is `/tmp/opencode/dataset-texturing-8w40xhr3`; the test removes its
successful exported resource set after full verification but retains the input
OBJ and dataset. The Android translation unit also passes:

```sh
/tmp/opencode/android-sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang++ -std=c++11 -O2 -fexceptions -frtti -DANDROID -DSCANNER_MODERN=1 -Icommon -Ithird_party/glm -Ithird_party/tango_3d_reconstruction/include -Wall -Wextra -fsyntax-only reconstruction/dataset_texturing.cc
```

The isolated exporter suite does not itself exercise APK linking or Java flow;
the installed integration's separate verification is recorded above.
