# Offline mesh/export regression checks

Run from the repository root:

```sh
bash tests/mesh_quality/run.sh
```

Requires host `g++` (or `CXX`), ASan/UBSan, and `/tmp/opencode`. The runner builds
the **actual** `common/data/file3d.cc`, `common/data/mesh.cc`, and
`common/exporter/ply.cc`, using the bundled GLM. It fails on sanitizer diagnostics
and assertion failures. GL declarations and standard transitive includes reuse
the existing host-test headers. `io_stubs.cc` replaces image/codec I/O and the
recorded-frame source; it does not replace geometry parsing or serialization.

## Checked contracts

- Indexed PLY: four vertices/two triangles plus a separate triangle produce
  seven vertex records and three face records. An independent text inspector
  checks header counts, record widths, and index bounds.
- Mixed PLY attributes use one declared schema. Missing arrays get zero defaults;
  provided normals and colors survive reading. Point-cloud mode emits no faces.
- OBJ position, normal, and UV indices have independent global offsets.
  Each mesh selects its own material, including after a textured mesh and when
  different meshes reference the same texture filename.
- Positions use nine significant decimal digits. Fixtures assert exact float
  equality after OBJ and PLY round trips (maximum coordinate error zero for the
  tested values). No transform or recentering occurs in these paths.
- A four-triangle fixture exports three triangles: only its exactly zero-area
  triangle is omitted. Every retained corner is compared with the source, in
  order; a triangle with `1e-20`-unit edges and opposite-winding coincident faces
  survive. This is validity filtering, not approximate simplification.
- Malformed indices, incomplete triangle buffers, mismatched attribute lengths,
  and nonfinite output coordinates fail `WriteModel` instead of emitting broken
  geometry. Its existing flush/fsync result is preserved.
- OBJ handles signed relative indices, per-corner missing attributes, tabs,
  long records, absent material libraries, `mtllib` after vertices, and relative
  filenames. Invalid references and unsupported polygon sizes are skipped safely.
- PLY follows declared scalar-property order, consumes only declared faces,
  rejects unsupported layouts/binary input, and handles truncated/invalid faces.
  Output appended to a nonempty vector does not change existing meshes.
- Missing PLY normals are averaged only across shared **source indices**.
  Disconnected surfaces 0.0001 units apart retain different normals instead of
  colliding under the old three-decimal position key. Canceling sums fall back to
  a finite face normal.
- Reusing one `ExporterPLY` twice emits two points per run, with identical output;
  prior frames do not accumulate. Production now moves the already-copied frame
  into the merged buffer instead of making a second deep copy.

## Android compile check

The changed production translation units can also be checked against real NDK
headers (no host mocks):

```sh
/tmp/opencode/pixelshare-sdk/ndk/25.1.8937393/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android33-clang++ \
  -std=c++11 -DANDROID -Wall -Wextra -fsyntax-only \
  -Icommon -Ithird_party/glm -Ithird_party/tango_3d_reconstruction/include \
  common/data/file3d.cc common/exporter/ply.cc
```

## Limits

These checks establish file/geometry consistency, not real-world scanning
accuracy, live alignment quality, GPU behavior, codec behavior, or a whole-app
build. There is no general decimator, welding, smoothing by proximity, or
coincident-face deduplication. The loader still produces flat triangles; OBJ
supports triangles/quads with its existing quad diagonal, and PLY supports ASCII
triangle faces (or point clouds). Binary PLY, arbitrary n-gon triangulation,
additional PLY elements, and general OBJ/MTL features remain outside this subset.

The PLY normal fallback uses two sequential face passes and a contiguous sum
array instead of a string-keyed tree and per-corner strings; this is an allocation
reduction by construction, not a measured peak-RSS or throughput claim. The
exporter still holds the current run's merged point clouds until writing.

`ReadModel` retains its existing void API, so unsupported/malformed input does
not provide a structured error. `File3d` still opens write targets with truncation
in its constructor; callers needing atomic replacement must stage their output.

Alignment follow-up: `Optimizer::Process` chooses a largest-perimeter face and
rotates/recenters the OBJ, while poses/depth projections have their own coordinate
contracts. Treat any future normalization as an explicit rigid transform shared
by mesh and poses, and test known camera projections and pairwise distances.
The optimizer and live alignment code are not modified by this patch.
