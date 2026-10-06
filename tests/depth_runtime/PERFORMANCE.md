# Preprocess row-cache optimization

## Scope and numerical contract

Only `Preprocess` in `common/depth/experimental.cc` changes. Cache seven source
rows of 7-wide/3-wide sums and counts plus 3-wide min/max. Compute each horizontal
sum **from zero, left to right**, once per source row, and add rows in the original
`dy=-3..3` order, including the original zero additions to the 3-wide sum. Clamped
edge rows/columns retain their repeated contributions. This is a row cache, not an
incremental/subtractive rolling sum. Median, feature arithmetic, quantization and
eligibility expression retain their original implementation.

## Reproduce

Before editing the source, freeze its `.cc` and headers:

```sh
python3 tests/depth_runtime/performance.py --freeze
# Use the printed directory as BASELINE; never freeze the optimized source as baseline.
BASELINE=/tmp/opencode/depth-preprocess-before-385ik0fd
python3 tests/depth_runtime/run.py
SANITIZE=1 python3 tests/depth_runtime/run.py
python3 tests/depth_runtime/performance.py --baseline "$BASELINE"
python3 tests/depth_runtime/performance.py --baseline "$BASELINE" --sanitize

NDK=/tmp/opencode/pixelshare-sdk/ndk/28.2.13676358
ANDROID_NDK_HOME="$NDK" python3 tests/depth_runtime/build_android.py
python3 tests/depth_runtime/performance.py --baseline "$BASELINE" --android-ndk "$NDK"
# Optional explicit device run: use the preceding command's artifact directory.
python3 tests/depth_runtime/performance_device.py /tmp/opencode/depth-preprocess-perf-66s32df_ \
  --adb /tmp/opencode/pixelshare-sdk/platform-tools/adb --device 192.168.1.114:33123
```

Both benchmark variants compile the complete source independently with C++11,
`-O2`, warnings-as-errors and no fast-math. The host uses g++; native uses NDK28.2
arm64 API24 clang++ with static libc++, `dl` and `log`. The existing test-only fake
NNAPI is reused through temporary copies of `runtime_test.cc`; its original file
is untouched. The benchmark never performs real NNAPI inference.

The 310-case corpus includes the original 90 NumPy oracle cases plus 220 seeded
cases: smooth noise, holes, zeros/signed zeros/subnormals, depth/confidence limits,
random depths, tiny/asymmetric dimensions, 1×1024/1024×1, 160×90, 160×120,
320×240, 320×480, 512×512, 1024×256 and 256×1024. Comparison serializes every
input/mask byte and every x/y/z/w output float for both synthetic correction
directions, plus eligible/changed/inferred stats. Includes unmapped points,
invalid-confidence originals, camera admission and per-link minimum-depth cases.
Timings/status text are excluded from byte equivalence. The fake output depends
on input feature bytes and exercises both correction signs.

Benchmarks use generated dense and hole-containing depth/confidence frames.
Timing encloses **Preprocess only**, including median, allocations, feature
generation and masks; the returned vectors' destruction and all comparisons,
serialization and hashing happen outside the timed region. Each shape has one
warm-up, then 8 measured host calls or 2 measured device calls. ABBA runs baseline,
current, current, baseline. Reported aggregates average the two per-run medians.
Every timed result is compared to its warm-up result; every serialized benchmark
byte is also compared between baseline/current (all four runs on the device).

The explicit device runner creates a unique `/data/local/tmp` directory, pushes
two executables, performs four short synthetic runs and pulls their output bytes,
then removes only that directory. No app install/restart, scan reads, camera,
settings changes, affinity or governor changes. Per-command timeout is 20 seconds.

## Verification and memory (2026-10-01)

- Normal and ASan/UBSan runtime contracts plus all **90/90** NumPy oracle cases pass.
- Normal and ASan/UBSan frozen-source comparison: **310/310** cases;
  **184,101,692 bytes identical**, **2,257,925 changed points** across both signs.
  Serialized SHA256: `dce6f9b8fd95e688030dc5f1d93b455ff8a9a9f98e9d94af5993bf5da052e991`.
- Device ABBA: all **2,980,352 feature/mask bytes identical** across all four runs.
  SHA256: `1d66582c3198492901bb72ff3cbec3f37481f8d597f1588117d26377451ac4f6`.
- NDK28.2 API24 ABI/dependency checks pass; no mandatory NNAPI library dependency
  or undefined NNAPI symbols. Runtime retains dynamic loading.

Exact additional vector **heap payload**, excluding allocator metadata/stack/RSS:
`24 * width * min(height, 7)` bytes, one additional allocation per call. Each row
entry is four 32-bit floats and two 32-bit ints on the tested host/arm64 ABIs.
Existing input/mask/median capacity totals `8 * width * height` bytes.

| Shape | Baseline payload | Current payload | Added payload |
|---|---:|---:|---:|
| 160×90 | 115,200 B | 142,080 B | **26,880 B** |
| 160×120 | 153,600 B | 180,480 B | **26,880 B** |
| 320×240 | 614,400 B | 668,160 B | **53,760 B** |
| 512×512 | 2,097,152 B | 2,183,168 B | **86,016 B** |

Worst added payload under existing guards is **172,032 B** at width 1024 and
height ≥7. No frame-to-frame retained cache. These are source-derived exact
payload deltas, not measured process RSS.

## Host component timings

Linux x86-64, Ubuntu g++ 11.4.0, ABBA with 8 measured calls per case per run.
The final run was after the sanitizer/native build jobs completed.

| Shape / input | Baseline ms | Current ms | Delta ms | Reduction | Speedup |
|---|---:|---:|---:|---:|---:|
| 160×90 dense | 1.432503 | 0.844535 | -0.587968 | 41.04% | 1.696× |
| 160×90 holes | 2.499402 | 0.832070 | -1.667332 | 66.71% | 3.004× |
| 160×120 dense | 1.887939 | 1.143101 | -0.744838 | 39.45% | 1.652× |
| 160×120 holes | 3.346483 | 1.065743 | -2.280740 | 68.15% | 3.140× |
| 320×240 dense | 7.725627 | 4.713508 | -3.012119 | 38.99% | 1.639× |
| 320×240 holes | 12.941367 | 4.905663 | -8.035704 | 62.09% | 2.638× |
| 512×512 dense | 26.386330 | 17.024175 | -9.362156 | 35.48% | 1.550× |
| 512×512 holes | 45.897171 | 16.289366 | -29.607806 | 64.51% | 2.818× |

Host/native generated benchmark inputs are compared within each toolchain, not
assumed identical across toolchains (the generator's float multiply/add can be
contracted on arm64). Both builds independently compare baseline/current bytes.

## Native component timings

Device `192.168.1.114:33123`, brief unpinned ABBA (whole runner completed in about
4.4 seconds including transfers). **These are component timings, not scanning
FPS.** Active-device scheduling/DVFS is uncontrolled; particularly the first
160×90 dense baseline was 10.725566 ms versus 5.484172 ms in the last baseline.
The table reports the specified aggregate without hiding that variability.

| Shape / input | Baseline ms | Current ms | Delta ms | Reduction | Speedup |
|---|---:|---:|---:|---:|---:|
| 160×90 dense | 8.104869 | 3.713439 | -4.391430 | 54.18% | 2.183× |
| 160×90 holes | 5.016174 | 2.884114 | -2.132060 | 42.50% | 1.739× |
| 160×120 dense | 4.689025 | 3.546081 | -1.142944 | 24.37% | 1.322× |
| 160×120 holes | 4.379405 | 2.749796 | -1.629609 | 37.21% | 1.593× |
| 320×240 dense | 10.487996 | 9.418335 | -1.069661 | 10.20% | 1.114× |
| 320×240 holes | 9.426534 | 6.331249 | -3.095286 | 32.84% | 1.489× |
| 512×512 dense | 19.627727 | 16.018616 | -3.609110 | 18.39% | 1.225× |
| 512×512 holes | 20.975860 | 12.193187 | -8.782673 | 41.87% | 1.720× |

## Source identity and artifacts

SHA256:

| File | Frozen baseline | Current |
|---|---|---|
| `experimental.cc` | `4e58db3076d37fe5d95d26bc50bac2899ce957d05195a498fbcef75b62a53313` | `e6f268981436a080c487e357c02ce03a55e629ea3869377914e5969923d11d59` |
| `experimental.h` | `da5594f5d816e74fb36f90ee23ef8773c42d30e94bac0e99b7c79ccb86d333a6` | same |
| `model_data.h` | `aa3b4bd3c8d3bd5c7aacb9b33f42e6ee5328e95ffe1d5c673617990b8a998709` | same |

- Frozen source/all depth headers: `/tmp/opencode/depth-preprocess-before-385ik0fd`;
  `sha256.json` records every frozen file.
- Sanitized full comparison: `/tmp/opencode/depth-preprocess-perf-dxv0h19z`.
- Final normal full comparison/host ABBA: `/tmp/opencode/depth-preprocess-perf-vbhe8v70`.
- Native binaries/raw ABBA logs/byte outputs: `/tmp/opencode/depth-preprocess-perf-66s32df_`.
- API24 smoke build/dependency audit: `/tmp/opencode/depth-runtime-api24-r3z0uq_m`.
- Normal oracle artifacts: `/tmp/opencode/depth-runtime-nvmbfqwk`;
  sanitized oracle artifacts: `/tmp/opencode/depth-runtime-yehylafp`.

Each performance build records source hashes and keeps generated corpus, binaries
and comparison output under `/tmp/opencode`. The comparison runner also asserts
that source outside `Preprocess`, the public header and embedded model match the
frozen baseline.
