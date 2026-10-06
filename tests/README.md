# Test index

Run suite commands from the repository root unless a suite says otherwise.
Each suite owns its fixtures and runners. Keep generated binaries, device captures
and private evidence outside the source tree; runners use temporary directories.
Java host runners reuse `java_tools.py` for `JAVA_HOME`/PATH discovery and the
compiler-module fallback when the `javac` launcher is absent.

## Current original-engine candidate

| Suite | Coverage |
| --- | --- |
| [Quality native](quality_native/README.md) | JNI compatibility bridge and bounded frame-timing summaries |
| `gles/` | Production GL-thread/EGL lifecycle against host boundary doubles |
| [Storage](storage/README.md) | Workspace selection and checked publication |
| [Sharing](sharing/README.md) | Share bundles and permissions |
| [Thumbnails](thumbnails/README.md) | Bounded loading, caching and ownership |
| `model_lookup/` | Library model lookup |

Quick focused checks:

```sh
python3 tests/quality_native/frame_timings.py
python3 tests/quality_native/run.py
python3 tests/gles/run.py
python3 common/ar/tests/test_compatibility_lifecycle.py
```

The JNI suite needs a Linux JDK with JNI headers and `g++`; the timing and GL
checks need Java compilation/runtime support. Consult each runner for additional
requirements. These host checks do not execute the complete original vendor
capture, texturing or export workflow.

## Shared and experimental pipeline checks

| Area | Suites |
| --- | --- |
| Capture | [I/O](capture_io/README.md), [Speed](capture_speed/README.md), [Preview](capture_preview/README.md), `capture_policy/`, `depth_capture/` |
| Geometry | [Geometry](geometry/README.md), [Alignment](alignment/README.md), [Mesh quality](mesh_quality/README.md), `reconstruction/` |
| Processing and export | [Texturing flow](texturing_flow/README.md), `dataset_texturing/`, [Export device](export_device/README.md), [Control/save](control_save/README.md) |
| Depth and accelerators | [Depth cleanup](depth_cleanup/README.md), [Depth runtime](depth_runtime/README.md), [Pixel TPU](pixel_tpu/README.md) |
| Platform and performance | [Page alignment](page_alignment/README.md), [Performance](performance/README.md) |

Some suites test the experimental reconstruction engine, which is not compiled
into the quality flavor. Passing them does not authorize replacing the validated
original app or imply real-device quality/performance acceptance.

See [build variants](../docs/scanner-build-variants.md),
[package audits](../tools/README.md), and
[device-validation requirements](../docs/scanner-device-validation.md).
