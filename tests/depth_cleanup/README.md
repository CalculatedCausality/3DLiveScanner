# Trained Pixel depth-cleanup experiment

**Latest:** [continued training and completed Pixel validation](FOLLOWUP.md)
produced a candidate that passes all tested detail-preservation checks on 56 fresh
scenes and runs at 0.748 ms median. The original aggregate-RMSE advantage gate
remains unmet. The sections below preserve the earlier V1/V2 results and their
then-current disconnected-device blocker; the phone has since been reconnected.

**Decision: neither candidate is enabled in the scanner.** A real trained model
ran through the explicitly selected Pixel `google-edgetpu` driver, but the quality
tests do not justify deployment. The second model's device run is additionally
blocked by the disconnected wireless-debugging endpoint.

All network code, scene generators and learned weights are original work under
the repository's Apache-2.0 license. No third-party pretrained checkpoint was
used. PyTorch is an isolated host training dependency, not an Android dependency.
No captured photographs/scans were read, no app was installed or restarted, and
no reconstruction or application source was changed in this experiment.

## Model and preprocessing

- Three SAME-padded 3x3 convolutions: `3 -> 12 -> 12 -> 1` channels; ReLU after
  the first two, linear residual head. **1,753 learned parameters**, exported as
  **1,880 bytes** of q8 weights, int32 biases and quantization descriptors.
- Fully convolutional training uses 48x48 patches; the target graph is fixed to
  120x160 for this experiment. No claim is made about matching a particular
  ARCore native depth-grid shape yet.
- Channels: `(measured depth - valid 3x3 local mean) / 0.04 m`, validity, and
  confidence. Contrast is clipped to [-1,1]. This avoids quantizing the original
  metric depth itself into coarse 8-bit depth bins.
- The scalar output is a normalized depth correction, bounded to **±20 mm** and
  added to the original floating-point measurement on eligible pixels only.
  This graph does not use the L2 operation that failed the previous diagnostic.
  That does **not** establish that the driver's L2 discrepancy is fixed.
- Eligibility requires all nine local samples, a local range below 80 mm, and
  distance of at least four pixels from the image boundary. Invalid pixels stay
  zero. V2 also preserves locally constant regions and centres whose local
  residual is smaller than half an input-quantization code. These are explicit
  preprocessing/postprocessing guards, not claims that the network learned exact
  identity. All matched-policy CPU baselines use the same guards and bound.
- The main results include an unrestricted bilateral filter separately, so a
  stricter neural guard cannot hide a stronger non-neural baseline.

Activation and weight quantization is unsigned per-tensor `QUANT8_ASYMM`.
Input/output scale is 1/128 and zero point 128; hidden ReLU tensors use zero 0
and calibration maxima plus 10% headroom. Filter scale is max-absolute-weight/127,
zero 128, with int32 bias scale equal to input scale times filter scale. A float
training phase precedes quantization-aware fine tuning. Deployment and binary
validation are described in [the NNAPI runner contract](../pixel_tpu/TRAINED_DEPTH.md).

## Data, split and acceptance

Training scenes are generated from analytic metric depth: planes, tilted planes,
curves, steps, one-to-four-pixel strips, low-contrast (25 mm) steps/strips, holes,
noise and confidence-labelled outliers. Noise parameters have not been calibrated
to the Pixel sensor. V2 includes horizontal, vertical and
oblique edges, plus separate clean sloped/curved evaluation scenes. High-noise
and common-mode-bias cases are out of the nominal training family set. Local
contrast cannot identify a constant metric bias, so the tests do not expect or
claim recovery of that unobservable error.

Training seeds start at 1,000,000; QAT uses 4,000,000; calibration uses 3,000,000.
V1's 9,000,000-series results were used as development evidence. V2 uses a fresh
9,100,000-series evaluation split with 42 scenes across 14 families. V1 and V2
overall values therefore must **not** be treated as the same-input A/B comparison.
Each final model is frozen/exported before its evaluation loop. Sources are
hashed and checked for mutation during training.

Each candidate's `acceptance.json` is written before training:

1. At least 10% lower overall RMSE than raw depth.
2. At least 5% lower overall RMSE than the best matched-policy CPU filter.
3. No scene-family RMSE regression above 2% plus 0.1 mm allowance.
4. No individual edge-RMSE regression above that allowance; thin-strip recall
   cannot fall by more than two percentage points.
5. No nonfinite results or filled invalid pixels; protected pixels are unchanged.
6. Actual TPU output must stay within 0.1 mm per-scene RMSE of host quantized
   simulation. A synthetic pass would still require separate real-sensor and
   reconstruction-level validation before app integration.

Overall RMSE is the square root of mean squared scene RMSE, giving each scene
equal weight. Reported geometric errors are synthetic ground-truth errors, not
measured physical phone accuracy. There are only three evaluation samples per
family; this is an engineering screen, not a large statistical benchmark.

## Results — 1 October 2026

### V1: real trained execution on the Pixel

1,200 float steps plus 500 QAT steps, 36 evaluation frames. Model SHA-256:
`d556f3157a063aa9cf1ac65828f07883251600cee5d57b9d9e5f2bb6fccbba37`.

| Measurement | Result |
| --- | ---: |
| Raw / strongest matched filter / actual TPU RMSE | 10.1596 / 7.6065 / 7.3872 mm |
| Improvement over raw / matched filter | 27.29% / 2.88% |
| TPU-driver median / p95 execution | 0.7918 / 1.6649 ms |
| Driver compilation / first execution | 233.51 / 14.30 ms |
| Reference CPU median execution | 26.01 ms |
| CPU/TPU maximum output difference | 2 quantization codes |
| Differences above two codes | 0 of 691,200 values |
| Clean-plane drift | 0.17785 mm RMS |

All three operations were supported by `google-edgetpu`; compilation selected
only that device. A separate run selected only `nnapi-reference`. No default
fallback was requested. This proves named-driver execution, not a hardware
occupancy measurement. Latency includes prepared-input copying, NNAPI setup,
execution and output copying; it **excludes** feature extraction, depth
postprocessing, camera acquisition, fusion and rendering. Reference CPU NNAPI is
not an optimized CPU-filter baseline.

**Rejected:** less than 5% improvement over the matched filter, and clean-plane
drift. The actual TPU depth-quality result agrees closely with host simulation.

### V2: revised guards and broader training, host evaluation only

4,000 float steps plus 1,500 QAT steps, 42 fresh evaluation frames. Model SHA-256:
`05e97d04b91b042a0aab1e746ba8ddad6320080cf3abdf8164dca62dfafce1cc`.

| Measurement | Result |
| --- | ---: |
| Raw / strongest matched filter / quantized-model RMSE | 9.5046 / 7.3888 / 7.2375 mm |
| Improvement over raw / matched filter | 23.85% / 2.05% |
| Unrestricted bilateral RMSE | 6.8201 mm |
| Clean plane / slope / curve error after guards | 0 / 0 / 0 mm |
| Failing thin-edge case, raw → model RMSE | 5.4016 → 6.3557 mm |
| Allowed edge RMSE in that case | 5.6096 mm |

**Rejected for integration:** insufficient improvement over the matched filter
and a low-contrast thin-edge regression. The gates were not relaxed to admit it.
The unrestricted bilateral filter has lower overall RMSE, but also changes
noise-free curved/sloped surfaces; both its advantage and tradeoff are retained
in the reports.

V2's device run stopped before inference because ADB reported the configured
Pixel missing. `adb devices -l` and `adb mdns services` found no device;
`adb connect 192.168.1.114:34709` returned `No route to host`. No V2 TPU timing or
quality result is claimed. Reconnect wireless debugging to complete that check.

## Reproduce

Use an isolated environment. The measured host used Python 3.10, NumPy 2.2.6 and
PyTorch 2.5.1+cpu, four training threads and deterministic Torch operations.

```sh
python3 -m venv /tmp/opencode/new-depth-venv
/tmp/opencode/new-depth-venv/bin/pip install numpy==2.2.6
/tmp/opencode/new-depth-venv/bin/pip install torch==2.5.1+cpu --index-url https://download.pytorch.org/whl/cpu
/tmp/opencode/new-depth-venv/bin/python tests/depth_cleanup/test_data.py
/tmp/opencode/new-depth-venv/bin/python tests/depth_cleanup/train.py \
  --output /tmp/opencode/new-depth-training --steps 4000 --qat-steps 1500 \
  --threads 4 --evaluation-seed 9100000
python3 tests/pixel_tpu/run_trained_depth.py \
  --model /tmp/opencode/new-depth-training/model.bin \
  --inputs /tmp/opencode/new-depth-training/inputs.bin \
  --serial "$PIXEL_SERIAL" --output /tmp/opencode/new-depth-device --repetitions 15
/tmp/opencode/new-depth-venv/bin/python tests/depth_cleanup/analyze.py \
  --training /tmp/opencode/new-depth-training \
  --tpu /tmp/opencode/new-depth-device/depth.tpu.bin \
  --cpu /tmp/opencode/new-depth-device/depth.cpu.bin \
  --output /tmp/opencode/new-depth-training/device-analysis.json
```

`analyze.py` writes `acceptance_passed:false` and explicit failures when a model
fails quality gates. Successful analysis execution does not mean acceptance.
Payloads are outside the repository; outputs are never overwritten implicitly.
The preprocessing contract test also verifies that flattening a 25 mm thin strip
is detected even though all its pixels remain valid.

Evidence directories:

- `/tmp/opencode/pixel-depth-trained-20261001/` — frozen V1 model/inputs and host/device quality.
- `/tmp/opencode/pixel-depth-device-v1-20261001/` — native reports and complete CPU/TPU outputs.
- `/tmp/opencode/pixel-depth-trained-v2-20261001/` — V2 model, inputs, host results and failed gates.
- `/tmp/opencode/pixel-depth-device-v2-20261001/` — explicit disconnected-device failure record.

Next model work must improve low-contrast edge behavior and provide a compelling
benefit over simple filters. Merely completing a faster accelerator inference is
not enough to enable it in the scanner. A real sensor validation set and end-to-end
preprocessing/inference/postprocessing measurements are still required.
