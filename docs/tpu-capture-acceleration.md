# TPU acceleration: capture-path measurements

## Objective

Use acceleration to replace work on the capture critical path while preserving
the partial-cell mesher that the user found clearly improved. Adding a neural
correction stage does not by itself accelerate capture.

The Pixel's `google-edgetpu` driver is explicitly selected in these probes;
there is no automatic reference-CPU fallback. The tests use generated inputs in
a separate shell process and do not start the camera, read scans or change app
settings. They establish operation/runtime costs, not TPU occupancy or app FPS.

## Measured depth-runtime cost

The existing 3→12→12→1 quantized model was measured at native 160×90 and at
320×180. Model parameters were unchanged; no new model was trained in this work.
Twenty measured calls follow warm-up. Reported medians select the upper middle
observation; p95 is the indexed 95th-percentile observation. The process was
pinned to CPU 5; device load and frequency remained uncontrolled.

| Generated shape | CPU feature preparation | TPU execute + binding | Complete Runtime::Apply |
| --- | ---: | ---: | ---: |
| 160×90 | 0.633 ms | 0.667 ms | 2.363 ms |
| 320×180 | 3.414 ms | 1.751 ms | 15.312 ms |

`Runtime::Apply` includes validation, feature generation, inference, guarded
point mapping and publication. It excludes ARCore/camera work, original-input
backup writes, fusion, mesh extraction, rendering and other recording I/O.
The separately sampled component medians should not be subtracted to derive an
exact missing-stage time. The earlier failed run measured 5.33 ms for the
160×90 complete runtime, demonstrating substantial run-to-run variation.

The network is already a small part of the path. A faster network alone would
not remove the CPU preparation and surrounding work, and the currently accepted
scanner configuration has this correction stage disabled altogether.

## Exact mesh-operation offload test

A fixed quantized 1×1 convolution computes observed-corner and sign counts for
each tetrahedron. Its masks match the CPU reference exactly. This is the smallest
useful arithmetic offload for one mesh-support decision; it is not a learned
mesh generator and does not include triangle generation or paging.

| Batch | Direct CPU classification | Packing + TPU + decoding | Comparison |
| --- | ---: | ---: | --- |
| 1 chunk / 24,576 tetrahedra | 0.0257 ms | 1.498 ms | about 58× slower offloaded |
| 8 chunks / 196,608 tetrahedra | 0.2036 ms | 4.574 ms | about 22× slower offloaded |

The packed inputs are 196,608 and 1,572,864 bytes respectively. Transfer/binding,
tensor preparation and dispatch overwhelm the small CPU operation. This tested
offload is therefore **not integrated into live scanning**. It does not establish
that every possible large tensorized fusion workload would be slower.

## NNAPI lifetime defect found and repaired

The initial standalone probe completed its first depth case and then crashed in
a service thread during runtime teardown (native exit 139, `pthread_cond_clockwait`,
caller address in an unloaded mapping). Each backend had been closing its
`libneuralnetworks.so` handle when destroyed, while service threads could outlive
that backend.

The optional loader now retains one process-lifetime mapping, shared under a
short mutex. Failed loads remain retryable. Executions, compilations and models
are still freed normally; only the library mapping remains. No mandatory NNAPI
dependency is introduced, and disabled TPU use does not load the library.

The same probe subsequently completed both depth shapes and both mesh batches,
including repeated graph/runtime construction and destruction. Host normal and
sanitized runtime contracts, all 90 byte-exact preprocessing oracle cases and
the API24 dependency audit passed. This is a reliability fix, not a measured
inference-speed gain.

## What a faster trained replacement would require

The current NNAPI integration performs inference, not training. Training would
happen off-phone. A smaller/distilled, quantization-aware model could reduce
inference cost, but it must replace a substantial existing stage to improve the
scanner that currently runs without it.

A candidate could combine depth filtering/support estimation, or predict dense
local fusion updates, with inexpensive input preparation and minimal transfers.
It needs an explicit CPU operation to replace, same-frame ownership, a bounded
fallback, and measured total capture-path latency. A mesh-update predictor also
needs checks for metric scale, thin surfaces, gaps, unsupported geometry and
temporal consistency. Training longer or producing smoother-looking output is
not evidence of either faster scanning or correct geometry.

The accepted raw recordings and partial-cell meshing result remain the baseline.
The previous experimental correction is not re-enabled merely because the TPU
can execute it quickly.

## Reproduction

```sh
python3 tests/pixel_tpu/run_pipeline_probe.py \
  --sdk /tmp/opencode/pixelshare-sdk --serial DEVICE \
  --output /tmp/opencode/NEW-TPU-RESULT-DIRECTORY --cpu 5
```

Initial failure: `/tmp/opencode/tpu-pipeline-costs-20261001`.
Completed probe: `/tmp/opencode/tpu-pipeline-costs-fixed-20261001`.
Source: `tests/pixel_tpu/pipeline_probe.cc`.

The independent native normal-cache experiment is documented in
[normal-cache-performance.md](normal-cache-performance.md).
