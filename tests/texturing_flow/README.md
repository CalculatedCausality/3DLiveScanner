# Texturing integration checks

Run from the repository root:

```sh
python3 tests/texturing_flow/run.py
python3 tests/texturing_flow/check_android.py
```

For Main's supplied synthetic legacy-format dataset, optionally run:

```sh
python3 tests/texturing_flow/run.py --dataset /tmp/opencode/scanner-synthetic-dataset-16kb-20260930
```

This checks all three 64×64 JPEG444 frames against the original in-place NV21
packing loop, then runs the real wrapper's `ApplyFrames` and checks the bytes and
known translated camera poses received at the injected C API update boundary.
Every supplied fixture file is SHA-256 checked before/after each variant run.
The fixture is read-only; its analytic previews are not engine output and this
test does not claim preview/reconstruction validation.

`run.py` compiles the **actual current production definitions** of the complete
`TangoTexturize` wrapper, `Image::JPG2YUV`, `Reconstruction::InitTexturing`, and
`App::Texturize`. It uses the real API types, GLM, dataset state/pose methods,
filesystem and the repository's libjpeg-turbo encoder/decoder. ASan/UBSan and
leak detection cover frame buffers, configs, contexts and meshes. The exact
Main postprocess lambda is also extracted, compiled and executed.

**Failure-injected boundaries:** native reconstruction/texturing C API and the
new dataset-export backend (its real implementation has a separate suite),
alpha-texture I/O, Poisson, subset analysis, scan clearing, binder integration,
libc seek/read/close/write/flush/sync/rename/temp-file failures, decoder creation,
and Java JNI/service/UI objects. This does not claim backend reconstruction
quality or Android lifecycle/device coverage. No unsupported backend options
are emulated as production features. Each configuration error is propagated.

Coverage includes missing/malformed state and poses, degenerate pose matrices,
invalid/empty frame subsets, missing/bad/truncated JPEGs, dimension mismatch,
explicit rejection of 4:2:0/4:2:2/grayscale JPEGs, exact 4:4:4 packing pixels,
even widths without four-byte alignment, concurrent decoding, repeated context
initialization, backend failures, exceptions, lock release, descriptor and
alpha-image cleanup, source retention,
one/two-pass and Poisson paths, malformed/missing OBJ output, precision, normals,
legacy compass mapping, temporary-file cleanup, and failure UI/status behavior.

The default modern route calls `ExportDatasetTexturedObj` rather than flattening
the full source through the old bounded C API loader. App integration reserves a
unique working OBJ, runs texturing there, then validates and applies the existing
compass rotation before publishing the requested final OBJ. Malformed output or
write/flush/fsync/rename failures preserve an existing destination and remove the
generated working OBJ and texture resources. Success retains its resources.
The injected backend deliberately returns malformed/missing output and tracks a
resource file to test these transaction boundaries; it does not stand in for
the large-model backend's geometry, visibility or texture-quality tests.

The Java checks also verify that failed dataset reconstruction stops before
`save`/texturing, and that reconstruction and texturing failures show distinct
messages. Native export diagnostics cross a generated `jstring` JNI contract.
The modern offline backend retains binder ownership of the dataset but releases
the render mutex, pauses the camera, and blocks camera readmission until export
returns. Failure and success both reset that admission flag. Result/error access
uses its own mutex, and progress updates use the reconstruction event mutex.
`tests/control_save/lifecycle_run.py` also checks resume/surface recreation while
offline export is active; the flow suite verifies the render lock is available
inside the backend boundary and all locks/flags are released after failures.

The tested `JPG2YUV` adapter intentionally retains the legacy app-generated 4:4:4
convention: packed Y then Cr at even source x / Cb at odd source x on even rows.
No RGB conversion, averaging, orientation change or support for other JPEG
subsampling is introduced. Output capacity must be at least `width*height*3/2`.

Both `SCANNER_MODERN=0` and `=1` execute the production wrapper/App definitions.
The retained modern C API path requests CPU backend 0 and simplification 1 (no
decimation); legacy retains the configured factor/default 10 and fast-pass 5.
Modern explicit atlas requests outside size 16–4096, count 0–8, or 16,777,216
RGBA texels (64 MiB) return false with a resource-budget diagnostic before config
creation. Count 0 remains 0 for the backend's bounded automatic selection.
The injected modern C API also rejects extraction before any frame update, so
the Poisson/two-pass preparation paths must submit real decoded frames first.
Legacy option tests verify pass-through, not that the proprietary SDK accepts
arbitrary values. Backend implementation and quality remain separately tested
by `tests/reconstruction/texturing_run.py`. The default large-dataset path is
covered separately by `tests/dataset_texturing/` and `tests/export_device/`.

`check_android.py` syntax-compiles all four affected native translation units
against real arm64 NDK/ARCore/Tango/OpenCV/codec headers in C++11 mode. It compiles
the entire JNI.java against Android SDK 35 (generated R constants only), checks
the generated JNI boolean signature and compiles the new resource with aapt2.
Set `ANDROID_SDK_ROOT`/`ANDROID_NDK_HOME` to override the installed defaults.
Both scripts honor `JAVA_HOME` (or a JDK on PATH); the local fallback is
`/tmp/opencode/scanner-jdk17`.
This is an API compile check, not APK linking, device testing or 16-KiB ELF
verification. All generated outputs use isolated temporary directories.
