# Performance regression harnesses

These are host-side correctness and bounded-work regressions, **not FPS benchmarks
or Android/GPU validation**. They compile current production sources; no production
method is copied into a fixture. Binaries, classes and sample recordings are built
in a unique temporary directory and removed on exit. Nothing is downloaded by the
runner, and no binary dependencies are vendored.

## Run

Requires Python 3.8+. Run from any directory using the runner's path.

### Native image and scene checks

On Linux/WSL, install a C++11 compiler with AddressSanitizer/UndefinedBehaviorSanitizer
and GNU-compatible `--gc-sections` linking, then:

```sh
python3 tests/performance/run.py native
```

`CXX` defaults to `c++` and may include a wrapper, e.g. `CXX='ccache g++'`.
Sanitizers are enabled by default; `--no-sanitizers` explicitly opts out on an
unsupported host. No SDK, NDK, system GL or system image-codec development package
is required. The runner uses the repository's GLM/libpng/libjpeg-turbo headers and
the Android preprocessor branch with the small host declarations in `native/include`.
This is not an Android cross-compilation check.

### Recorder ownership and encoding checks

Provide a JDK 11+ (17 recommended), an installed Android platform jar, and the
same JCodec version used by `scanner/app/build.gradle` (currently **0.2.3**):

```sh
export JAVA_HOME=/path/to/jdk-17
export ANDROID_SDK_ROOT=/path/to/android-sdk
export ANDROID_API=33                       # default; also accepts ANDROID_HOME
export JCODEC_JAR=/path/to/jcodec-0.2.3.jar
export JCODEC_ANDROID_JAR=/path/to/jcodec-android-0.2.3.jar
python3 tests/performance/run.py recorder
```

Alternatively, set `ANDROID_JAR=/path/to/platforms/android-33/android.jar` directly.
`JAVA_HOME` may be omitted if Java is on `PATH`; the shared Java helper also accepts
the JDK compiler module when the `javac` launcher is absent. JCodec jars can come
from an existing Gradle cache or Maven Central coordinates
`org.jcodec:jcodec:0.2.3` and `org.jcodec:jcodec-android:0.2.3`. The runner fails with
an actionable message when an input is missing; it does not silently skip tests.

With those inputs set, run both suites with:

```sh
python3 tests/performance/run.py all
```

Each compile/test command is printed. Nonzero subprocess exits, assertions,
UBSan findings and timeouts fail the runner. The recorder failure-recovery test
intentionally prints one `EXPECTED: injected encoder failure` stack trace.

## Actual code and mocked boundaries

| Check | Actual code | Replaced boundary / limits |
| --- | --- | --- |
| `image_test.cc` | `common/data/image.cc` | GL types/logging and two global codec initializers are mocked. Unused codec I/O is link-eliminated. Checks 25 flip/roundtrip cases, stable pixel-buffer address, no `new[]` during flips, and 59 nearest-neighbor downscales, including odd/single-row/single-column dimensions. |
| `scene_test.cc` | `common/thread/scene.cc`, `common/data/mesh.cc`, `common/data/image.cc` | GL calls, `GLSL` and `GLRenderer` are instrumented mocks. Checks 100 alternating textured/point draws, draw order, one MVP preparation per shader/pass, refreshed uniform values, custom matrix, empty meshes, and CPU context-abandon/reupload decisions. Does not execute shaders or validate a real context. |
| `RecorderTest.java` | Full production `Recorder.java`; real JCodec `Picture`, `BitmapUtil`, file-channel code | First type-checked against the real Android/JCodec jars, with only a small surface-view stub. Runtime uses a controllable clock, array-backed bitmap, timestamp marker canvas, fake activity/audio boundary, fake GL readback and blocking/failing encoder. Checks ownership, zero readbacks across 1,000 busy calls, one bitmap/Picture across 100 frames, RGB/flip output, overlay clearing, stop/drain/restart, failure recovery, frame skipping, and 500,000 actual recorder scheduling calls against the original timestamp-loop oracle. |
| `CodecReuseTest.java` | Real JCodec encoder, conversion, muxer and demuxer | Only Android bitmap storage is mocked. Compares H.264 payload bytes, PTS and duration for 30 newly allocated versus reused input Pictures. Separate classpath excludes the fake encoder entirely. This checks the codec reuse contract, not the entire Recorder pipeline. |

The mock bitmap does not emulate Android premultiplied-alpha rounding, the canvas
does not render real fonts, and no gallery publishing/audio recording is exercised.
Android device tests and full SDK/NDK app builds remain separate checks.
