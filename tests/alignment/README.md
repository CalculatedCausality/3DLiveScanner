# Object alignment correctness

```sh
bash tests/alignment/run.sh
```

Compiles the production `optimizer.cc`, `file3d.cc`, and `mesh.cc` under host
ASan/UBSan. Only image/codec/GPU dependencies are stubbed. Requires `g++` (or
`CXX`) and `/tmp/opencode`; fixtures and executables are temporary. Assertions and
sanitizer failures fail the runner.

## Preserved ordinary behavior

This operation normalizes a standalone OBJ object; it is not frame registration.
The orientation candidate remains the largest **perimeter**, not area, among
valid triangle faces. Winding is unchanged: the negative geometric face normal
becomes output +Y. The original `(x,z,-y)` axis conversion is included in the
single rotation matrix. Afterwards the referenced geometry's X/Z AABB midpoint
becomes zero and its minimum Y becomes zero. This intentional recentering moves
the world origin; it must not be described as retaining absolute world poses.

## Correctness fixes

- Compute cross products, lengths, rotation, and bounds in double precision.
  Zero-area/nonfinite faces cannot define orientation. A model with no usable
  face is left unchanged. No triangles are deleted by the optimizer.
- The original `cross(worldUp, normal)` cannot determine heading at either Y
  pole. When its length is at most `8 * float epsilon` (about `9.54e-7`), project
  world +X into the tangent plane instead. This narrow float-input uncertainty
  band has an explicit deterministic heading: +Y gives identity, -Y a 180-degree
  rotation about X. Outside the band the original heading formula is retained.
  There is no claim of a globally continuous heading through this singularity.
- Require finite entries, an orthonormal 3x3 block, determinant +1, and a pure
  rotation homogeneous row/column. The old comparison of only the length of a
  decomposed scale vector did not establish rigidness.
- Bounds, positions, and normals share the same rotation. There is no fallback
  that bounds unrotated data but then applies a hidden axis conversion on output.
  Normals are rotated without translation or rescaling; zero normals stay zero.
- Use unbounded-line parsing of actual `v`/`vn` records. Preserve UVs, `vp`, face
  and material records, comments, and suffixes such as vertex colors. Export
  coordinates at 17 significant digits and reject nonfinite/out-of-float-range
  results rather than writing invalid coordinates.
- Stage the rewrite beside the OBJ, flush/fsync and rename only after successful
  validation/writing. A failed rewrite leaves the original bytes intact. Use the
  supplied `.obj` path on both Android and host (the old host branch appended an
  extra `.obj` after loading a different path). Release loader-owned images.

## Fixtures

The suite exercises ±X, ±Y, ±Z normals, ordinary oblique normals, near-pole normals
inside/outside the fallback band, opposite winding, zero normals, degenerate-only
and mixed-degenerate geometry, empty/missing files, nonfinite positions/normals,
malformed coordinates, overflow, and unsupported homogeneous vertex weights.

It recovers the exported rotation independently using three axis probes and
checks orthonormality, determinant +1, every pairwise distance, a single common
translation for every point, floor-centering, normal transformation, unchanged
face/material records, and finite exported coordinates. Uniform scaling (including
`1e-20` and `1e20` fixtures) and translated-input equivalence are checked. A
long/thin face explicitly verifies the existing perimeter selection policy.
The runner prints measured maximum distance and orthogonality errors.

## Read-only coordinate-consumer trace and wider follow-up

- `scanner/app/src/main/java/.../main/JNI.java` documents `optimize` as object-only.
- `scanner/app/src/main/jni/app.cc`: the JNI entry calls `App::Optimize`, which
  locks and invokes `Optimizer::Process(filename)`. It does not pass a dataset,
  update poses, or reload the scene. No new call sites are introduced here.
- `App::Save` separately creates `posesOBJ.csv` and `posesPLY.csv` through
  `common/exporter/csvposes.cc`; those paths have compass/axis conventions.
- `App::Texturize` projects the dataset before its own compass reorientation.
  `common/postproc/texturize.cc` and `common/exporter/depthmaps.cc` consume stored
  `SCREEN_CAMERA` projections. They do not consume an optimizer transform.
- The reviewed Java postprocessing path (`Main.java`) calls `JNI.texturize`;
  `Exporter.java` copies/publishes models and resources. This patch does not
  enable object normalization in either path.

Consequently, applying object normalization to a model still paired with raw
camera poses would require a wider, explicit API/data-format change. With a
shared world-frame rigid transform T, camera-to-world poses become `T * C`, and
world-to-screen matrices become `P * inverse(T)`. Existing compass/axis changes
must first be accounted for so these are composed in the same coordinate frame.
Persist that transform and test known camera projections before integrating it.
Those consumers are outside this patch's ownership and were not edited.

## Limits

The largest-perimeter rule can still choose a poor semantic orientation in noisy
scans; retaining it here avoids inventing a new scene-alignment heuristic. The
loader's triangle/quad subset and float input representation remain. Non-unit
homogeneous vertex weights are conservatively refused, not dehomogenized.
Process retains its void API and logs failure; unsupported/invalid inputs can
remain nonfinite in the original file because refusal is not data repair.
Atomic publication requires a writable parent directory and supported filesystem
operations. Host tests do not emulate Android storage or prove crash/power-loss
durability. No SLAM drift, ICP, or real-world scanning-accuracy claim is made.

NDK syntax check (real Android headers, no test stubs):

```sh
/tmp/opencode/pixelshare-sdk/ndk/25.1.8937393/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android33-clang++ \
  -std=c++11 -DANDROID -Wall -Wextra -fsyntax-only \
  -Icommon -Ithird_party/glm common/postproc/optimizer.cc
```
