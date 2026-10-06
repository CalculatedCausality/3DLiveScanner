# Pixel depth-network accelerator experiment

Standalone research tools, independent of the scanner build. See
[`docs/pixel-accelerators.md`](../../docs/pixel-accelerators.md) for findings and
the model/license shortlist. New probe code is covered by the repository's
Apache-2.0 license. It contains no third-party model implementation or weights.

## Reproduce

From the repository root, with the existing Android SDK/NDK and Python 3:

```sh
python3 tests/pixel_tpu/run_probe.py \
  --serial 192.168.1.114:34709 \
  --output /tmp/opencode/pixel-tpu/my-run
```

The output directory must not exist. Optional flags: `--sdk`, `--ndk-version`,
`--repetitions` (default 15, plus 3 warmups), and repeated `--case decoder:q8`.
The runner builds with NDK r28c, API 29, C11, `-O2 -Wall -Wextra -Werror`, and
16 KiB ELF page alignment. No JDK, Gradle, root, APK or app dependencies needed.
Each case has a 90-second remote timeout. Only a unique
`/data/local/tmp/pixel-tpu-<uuid>` directory is uploaded/removed. The tools read
device identity and use shell NNAPI; they never access a camera or scan directory.
They do briefly compete for system compute resources, so avoid overlapping with
a controlled scanner performance measurement.

**The full suite currently exits 1 on this Pixel**: `decoder_l2:q8` runs but fails
the fixed two-code numerical tolerance (binary exit 4). This failure is retained,
not converted into a passing test. Unsupported graphs are explicitly recorded
and skipped for that device; those skips are not accelerator successes. A
supported graph that cannot compile/execute also fails the runner.

```sh
# NumPy is needed only for numerical analysis, not building/running the probe.
python3 tests/pixel_tpu/analyze_outputs.py /tmp/opencode/pixel-tpu/my-run

# Independently download public artifacts and inspect actual MiDaS TFLite tables.
# Standard library only; about 67 MB; no model execution or pickle loading.
python3 tests/pixel_tpu/inspect_models.py \
  --output /tmp/opencode/pixel-tpu/my-model-inspection
```

## Files and evidence

- `depth_ops.c`: model construction, exact-device support queries/compilation,
  timings, nonconstant synthetic inputs, and full final-output CPU comparison.
- `run_probe.py`: builds/runs all 15 cases; persists compiler command, source and
  executable SHA-256, device fingerprint, support booleans, per-call latency
  samples, status, stderr, and cleanup status in `results.json` and per-case logs.
  For q8 `l2`, `decoder`, and `decoder_l2`, saves the final CPU/TPU NHWC outputs as
  raw uint8 `.bin` files (120×160×3 = 57,600 bytes each).
- `analyze_outputs.py`: hashes and compares those buffers; emits
  `numerical-analysis.json`. Reported vector angles are **cross-device agreement**,
  not surface-normal accuracy against a real scene.
- `inspect_models.py`: pins and hashes NConv sources/checkpoint and MiDaS release;
  reads FlatBuffer I/O, tensor types, quantization parameters and operator versions.
  Downloads remain exclusively in the selected scratch directory. License files
  are retained beside them. The PyTorch checkpoint is never deserialized.
- `evidence-2026-10-01.json`: compact checked-in record of the final run and model
  inspection; raw samples and buffers remain in the scratch paths it identifies.

## Interpretation

The decoder uses spatially/channel-varying, deterministic diagnostic kernels;
it is **untrained**, not a depth-cleanup model. Inputs contain a ramp, a step and
an invalid-depth stripe; standalone L2 also includes negative components and
zero vectors. The convolution deliberately spreads information across gaps.
Thus no depth RMSE, edge-preservation, gap-fill or normal-quality claim is valid.

Latency includes input memcpy, execution creation, input/output binding,
synchronous NNAPI compute (including runtime/driver transfers), output memcpy,
and execution destruction. It excludes scene generation/quantization, graph
construction/compilation, ADB transfer, model loading, app scheduling and scanner
postprocessing. Compilation and first-call times are reported separately.
Only the final iteration is numerically compared. These are short, sequential,
uncontrolled-clock shell runs; the reference CPU is not an optimized LiteRT CPU
baseline. Driver selection is proven; physical hardware occupancy/internal
driver implementation is not measured.
