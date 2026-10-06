# Fixed trained-depth NNAPI runner contract

`trained_depth.c` and `run_trained_depth.py` consume exported quantized parameters
from host training. They construct exactly three ordinary `CONV_2D` operations:

```
NHWC [1,120,160,3]
  -> 3x3, 12 output channels, stride 1, SAME, fused ReLU
  -> 3x3, 12 output channels, stride 1, SAME, fused ReLU
  -> 3x3,  1 output channel,  stride 1, SAME, FUSED_NONE
```

The result is a **normalized residual**, decoded as
`(output_code - output_zero) * output_scale`. There is no L2 normalization,
residual addition, resizing, masking, clipping to depth ranges, or unit conversion
in the runner. Training owns input-channel meanings, normalization, calibration,
residual-to-depth conversion and quality evaluation. FP32 weights stay on the
training host. File contents do not prove that a model was trained; every report
therefore says `quality_claim: not_evaluated`.

## DCLN0001 model file (exactly 1,880 bytes)

All multibyte values are **little-endian**, with no alignment padding, dimensions,
counts, metadata or trailing bytes in the file. Floats are IEEE-754 binary32;
integers are signed two's-complement int32.

| Offset | Content | Bytes |
| ---: | --- | ---: |
| 0 | ASCII magic `DCLN0001` | 8 |
| 8 | Input activation: float32 scale, int32 zero point | 8 |
| 16 | Hidden 1 activation: float32 scale, int32 zero point | 8 |
| 24 | Hidden 2 activation: float32 scale, int32 zero point | 8 |
| 32 | Output activation: float32 scale, int32 zero point | 8 |
| 40 | Layer 1 weight scale | 4 |
| 44 | Layer 1 uint8 weights, OHWI `(12,3,3,3)` | 324 |
| 368 | Layer 1 int32 biases, `(12,)` | 48 |
| 416 | Layer 2 weight scale | 4 |
| 420 | Layer 2 uint8 weights, OHWI `(12,3,3,12)` | 1,296 |
| 1,716 | Layer 2 int32 biases, `(12,)` | 48 |
| 1,764 | Layer 3 weight scale | 4 |
| 1,768 | Layer 3 uint8 weights, OHWI `(1,3,3,12)` | 108 |
| 1,876 | Layer 3 int32 bias, `(1,)` | 4 |

Each activation is per-tensor unsigned `QUANT8_ASYMM`, with its own positive
finite scale and zero point in `[0,255]`. There is no prescribed activation
scale or zero. Each weight tensor is also unsigned `QUANT8_ASYMM`, with fixed
zero point **128**. The exporter quantizes symmetric signed weights using
`weight_scale = max(abs(float_weights)) / 127` per layer and stores signed codes
offset by 128 (normally codes 1 through 255). A completely zero weight tensor
needs an exporter-chosen positive scale. Bias zero is 0 and bias scale is exactly
`float32(input_activation_scale * weight_scale)`; the file contains int32 bias
codes, not float biases. No per-channel quantization. OHWI order means I changes
fastest, followed by W, H, O; transpose host OIHW tensors accordingly.

Both loaders reject wrong magic/length, nonfinite or nonpositive scales, zero
points outside `[0,255]`, and float32 bias-scale products that overflow or
underflow to zero. The native loader validates all model and input bytes before
constructing a graph. It cannot verify the exporter's `maxabs/127` derivation
without the original FP32 weights. It accepts the full uint8 storage range.

## Input and output files

- Input: concatenated uint8 NHWC `[120,160,3]` frames, exactly **57,600 bytes per
  frame**, 1–64 frames (maximum 3,686,400 bytes), with no header or padding.
- Outputs: `<prefix>.tpu.bin` and `<prefix>.cpu.bin`, each exactly
  `frame_count * 19,200` bytes, uint8 `[120,160,1]` frames in original order.
- Inputs must be regular files. Empty, oversized and partial frames are rejected.
- Native report: `<prefix>.report.jsonl`; records are also emitted on stdout.
  The prefix must be new. Output/report files are created exclusively, preventing
  accidental reuse of stale evidence. A device output is published only after
  every frame for that device executed successfully; check the `output` record
  and run status before using a file.

Native invocation:

```sh
trained_depth MODEL INPUT OUT_PREFIX [repetitions]
```

Repetitions default to 5 and must be 1–100. Exit codes: 0 = execution and output
success; 2 = argument/file/format/allocation failure; 3 = missing named device or
unsupported graph; 4 = NNAPI API, compilation or execution failure. Quantized-code
differences alone never cause failure. Even a model rejected before graph
construction produces an error report when the output prefix is writable.

## Explicit-device execution and measurement

Only `google-edgetpu` with NNAPI accelerator type and `nnapi-reference` with CPU
type are eligible. Every operation must report support on each selected device.
The runner aborts an unsupported accelerator graph and retains the report.
Each compilation uses `ANeuralNetworksCompilation_createForDevices` with exactly
one device and `PREFER_FAST_SINGLE_ANSWER`. No default-device compilation, relaxed
FP32 graph, or fallback path is requested. Driver name, version, type and feature
level are recorded. Selection proves the requested NNAPI driver, not physical
hardware occupancy inside that driver.

Execution is sequential, TPU then reference CPU. For each device:

1. Report compilation wall time and status.
2. Run **three warmups on frame 0**. Warmup 0 is the first call; report all three
   individual calls, first-call latency and summed warmup time.
3. Run the requested timed repetitions on the **same unchanged frame 0**. Record
   each sample and nearest-rank p50/p95 of end-to-end latency.
4. Execute **every input frame once more**, recording its individual latency and
   collecting all outputs in order. These calls, not benchmark scratch outputs,
   supply the saved buffers and comparisons.

Each `call` record reports status, phase/index/input frame, input memcpy time,
`nnapi_ms` (execution creation, binding and synchronous compute), output memcpy
time, and end-to-end time (all of those plus execution destruction and timing
overhead). Warmup/benchmark logging is outside the measured interval. Compilation,
model/input reading, quantization, ADB, output-file writing and downstream depth
processing are excluded. These short shell runs have uncontrolled clocks and
thermal state; `nnapi-reference` is not an optimized application CPU baseline.

Per-frame and aggregate (`frame: -1`) comparisons report integer-code unequal
counts, count above two codes, max/mean absolute difference, mean signed
TPU-minus-CPU difference, and each device's output range. The two-code statistic
is descriptive, **not an acceptance threshold**. Depth-quality gates belong to
main's real trained-model evaluation.

## Host runner

```sh
python3 tests/pixel_tpu/run_trained_depth.py \
  --model /tmp/opencode/training/model.bin \
  --inputs /tmp/opencode/training/inputs.bin \
  --serial 192.168.1.114:34709 \
  --sdk /tmp/opencode/pixelshare-sdk \
  --output /tmp/opencode/trained-depth-real-run \
  --repetitions 5
```

`--model`, `--inputs`, `--serial` and `--output` are required. `--sdk` defaults to
the path above; repetitions default to 5. Output must be a **new directory outside
the workspace**, resolving symlinks. Standard-library Python only. No execution
starts without valid supplied model/input files; it does not search for training
artifacts or generate substitutes.

The build pins NDK **28.2.13676358**, arm64 API 29, C11,
`-O2 -Wall -Wextra -Werror -Wno-deprecated-declarations` and 16 KiB ELF page
alignment. Output retains snapshots of native/Python sources and model/input
files, executable, build logs, native stdout/stderr, native JSONL, raw outputs,
and `results.json`. The manifest contains SHA-256 and byte counts for sources,
executable, model, inputs and successful outputs; build command/compiler version,
ADB version, Android build/device identity and NNAPI driver identity are included.

Only the executable and two supplied binary artifacts are uploaded into a unique
`/data/local/tmp/trained-depth-<uuid>` directory. The native process has a remote
180-second kill timeout and the host ADB call a 195-second timeout. Cleanup of
that exact scratch directory is attempted in `finally`, with outcome retained.
Disconnected-device cleanup failures are explicit, not silently declared clean.
Reports are retained for unsupported graphs, driver failures, invalid files,
build failures and timeouts. The tools do not read scans/images, install/restart
apps, or integrate with reconstruction.

## Implementation smoke versus trained quality

Deterministic generated quantized weights/inputs may test parsing, graph support,
execution, frame ordering and cross-device agreement. Such evidence must be
labeled **implementation smoke, not trained quality**. It establishes neither
depth accuracy nor a useful residual predictor. Real weights/input fixtures and
quality metrics are produced outside this runner by main. See the delivered
scratch smoke report for the exact sources, artifacts, identities and timings.

### Implementation smoke evidence — 2026-10-01

**Untrained deterministic parameters only.** Artifacts and the generator are in
`/tmp/opencode/trained-depth-implementation-smoke/`. The primary host report is
`run/results.json`; additional contract-check evidence is in
`validation/results.json`. The generated three-frame fixture uses activation
scales `0.007, 0.011, 0.009, 0.005` and zero points `83, 29, 17, 123`, signed
spatial/channel-varying kernels, nonzero biases, and input frames A, B, A.

On device `192.168.1.114:34709`, all three operations reported support on both
`google-edgetpu` (driver 2.0) and `nnapi-reference` (driver 16091614).

| Device | Compile ms | First call ms | Repeated p50 ms | Repeated p95 ms |
| --- | ---: | ---: | ---: | ---: |
| google-edgetpu | 344.071 | 7.820 | 0.415 | 0.821 |
| nnapi-reference | 0.171 | 74.885 | 22.933 | 27.448 |

There were three warmups and five repeated samples per device. Each output file
contains 57,600 bytes (three complete residual frames). Across all outputs, 808
codes differed, maximum absolute difference was **1 code**, and mean absolute
difference was **0.014028 codes**. Both outputs ranged from 105 to 131, crossing
the output zero point 123. A independently repeated after B produced the same
bytes, and B differed from A on both devices.

Additional checks passed:

- **49 native rejection cases**, each with exit 2 and a retained error report
  before any graph record: malformed magic/length/trailing bytes, every scale's
  NaN/infinity/zero/negative values, each zero point's lower/upper violation,
  bias-scale overflow/underflow, empty/partial/65-frame input, and invalid
  repetition arguments. Applicable cases also passed the host rejection checks.
- The **64-frame maximum**, default five repetitions, exact 1,228,800-byte
  output files per device, and all 64 frame positions matched the three-frame
  run's corresponding outputs.
- An independent NumPy int64 OHWI/SAME convolution calculation, including
  per-layer quantization and fused activations, agreed with reference CPU output
  within one code. This is an implementation check, not a depth-quality metric.
- Both remote scratch directories were cleaned successfully (exit 0).

These results establish runner/contract behavior for this generated fixture.
Real trained-model support and residual/depth quality must be measured separately.
