# Original-engine modernization and frame pacing

## Protected comparison

The restored original app remains `com.lvonasek.arcore3dscanner`, with APK
SHA-256 `0b1e81ecfcaabcf04494de38ff98754b1ca0e4abf7d99cdfdebb9476b1f51d0e`.
The user verified that its scan quality returned. See [rollback](quality-rollback.md).

The default `quality` flavor installs separately as
`com.lvonasek.arcore3dscanner.quality`, labelled **3D Scanner Quality Test**.
Capture scratch is `files/capture-dataset`; its library is `files/quality-scans`,
both inside that package's private data directory. It does not migrate or
enumerate the baseline's library.

All nine native libraries come from the exact validated APK. Gradle verifies its
SHA-256 and matches ARCore 1.31.0's JNI SDK before packaging Java from that SDK.
The small JNI bridge adapts changed signatures, gates draw/pause lifecycle,
uses the original save barrier, and checks texturing output. It passes only JNI
primitives/arrays, not native C++ objects, pixels, points or poses between engines.
TPU correction and reduced-detail preview are disabled. Defaults match the
successful 2 cm / 4 m / clearing-on / offset-on profile.

These original libraries are not claimed to support a 16 KiB kernel. The tested
Pixel uses 4 KiB pages. ZIP alignment does not establish runtime compatibility.

## Evidence so far

First candidate: `artifacts/3DLiveScanner-quality-isolated-debug-2026-10-01.apk`,
SHA-256 `70d06bac1170e3067bd5aa77e6d7ffcc00277f7d150b55a0583136e6e2731bbe`.
User feedback: **“The same quality feels stuttery.”** This is positive quality
feedback, not an accepted performance result or a completed save/export test.

Read-only analysis of its 131 committed timestamp files found:

| Measurement | Value |
| --- | ---: |
| First-to-last capture span | 31.497777 s |
| Average capture cadence (130 intervals / span) | 4.127 frames/s |
| Mean capture interval | 242.291 ms |
| Median interval | 236.693 ms |
| 95th-percentile interval (nearest rank) | 305.050 ms |
| Maximum interval | 374.549 ms |

`state.txt` was unchanged before and after reading the timestamp-only archive.
No images or geometry were copied. Capture timestamps measure accepted input
cadence, **not displayed preview FPS**; motion selection and tracking can also
affect which frames are captured.

Earlier Android `gfxinfo` showed 715 UI frames, 13 janky (1.82%), 5 ms median,
8 ms p95, and 57 ms p99. Those figures concern the Android UI renderer, not the
separate scanner OpenGL surface. They cannot explain or disprove preview stutter.
The app was subsequently backgrounded, so those historical totals are not a
controlled timing window for a fresh capture.

Separate storage probes established that the private workspace can write faster,
but did not measure a whole-app speedup. App-UID serial private-write means were
6.260/5.608 ms. A shell-UID ABBA probe measured shared means 135.949/120.445 ms
versus internal 12.598/9.716 ms. The different UIDs must not be combined into a
single controlled speedup claim. Raw evidence:
`/tmp/opencode/scanner-build-verification/quality-storage-20261001.log`.

## Debug-only pacing diagnostics

The archived `3DLiveScanner-quality-pacing-debug-2026-10-02.apk` preserves the
engine and frame scheduling. Installation is pending a working device connection.
`FrameTimings` is enabled only by `qualityDebug`. Other flavors and release
leave the GL observer null. It retains at most 256 numeric samples, with no
per-frame logs, disk writes or allocations. Summaries copy under a short lock
and calculate percentiles outside it.

- `native_draw_ms`: elapsed time around the JNI draw call, including the bridge
  lifecycle gate. It does not yet subdivide internal camera, fusion or rendering.
- `callback_ms`: complete Java renderer callback, including waiting to enter
  `Main.onDrawFrame`'s monitor and native drawing.
- `java_other_ms`: callback minus JNI draw time; includes remaining native
  status calls, Java work and any monitor wait.
- `egl_swap_ms`: CPU time waiting in the buffer-swap call, not GPU execution time.
- `start_interval_ms`: start-to-start GL callback interval within one mode.
- `outside_frame_ms`: previous swap return to the next callback start, covering
  scheduling and loop work outside callback/swap.

Groups distinguish capture, preview, and all callbacks. `frames_total` counts
callbacks/swap attempts since reset, **not frames actually presented**. Swap
errors are counted. Mode transitions and the first frame after an activity
resume do not produce interval samples. Missing native calls are not reused
from earlier frames. `last_frame_age_ms` exposes stale results; a new process
without an activity has an empty summary.

Query while the candidate is running or shortly after a scan:

```sh
adb shell am broadcast \
  -a com.lvonasek.arcore3dscanner.quality.QUALITY_STATUS \
  -n com.lvonasek.arcore3dscanner.quality/.QualityStatusReceiver
```

This DUMP-permission-protected debug receiver is read-only and never starts a
camera. Query at the end of a sustained scan before stopping capture so the
recent ring contains capture frames. A restart loses these in-memory timings.

If native draw dominates, inspect its internal scheduling separately. If Java
overhead, swap, or outside-frame gaps dominate, target that measured stage.
Repeated UI postings and periodic UI-thread resource queries are candidates,
not established causes. No geometry policy change is justified by these data.

## Build, checks and acceptance

```sh
python3 tests/quality_native/frame_timings.py
python3 tests/quality_native/run.py
# From scanner/, with JDK 17 and the documented Android SDK:
./gradlew :app:assembleQualityDebug --max-workers=2
# From the repository root:
python3 tools/check_quality_apk.py scanner/app/build/outputs/apk/quality/debug/app-quality-debug.apk
```

See [test coverage](../tests/quality_native/README.md) and
[package audit requirements](../tools/README.md). The Linux output init script used locally redirects
the APK to `/tmp/opencode/scanner-modern-integration/app/outputs/apk/quality/debug/`.

Update only the candidate after the user confirms its pending scan is safe.
Keep the baseline installed and verify its APK identity. Then obtain stage
timings during a fresh out-and-back capture and compare smoothness as well as
quality. Full save, reopen, texturing and export acceptance remain outstanding.

Any future TPU work must demonstrate complete-pipeline benefit against a CPU
alternative at the same quality, including input packing, synchronization and
output transfer. Existing synthetic denoising and slower small-operation TPU
offload do not meet that requirement. Training is separate from phone inference.
