# Pixel TPU feasibility and depth-network experiments

**Live-test update:** at the user's explicit request, the model is now installed
and enabled as an [experimental live mode](experimental-tpu-test.md). Actual
Pixel capture logs confirm TPU processing at native 160x90 and committed input
backups. Historical research approval/failure statements below remain valid;
experimental testing is not a claim of production approval.

## Latest: actual trained residual model

**Update:** the continued study and device run are now complete. The final
balanced-edge candidate ran through `google-edgetpu` at **0.748 ms median** and
passed the detail-preservation checks on **56 fresh synthetic scenes**, reducing
depth RMSE from **8.804 to 6.540 mm**. The original aggregate-error advantage gate
still fails against a filter whose own detail checks fail. This is a promising
quality tradeoff, not production approval or a real-sensor accuracy claim.
See [the full follow-up](../tests/depth_cleanup/FOLLOWUP.md) and
[production integration findings](pixel-depth-integration.md).

Earlier, an original three-convolution, 1,753-parameter depth-correction model was
trained, quantized and executed with only `google-edgetpu` selected. Its median
prepared-input execution was **0.792 ms**, and actual accelerator outputs reduced
synthetic depth RMSE from **10.160 to 7.387 mm**. This is trained-model evidence,
distinct from the untrained operation probes below.

It is **not enabled in the scanner**: it improved on the strongest matched-policy
filter by only 2.88% and shifted a clean plane. A revised model removes that
clean-surface drift through explicit guards, but still fails the filter-benefit
and low-contrast thin-edge gates. The revised model's device test is blocked by
disconnected wireless debugging. No real-sensor accuracy or app-FPS gain is claimed.

See [training, held-out quality gates and evidence](../tests/depth_cleanup/README.md)
and [the trained-model NNAPI runner](../tests/pixel_tpu/TRAINED_DEPTH.md).

The Pixel 9 Pro XL exposes a Tensor inference accelerator through its existing
NNAPI stack. A standalone, temporary probe on the connected device reported:

```text
google-edgetpu     type=ACCELERATOR  feature=1000008  version=2.0
nnapi-reference    type=CPU          feature=1000008
```

A quantized ADD graph with 2,048 runtime input/output elements was checked for
support, compiled with **only `google-edgetpu` selected**, and executed successfully.
Every output matched the expected value. The NNAPI reference CPU device was not
included in the compilation device list. This establishes that the named driver
can execute that graph; it is not a measurement of TPU utilization, depth-model
support, model quality or scanner speed. The probe ran as shell UID and did not
install/restart the scanner, use captured images, or change permissions.

Temporary probe sources are under `/tmp/opencode/scanner-build-verification/`
(`nnapi_devices.c`, `probe_nnapi.py`). Uploaded probe files were removed after use.
No NNAPI/LiteRT dependency or model was added to the scanner by this investigation.

## Where acceleration fits

- A trained depth-denoising, depth-completion or surface-normal network could be
  a useful TPU workload. It needs per-model operator/quantization checks, actual
  app-context deployment tests, measured latency including transfers, and geometry
  accuracy/edge/thin-feature tests. Plausible-looking added depth is not evidence
  of correct measured geometry.
- Current sparse voxel fusion, hash/map access and mesh triangulation are ordinary
  geometry algorithms, not neural-network inference graphs. There is no general
  switch that runs those C++ routines on the TPU. CPU algorithm improvements and
  suitable GPU-compute designs address those operations more directly.
- ARCore manages its own depth implementation. This probe does not establish
  which parts of ARCore currently use the TPU, GPU or CPU.

## SDK support boundary

Google's **May 19, 2026 Tensor ML SDK Beta announcement** lists the Pixel 10 family
as supported. That newer LiteRT/Tensor SDK route must not be assumed to support
this Pixel 9 merely because the phone has Tensor silicon. Conversely, that SDK
list does not negate the working older NNAPI driver observed above.

NNAPI is deprecated (also marked in the installed NDK r28c header), so a new ML
feature needs a deliberate compatibility/fallback plan. The useful next TPU
experiment would compare a suitable depth-cleanup model against the current
measured depth input, not replace the geometry engine with an untested ML pipeline.

Sources:
- https://developers.googleblog.com/google-tensor-sdk-beta-with-litert/
- https://github.com/google-ai-edge/LiteRT
- https://developer.android.com/ndk/guides/neuralnetworks

## Depth/normal workload experiment — 1 October 2026

**Result:** a realistic-sized quantized convolution/decoder graph executes through
the explicitly selected Pixel 9 `google-edgetpu` driver, beyond the earlier ADD
probe. No deployable pretrained depth-cleanup/normal model was established in this
bounded investigation. The experiment also exposed a reproducible numerical
failure when quantized L2 normalization follows the diagnostic decoder.

Implementation and reproduction commands are in
[`tests/pixel_tpu/`](../tests/pixel_tpu/README.md). No scanner source, Android app,
build dependency, captured dataset, permissions or installed package was changed.
No app install/restart, log clearing or camera access was used. All inputs are
generated in the standalone shell process. Device runs lasted roughly 9–12 seconds
each; they can contend with other phone workloads, so these timings are not a
controlled simultaneous scanner benchmark.

### Model shortlist: inspected artifacts versus deployment blockers

#### 1. NConv-CNN unguided depth: closest input/task match, not a ready TPU model

- Upstream: [abdo-eldesokey/nconv](https://github.com/abdo-eldesokey/nconv), pinned
  revision `d85d4b659f2207b397c62d81f27f363baf3397be`.
- Downloaded actual pretrained artifact
  `workspace/exp_unguided_depth/checkpoints/CNN_ep0003.pth.tar`: **12,251 bytes**,
  SHA-256 `27161a0a69d731e887ee1ce3894dec7794fd8a21178bbf2c49fc1a0dc0517f1c`.
  It was hashed, not deserialized or executed. Thus checkpoint tensor contents
  and quantization are not claimed to have been independently inspected.
- Reviewed the actual `network_exp_unguided_depth.py`, `modules/nconv.py`,
  `params.json`, and KITTI loader. Source-level interface: float NCHW depth
  `[N,1,H,W]` plus confidence of the same shape, returning depth and confidence.
  Loader uses nonzero-depth validity, KITTI encoded depth divided by 256, and
  `invert_depth=false`; that division is KITTI unit decoding, **not proof of a
  normalized [0,1] depth range**, despite the loader comment. Use H/W divisible
  by 8 for an initial fixed-shape export because there are three 2× pool levels.
- Actual operations include 5×5, 3×3 and 1×1 normalized convolutions, multiplication
  by confidence, division by convolved confidence plus `1e-20`, learned bias,
  confidence normalization, max-pool **with returned indices and depth gather**,
  nearest-neighbor resize and concatenation. It hard-codes CUDA temporary tensors.
  This is not equivalent to replacing the layer with ordinary `CONV_2D`.
- NNAPI `DIV` has float16/float32/int32 signatures, **no quant8 signature**
  (installed NDK `NeuralNetworksTypes.h`, lines 2127–2175). Our float32 DIV case
  is rejected by this TPU driver. The reviewed release supplies PyTorch code and
  checkpoints, not a verified calibrated full-integer mobile export. Confidence
  approaching zero and indexed pooling need careful export/redesign; this
  experiment did not test a complete NConv graph or its gather conversion.
- **License:** repository and source headers say GNU GPLv3. The checkpoint is in
  that repository; no separate checkpoint-specific grant was verified. It is not
  an Apache/MIT drop-in for this Apache-2.0 project. No NConv implementation or
  weights were copied into the scanner. KITTI training/domain assumptions also
  do not establish indoor ARCore-depth cleanup quality.

#### 2. MiDaS v2.1 small mobile: accessible MIT artifact, wrong input/task and dtype

- Actual upstream release download:
  [`model_opt.tflite`](https://github.com/isl-org/MiDaS/releases/download/v2_1/model_opt.tflite),
  **66,338,288 bytes**, SHA-256
  `93d871071edff1218973ce25ee27ce95ccd20450c70a55e1b89efa3f5a772cdd`.
- Independently parsed its TFL3 FlatBuffer: input **float32 `[1,256,256,3]`**,
  output **float32 `[1,256,256,1]`**; 331 float32 and 5 int32 tensors;
  **zero tensors with quantization scales**. The filename “opt” does not mean INT8.
- Exact exported graph inventory: **73 CONV_2D v1, 24 DEPTHWISE_CONV_2D v1,
  27 ADD v1, 7 RELU v1, 5 RESIZE_BILINEAR v3** (136 total). Fused activation and
  resize flags still require per-node inspection for an actual converter/runtime.
- Upstream [mobile documentation](https://github.com/isl-org/MiDaS/tree/master/mobile)
  identifies the EfficientNet-Lite3 small decoder and RGB image input. Its output
  is monocular **relative inverse depth**, not corrected measured metric depth or
  normals. A synthetic depth tensor cannot be substituted for an RGB input to
  obtain a meaningful quality benchmark. Exact TFLite image preprocessing was not
  validated by an inference run here.
- The tested float32 conv/decoder shapes are all rejected by `google-edgetpu`.
  This does not constitute a full-model delegation audit, but there is no evidence
  this unquantized artifact can run wholly on this driver. It needs calibrated
  conversion plus complete node/partition checks before any TPU claim.
- **License:** MIT, verified from the release repository's `LICENSE`, retained
  beside the downloaded artifact. More permissive than NConv/DSINE, but licensing
  alone does not solve its input/domain and quantization mismatch.

#### 3. DSINE: useful normal reference, excluded from a quick deployable path

- [Official README](https://github.com/baegwangbin/DSINE) links pretrained
  `exp001_cvpr2024/dsine.pt` on Google Drive; weights were not downloaded here.
- Reviewed `models/dsine/v02.py`: float RGB image + camera intrinsics; returns a
  list of normalized three-channel maps. Includes iterative ConvGRU refinement,
  neighborhood unfold, masked assignments, division, vector normalization,
  axis-angle rotation/matrix multiplication and learned upsampling. No verified
  quantized LiteRT artifact was identified in the reviewed release instructions.
- **License:** the official [LICENSE](https://github.com/baegwangbin/DSINE/blob/main/LICENSE)
  is an Imperial research-only agreement with non-commercial/internal-or-academic
  restrictions and transfer/access restrictions (BSD components have separate
  terms). It is not a permissive normal-estimation dependency.

### Tested graph and quantization contract

The probe constructs NHWC models directly with NDK NNAPI, avoiding a framework
dependency or opaque delegate fallback:

```text
input [1,120,160,2] (synthetic depth-like ramp + validity-like channel)
  -> 3x3 CONV_2D 2->8, SAME, stride 1, fused ReLU       [1,120,160,8] (skip)
  -> 3x3 DEPTHWISE_CONV_2D, multiplier 1, SAME, ReLU   [1,120,160,8]
  -> 1x1 CONV_2D 8->8, ReLU                          [1,120,160,8]
  -> 2x2 AVERAGE_POOL_2D, VALID, stride 2              [1,60,80,8]
  -> RESIZE_BILINEAR to 160x120                       [1,120,160,8]
  -> CONCATENATION with skip, channel axis 3          [1,120,160,16]
  -> 1x1 CONV_2D 16->3, ReLU                         [1,120,160,3]
  -> optional L2_NORMALIZATION on last axis           [1,120,160,3]
```

The seven-op graph is a decoder-shaped **support/latency probe**, not NConv,
MiDaS, DSINE or a trained network. Kernels vary by spatial/channel index and use
exactly representable positive coefficients; no random/untrained-weight quality
claim is made. The ramp has a step and a zero-depth stripe. The standalone L2
case additionally tests negative vector components and all-zero vectors.

Tested q8 contract: **unsigned `TENSOR_QUANT8_ASYMM`, scale 1/128, zero-point 128**
for all activations and filters. Bias is int32 with scale 1/16384 and zero-point
0. Conv filters use OHWI; depthwise filters use `[1,3,3,C]`. L2 output's mandated
scale/zero-point is also 1/128 and 128. Pool/resize/concat retain matching scales.
Only static batch-1 shapes, per-tensor filters, implicit NHWC, and default legacy
resize coordinates were tested. **Signed INT8, per-channel weights, float16,
dynamic shapes, alternate resize flags, large trained networks, and quantization
accuracy across physical depth ranges remain unverified.** The diagnostic scale
is not a proposed metres-to-integer mapping for real captured depth.

### Measured final run

Pixel 9 Pro XL, Android API 37, fingerprint
`google/komodo/komodo:17/CP3A.260905.009/16091614:user/release-keys`.
Driver `google-edgetpu`, accelerator type, version 2.0, feature level 1000008.
Final run started **2026-09-30 17:32:01 UTC** (1 October local session date).

For each case, query every operation, then compile with
`ANeuralNetworksCompilation_createForDevices(..., {google-edgetpu}, 1, ...)` and
`PREFER_FAST_SINGLE_ANSWER`. Unsupported graphs are skipped; default compilation
is never used. Separate compilations selecting **only `nnapi-reference`** provide
CPU comparison. No reference-CPU fallback is requested in accelerator runs.
This proves named-driver execution, not measured physical TPU utilization or
knowledge of any internal driver implementation.

Each timing has 3 warmups (first call also reported) and 15 measured calls.
The p50/p95 is nearest-rank; with 15 calls p95 is the maximum. Timed work includes
host input/output copies, execution allocation/binding, synchronous compute and
its NNAPI transfers, and execution destruction. Scene generation/quantization,
compilation, ADB, camera/app scheduling and geometry are excluded. CPU is NNAPI's
reference implementation, **not an optimized CPU performance baseline**.

| q8 case | Selected-driver p50 / p95 ms | Reference CPU p50 ms | Max final output difference, integer codes |
|---|---:|---:|---:|
| 3×3 conv, 2→8 | 1.002 / 1.587 | 5.716 | 1 |
| 3×3 depthwise, C=8 | 1.804 / 2.551 | 5.218 | 1 |
| 1×1 conv, 8→8 | 1.886 / 2.476 | 3.896 | 1 |
| Average pool | 1.623 / 2.095 | 0.771 | 1 |
| Bilinear resize | 0.981 / 1.727 | 1.118 | 1 |
| Concatenate two C=8 inputs | 2.408 / 2.522 | 0.436 | 0 |
| ADD, C=8 | 2.136 / 2.487 | 0.417 | 0 |
| MUL, C=8 | 2.127 / 2.949 | 1.108 | 1 |
| Standalone L2, C=3 | 1.426 / 1.790 | 3.064 | 1 |
| Seven-op decoder | **0.902 / 1.449** | 32.147 | 1 |
| Decoder + L2 | **1.240 / 1.471** | 34.087 | **11 — tolerance failure** |

All eleven q8 graphs reported complete driver support and compiled/executed.
The seven-op graph transfers **38,400 input + 57,600 output bytes** per call;
driver compilation took **263.505 ms**, first execution **16.541 ms**. For decoder
+ L2 these were **132.089 ms** and **7.734 ms**. This illustrates why initialization
and amortized resident-graph latency must be accounted for separately. Isolated
pool/concat/add/mul were slower on the selected accelerator than the reference
CPU here; many tiny separately delegated islands are not a sensible default.

The float32 conv, L2, DIV and seven-op decoder cases reported **unsupported** on
the accelerator and ran **only as explicitly labeled CPU baselines**. CPU p50s
were 1.774, 0.139, 0.387 and 6.342 ms respectively. No float32 TPU timing exists.

#### Numerical failure retained, not hidden

The final decoder q8 output had 24 distinct codes and agreed within one code over
all 57,600 components. With L2 appended, **1,056 components across 732 pixels**
exceeded the fixed two-code tolerance; maximum difference **11 codes**. Cross-device
vector-direction disagreement reached **5.846°** (p95 **1.095°**, mean **0.208°**).
Those angles compare two executions of the same untrained graph, **not predicted
normals against ground truth**. Standalone signed-direction L2 inputs agreed
within one code, including zero vectors.

Separately compiled pre-L2 CPU output norms at disagreeing pixels ranged from
8.66 to 36.96 integer-code units. This is consistent with normalization amplifying
small upstream differences, but does not prove the exact fused-compiler cause.
The failure reproduced in runs 02 and 03. Run 01 used overly uniform filters,
giving a degenerate equal-channel normal head; it is superseded, not the reported
normal-head result. The final binary returns **4** for the failed comparison and
the overall runner returns **1**. All other cases return 0, including explicitly
recorded unsupported-device skips. Remote cleanup returned 0.

No neural quality benchmark was performed: none of the shortlisted pretrained
models was run through this accelerator, and a diagnostic smoothing graph cannot
establish edge/gap preservation or metric accuracy. Full outputs, including the
failed head, are saved so the numerical finding can be independently checked.

### Evidence and next integration step

Persistent compact evidence:
[`tests/pixel_tpu/evidence-2026-10-01.json`](../tests/pixel_tpu/evidence-2026-10-01.json).
Raw local artifacts:

- `/tmp/opencode/pixel-tpu/run-03/results.json`: exact compile/run commands, hashes,
  device identity, per-op booleans, every latency sample, comparison and exit status.
- `/tmp/opencode/pixel-tpu/run-03/*.stdout.jsonl` and `*.stderr.txt`: case logs.
- `/tmp/opencode/pixel-tpu/run-03/{l2,decoder,decoder_l2}-q8.{cpu,tpu}.bin`:
  synthetic final outputs; `numerical-analysis.json`: independent NumPy comparison.
- `/tmp/opencode/pixel-tpu/models-01/manifest.json`: downloaded model/source/license
  URLs, sizes, SHA-256 and actual MiDaS graph inspection. No private imagery.

**Next bounded step:** obtain or train a clearly licensed, depth+validity-input
compact residual CNN, exporting a fixed-shape full-integer graph limited initially
to the demonstrated convolution/resize/concat operations. Establish an FP32
baseline before quantization, use representative depth/confidence calibration,
and preserve an independent invalid-depth mask. For a normal head, compare
dequantized CPU normalization against the failed in-graph quantized L2 case rather
than assuming operation support guarantees usable normals. NConv is a useful
offline reference only after resolving checkpoint/license use; its confidence
division and gather cannot simply be quantized away. MiDaS requires a different
RGB-prior feature and is not a measured-depth-cleanup replacement.

Before scanner integration, benchmark that **actual trained model** against an
analytic perspective plane, sloped plane, depth step, thin foreground surface
and holes: valid-pixel metric depth error, normal angular error, edge displacement,
and false gap filling, compared with unchanged depth and a simple non-neural
baseline. Reject unsupported full graphs or explicitly report mixed/CPU paths;
do not silently count partial delegation as TPU success. Then measure app-context
latency including preprocessing, quantization, transfer, inference and mask/output
handling with compiled-model reuse. Direct NNAPI works here but is deprecated;
an isolated LiteRT NNAPI delegate path would need an explicit accelerator name,
disabled NNAPI CPU device, and verified full delegation/partition counts. Its
interpreter CPU fallback is a separate concern from NNAPI reference-CPU selection.
The Pixel-10-only Tensor SDK and Coral Edge TPU compiler are not established
deployment paths for this Pixel 9 experiment.
