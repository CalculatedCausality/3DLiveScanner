# CPU texturing / OBJ compatibility contract

The implementation is clean-room source in `reconstruction/texturing.cc`, with
private `texture_geometry.h` and `texture_obj.h`. It does not use the closed
Tango3DR binary. This is a CPU best-view texture baker, not a claim of equivalent
Tango quality or validated real-device performance.

## Exact exported ABI

All signatures come from the existing `tango_3d_reconstruction_api.h`:

- `Tango3DR_TexturingContext_create`
- `Tango3DR_TexturingContext_destroy`
- `Tango3DR_TexturingContext_setColorCalibration`
- `Tango3DR_updateTexture`
- `Tango3DR_getTexturedMesh`
- `Tango3DR_Mesh_loadFromObj`
- `Tango3DR_Mesh_saveToObj`

There is no GL texturing or dataset-convenience API implementation here. There
are no new public structs, getters or diagnostic ABI extensions. Configuration
is accessed exclusively through public `Config_getInt32/getDouble` functions.

## Options

| Key | Accepted values / behavior |
|---|---|
| `texturing_backend` | CPU (`0`) only |
| `mesh_simplification_factor` | Exactly `1`; geometry is never decimated |
| `texture_size` | Integer 16–4096, not necessarily a power of two |
| `max_num_textures` | 0–8; 0 chooses the bounded maximum, not unlimited storage |
| `bevel` | Finite 1–16 pixels, rounded up to a texel gutter |
| `min_resolution` | Finite nonnegative metres/texel; 0 seeds density from longest edge / 64 |
| `downsample` | Positive integer; process the first valid update and then every Nth valid update |

All atlas pages together are limited to **16,777,216 RGBA texels / 64 MiB**.
Explicit counts whose full pages exceed this ceiling are rejected, even if the
particular input mesh might use fewer pages. Examples: four 2048² pages or one
4096² page fit. `max_num_textures=0` uses at most eight pages within that ceiling.

Null config uses CPU, simplification 1, size 2048, count 4, bevel 3, resolution 0,
and downsample 1. The core Config object's historical simplification default may
be 3: **callers must explicitly set it to 1**. Unsupported values make context
creation return null. No transformation option is consumed; mesh coordinates and
camera poses must already use the same metric frame. The core must reject
unsupported transform settings rather than silently accept them.

Packing may coarsen the requested density in 1.25× steps to satisfy count/size
limits. It never drops faces. Failure to fit even minimum-size tiles returns
null. The effective density and page count are tracked in the context; the public
mesh exposes actual page dimensions/count and UVs, not an effective-resolution
getter. Each triangle uses a square right-triangle parameterization with its
longest edge setting density, so skinny triangles pack inefficiently.

## Projection, visibility, and ownership

- Poses are **camera-to-world**, quaternion `(x,y,z,w)`; camera axes are +X right,
  +Y down, +Z forward. A quaternion within 0.01 of unit squared norm is normalized;
  other/nonfinite poses are rejected.
- Supports finite Brown 2-, 3-, and 5-parameter calibration. Unknown and FOV /
  equidistant calibration models are rejected. Calibration/image dimensions must
  match. Focal lengths must be positive, principal point within the image.
- RGB/RGBA rows use byte stride. NV21 uses byte stride on both Y and interleaved
  VU planes; dimensions and stride must be even. NV21 is decoded as **full-range
  JPEG YCbCr**, matching the recorded dataset pipeline, not limited-range video.
  Input rows are top-down. Maximum source dimension 8192, 16,777,216 source pixels,
  maximum row stride 32768 bytes.
- Best view score is projected triangle area × absolute view cosine. Sufficiently
  grazing/subpixel views are skipped. No backface convention is assumed: tests
  are two-sided, and winding/normals are preserved verbatim.
- All vertices must project into the image and in front of the camera. Every
  atlas texel is projected from its 3D barycentric position, including Brown
  distortion; this avoids affine screen-space interpolation errors. Samples use
  bilinear filtering. Gutter texels clamp barycentric coordinates to the triangle.
- A median-split BVH performs camera-to-surface segment tests against **all mesh
  triangles**, including offscreen and back-facing occluders. Endpoint tolerance
  is max(10 micrometres, 1e-7 ray length). Every sampled point is tested. If any
  sample is occluded/out of image, the entire candidate triangle is rejected and
  its previous best view remains. This is conservative sampled visibility, not a
  proof against sub-texel occluders.
- There is no depth image argument. Visibility cannot detect transient objects
  or surfaces absent from the input mesh. No image blending, pose refinement,
  exposure correction, seam optimization, or photometric calibration is done.
- Context creation owns a copy of source triangle positions/normals/colors. It
  preserves face order and corner winding; extraction duplicates vertices for UV
  seams. Positions are never rescaled. Old textures are replaced, not retained as
  a fallback for unseen faces.
- Updates copy only accepted samples into a bounded atlas. **No source image or
  pose is retained**. Temporary image storage is at most one RGBA tile (up to one
  page). Geometry/BVH storage is O(face count), capped at 500,000 faces and
  1,500,000 source vertices. Persistent atlas plus geometry memory is independent
  of update count. Export/extraction requires additional output copies.
- Unobserved faces have texture ID `-1`, never fake vertex-color textures. If no
  face has been observed, extraction returns `TANGO_3DR_ERROR`.
- Extracted meshes are independent of the context and freed by core
  `Tango3DR_Mesh_destroy`. Core mesh texture allocations must be RGBA, stride
  `width*4`, with `std::free` ownership. OBJ variable-size texture buffers use the
  same malloc/free contract, without adding ImageBuffer ABI implementations.

Use contexts on one thread as required by the original API. Supplied pointers
must identify valid objects and complete stride-sized buffers: the C ABI has no
allocation-length argument and cannot verify a caller's underlying allocation.
Mesh capacities, array presence, finite values, indices, formats, dimensions,
and degenerate faces are checked. Creation returns null on errors; other entry
points return statuses and catch C++ exceptions. Failed extraction/loading leaves
the caller's output struct untouched. Successful extraction/loading assigns a new
owned struct; callers must destroy any previous output before reusing it.

## OBJ and images

The adapter uses production **`oc::File3d`, `oc::Mesh`, `oc::Image`**. It does not
replace their parsers or codecs with mock implementations. Input preflight guards
against silently discarded geometry, missing images and unsafe legacy PNG decode
error paths. PNG/JPEG input is fully validated using the real linked codecs first.
Inputs must stay immutable for the duration of a load.

Supported input is triangulated OBJ, independent position/UV/normal indices including negative indices,
one MTL in the OBJ directory, and diffuse `Kd`/`map_Kd` materials. Normals missing
from OBJ are generated by File3d. Textured faces must specify UVs. Unsupported
polygons, extra vertex components/colors, unknown statements, malformed faces,
degeneracies and nonfinite records return errors, rather than a partial mesh.
Quads are also rejected: pre-triangulate rather than relying on the production
parser's fixed diagonal for possibly concave or nonplanar polygons.

- Limits: OBJ/JPEG/PNG files 32 MiB each; MTL 64 KiB; OBJ line 4096 bytes; MTL line
  1000 bytes; nine material definitions (eight textures plus untextured); up to
  eight effective textures and 64 MiB aggregate decoded RGBA data. Texture images
  are limited to 4096 per dimension.
- PNG supports noninterlaced 8-bit RGB/RGBA. JPEG uses `.jpg`. Other image types,
  palette/grayscale/16-bit/interlaced PNG and map options are explicitly rejected.
- JPEG's production bottom-up memory is normalized to top-down texture storage;
  PNG is already top-down. OBJ UVs retain their standard bottom-left convention.
- Diffuse solid colors become 1×1 textures except white, which stays untextured.
  `Kd` for mapped materials is baked into RGB values. Image alpha is retained.
- MTL transparency and nonneutral/emissive/other material effects are rejected.
  Historical File3d boilerplate (`Ka 1`, `Ks .5`, `Ns 96.078431`, `illum 2`, etc.)
  is accepted as a diffuse compatibility subset. Original material names and
  specular/smoothing/group metadata are not representable in the Tango mesh ABI.
- Output is OBJ + MTL + `<base>_texture_<index>.png`. Geometry serialization and
  PNG encoding use File3d/Image. A small MTL adapter replaces File3d's fixed .64
  multiplier with `Kd 1 1 1`, avoiding darkened calibrated textures.
- Output groups faces by texture to bound material allocations. **Face order and
  vertex indexing may change on OBJ save**, but metric positions, triangle corner
  order/winding, UVs, normals and diffuse assignments are preserved.
- Arbitrary vertex colors on untextured faces are rejected on save: standard OBJ
  cannot represent them. Colors under actual textures do not replace those maps.
- Basenames/material and texture references with spaces are unsupported; parent
  directory names may contain spaces. Only lowercase `.obj` paths are accepted.
- Save returns errors on codec/I/O failure but is not a transactional multi-file
  publication API. It may leave partial output files. The caller should use its
  existing staged export/publish flow.
- `oc::Mesh` has shallow image pointers; load cleanup deletes each unique image
  exactly once. Save uses independent image owners and exception-safe detachment.

The backend serializes its own OBJ operations. Production `oc::Image` codecs use
global state; callers must also avoid concurrent unrelated Image codec calls.
This is a parent integration requirement, not a claim that these old helpers
have become thread-safe.

## Integration and host checks

Statically link `texturing.cc` into the modern reconstruction archive consumed by
`lib3dscanner`. Parent symbols must include File3d/Mesh/Image and the existing
libpng (simplified read enabled), TurboJPEG, zlib and C++ runtime. Include paths
must cover `common`, GLM, Tango ABI headers, libpng and TurboJPEG. C++ exceptions
must be enabled so failures can be converted to C statuses.

Run from the repository:

```sh
python3 tests/reconstruction/texturing_run.py
```

The runner builds real core, adapter, production File3d/Mesh/Image and **vendored
JPEG/PNG C sources** with ASan/UBSan. It uses only the existing host GL/log header
boundary stubs and system zlib. It performs no Gradle/device calls and keeps build
outputs in a temporary directory under `/tmp/opencode`.

The vendored JPEG code has legacy signed-left-shift UB (observed in
`jdhuff.c:577`). The runner disables **only UBSan shift instrumentation for the
vendored JPEG C objects**; ASan and remaining UBSan checks stay enabled there.
All new backend code, core, and production C++ helpers retain full ASan/UBSan.
This is an explicit test limitation, not a fix or a clean UBSan bill for the old
JPEG implementation. Core's test-only allocation failpoint also exercises partial
output-allocation cleanup and unchanged caller outputs.

Fixtures cover calibrated gradients, independent perspective/Brown projection,
translated/rotated poses, padded RGB/RGBA/NV21, source/result lifetimes, best-view
replacement, interior occlusion, behind-camera surfaces, unseen IDs, atlas page /
gutter bounds and exhaustion, downsampling, repeated updates, rejected options,
real OBJ/MTL/PNG roundtrips with **every exported PNG byte checked**, negative OBJ
indices, diffuse material transitions, malformed/missing files, I/O failure,
and real production JPEG → NV21 plus JPEG OBJ orientation/diffuse modulation.

These are host correctness fixtures. Android linkage, 16 KiB packaging, recorded
scene quality, throughput, and device behavior remain parent/device validation
tasks, not results asserted by this test suite.
