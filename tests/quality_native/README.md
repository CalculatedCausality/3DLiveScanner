# Original-engine compatibility and timing checks

## Commands

From the repository root:

```sh
python3 tests/quality_native/frame_timings.py
python3 tests/quality_native/run.py
```

Use `JAVA_HOME` to select a Linux JDK, or make Java available on `PATH`.
The bridge suite also requires `g++` and the JDK's `include/jni.h` and
`include/linux/jni_md.h`. Output is built in temporary directories and removed
when each runner exits.

## Coverage

- `run.py` compiles the production JNI bridge and a host-only original-ABI
  fixture, then runs production `JNI.java` with the real JVM and `-Xcheck:jni`.
  It checks argument forwarding, foreground admission, the original save barrier,
  texturing failure/output preservation, and rejection of experimental options.
- `frame_timings.py` compiles production `FrameTimings.java`. It checks stage
  attribution, skipped native calls, mode changes, bounded ring wraparound,
  lifecycle reset, percentiles and concurrent read-only summaries.

These are boundary tests. They do not reconstruct a real scan, invoke the
original vendor texturer, prove device smoothness or establish physical accuracy.
Verify the packaged native bytes separately with `tools/check_quality_apk.py`.

See [current candidate and profiling](../../docs/quality-modernization.md).
