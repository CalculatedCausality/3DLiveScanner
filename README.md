# 3D Live Scanner

Android 3D scanning, shared native libraries, and desktop dataset tools.
The scanner UI is English-only.

## Current development status

The user-validated original-engine app is the quality baseline. The default
scanner build is the separate **3D Scanner Quality Test** app (`quality` flavor),
which preserves the original native binaries and uses independent storage.
Quality feedback is positive; capture smoothness is still under investigation.

- [Build variants and commands](docs/scanner-build-variants.md)
- [Quality Test status and diagnostics](docs/quality-modernization.md)
- [Restored baseline and preservation policy](docs/quality-rollback.md)
- [Documentation index](docs/README.md)
- [Test index](tests/README.md)
- [APK audit tools](tools/README.md)
- [Archived builds and evidence](artifacts/README.md)

## Repository layout

| Directory | Purpose |
| --- | --- |
| `scanner/` | Main Android scanner application and flavor-specific integrations |
| `common/` | Shared Java and native application code |
| `arcore/`, `arengine/` | AR SDK integrations |
| `reconstruction/` | Experimental owned reconstruction/texturing engine |
| `dataset_extractor/`, `dataset_viewer/` | Desktop dataset tools |
| `third_party/` | Bundled libraries and their licenses |
| `docs/`, `tests/`, `tools/`, `artifacts/` | Guides, verification suites, audits, and build evidence |

The scanner builds from `scanner/` ([original demo](https://youtu.be/ku_Slo-li3c)).
Scanner dependency versions live in `scanner/app/build.gradle`.
Desktop tools share native source lists through `common/sources.cmake`:
`dataset_extractor` exports captured point clouds as PLY; `dataset_viewer` displays
recorded datasets. Bundled-library licenses are retained in `third_party/`.

## On-device library and sharing

The scanner retains an existing writable public library. When Android scoped
storage prevents those writes, it selects writable app-owned external/internal
storage and remembers that location across launches. Legacy public scans are not
automatically moved into app-owned storage. The library screen identifies app
storage; its contents follow Android's app-data/uninstall lifecycle, so use Share
to keep copies outside the app.

The viewer and library offer local model sharing through Android's share sheet.
OBJ shares contain the selected mesh and referenced material/texture assets;
PLY shares contain the original model file. Raw datasets have a separately
labelled archive action or can be exported to a model first.

## Android build

Use the Gradle wrapper in `scanner/`. The scanner now uses Gradle 8.10.2,
Android Gradle Plugin 8.8.2, JDK 17, SDK Platform 35 and NDK r28c
(28.2.13676358). Configure `sdk.dir` in `scanner/local.properties`, then run
`./gradlew :app:assembleQualityDebug` from `scanner/`. The default quality flavor
packages the archived original native binaries with matching ARCore 1.31.0 Java
integration. It installs separately from the original app. The experimental
modern flavor uses the owned reconstruction backend and ARCore 1.56.0; its live
scan quality is not accepted. Quality and legacy are not 16 KiB-supported builds. See
[build variants](docs/scanner-build-variants.md) and the
[migration status/validation boundaries](reconstruction/MIGRATION.md).

## Verification

With a JDK (Java 8 or newer) available through `JAVA_HOME` or `PATH`, run:

```sh
python3 tests/gles/run.py
python3 common/ar/tests/test_compatibility_lifecycle.py
python3 tests/quality_native/frame_timings.py
python3 tests/quality_native/run.py
```

These checks cover GL/AR lifecycle, JNI compatibility and timing summaries using
host boundary fixtures. The JNI suite also needs Linux JDK headers and `g++`.
Use the [test index](tests/README.md) for storage, sharing, thumbnails, geometry
and experimental pipeline suites. Use the [audit tools](tools/README.md) to check
the packaged APK. Real scan quality, sustained performance and Android workflows
require the [device checks](docs/scanner-device-validation.md).
