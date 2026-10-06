# Continued model work and completed Pixel validation

## Result

After the earlier premature handoff, offline model development continued and the
Pixel was reconnected at `192.168.1.114:33123`. The final candidate completed
inference on **56 fresh held-out synthetic scenes** through the explicitly
selected `google-edgetpu` driver.

| Measurement | Final candidate |
| --- | ---: |
| Raw depth RMSE | 8.8039 mm |
| Trained model RMSE, actual TPU outputs | 6.5400 mm |
| Reduction versus raw | 25.71% |
| Lowest tested matched-filter RMSE (5x5 mean) | 6.5199 mm |
| TPU prepared-input inference median / p95 | 0.748 / 0.846 ms |
| Compilation / first call | 253.39 / 11.64 ms |
| Edge/thin-feature/clean-surface/validity gates | All passed on these 56 scenes |
| Original 5% overall-RMSE advantage gate | Not passed |

**Production integration remains unapproved.** The original aggregate-error gate
is still reported as a failure; it was not lowered or removed. No actual sensor
accuracy or capture-FPS improvement is claimed. Input-feature preparation and
depth postprocessing are excluded from the reported NNAPI execution time.

There is an important qualification to the baseline comparison: **none of the
tested denoising baselines passed all the same detail-preservation gates**. The
5x5 mean's slightly lower overall error comes with low-contrast edge/strip failures.
Unrestricted bilateral also changes clean sloped/curved surfaces. The trained
candidate is therefore a promising detail-preserving tradeoff, not simply a
slower equivalent of those filters. This does not make it production-validated:
different CPU filters, real sensor data and whole-pipeline costs remain to test.

## Work completed

1. Added coarse measured-depth context alongside the fine local residual and
   confidence. The earlier input encoded high-frequency residuals but discarded
   broader foreground/background structure.
2. Changed training to random crops of full 120x160 frames, preserving the
   evaluation pixel-gradient scale instead of generating every training patch
   as a separate small-camera image.
3. Added wider context, gradient supervision and stronger 5x5/7x7 CPU baselines.
4. Corrected an actual supervision problem: metric range alone classified steep
   smooth surfaces as edges. Training now distinguishes true discontinuities
   using synthetic scene labels. The original validation metric was retained.
5. Balanced training crops around thin structures. Large step foreground regions
   previously dominated the foreground weighting, while thin objects were rare.
6. Froze the selected weights before evaluating a new 9,200,000-series split:
   14 scene families, four samples each. No fitting or calibration occurred in
   this fresh evaluation.
7. Implemented an independent integer-accumulation model evaluator and checked
   it against real NNAPI outputs rather than trusting training simulation alone.
8. Completed phone inference and evaluated the actual output buffers against
   the same depth, edge, strip, hole and clean-surface metrics.

## Development record

The 9,100,000-series split was already inspected and is explicitly development
data. It is not presented as untouched testing after model selection.

| Development candidate | Overall RMSE | Detail result |
| --- | ---: | --- |
| Coarse context + full-frame crops | 7.1652 mm | Low-step and low-thin edge failures |
| Wide context + conservative edge targets | 7.7471 mm | Thin-recall failure; smooth-gradient supervision problem |
| Corrected semantic-edge targets | 7.2206 mm | Remaining thin-recall failure |
| Balanced thin-structure crops (selected) | 7.3533 mm | Detail gates passed; aggregate-value gate failed |

The selected candidate was chosen for detail preservation, not the lowest
development aggregate error. Its final 6.5400 mm result uses a different, fresh
split and must not be compared directly to the development numbers as a speedup
or accuracy gain from another training iteration.

Model SHA-256:
`76ef7a8c8376a1a921e2f055419d27b4eaeca425ac2b430dd3f14a89eae84be0`.
The graph remains three q8 Conv2D layers, 1,753 parameters, 1,880-byte export.

## Numerical comparison

The independent evaluator performs integer dot products, int64 bias addition,
and requantization using the exported descriptors. It matched **all 1,075,200
TPU output codes exactly** for the final 56-frame run. NNAPI reference CPU
outputs differed by up to **12 codes**. The original two-device, two-code
comparison therefore does not pass; the CPU discrepancy is retained explicitly.
The target-specific arithmetic check passes, and postprocessed depth quality
is evaluated independently rather than treating reference CPU as unquestioned
ground truth. No fallback to that CPU device was requested for the TPU run.

The new integer evaluator was also cross-checked on the earlier V1 run: TPU
matched exactly and reference CPU differed by at most two codes on 36 frames.
The prior L2-normalization discrepancy is not claimed to be fixed; this model
has a scalar residual head with no L2 operation.

## Reproduce the selected candidate

```sh
/tmp/opencode/pixel-depth-venv/bin/python tests/depth_cleanup/train.py \
  --output /tmp/opencode/new-balanced-training --steps 10000 --qat-steps 2500 \
  --threads 4 --evaluation-seed 9100000 --feature-version wide_context \
  --full-frame-crops --include-high-noise --edge-weight 8 --foreground-weight 40 \
  --gradient-weight .25 --semantic-edge-loss --strong-baselines --focus-thin
/tmp/opencode/pixel-depth-venv/bin/python tests/depth_cleanup/evaluate.py \
  --training /tmp/opencode/new-balanced-training \
  --output /tmp/opencode/new-balanced-holdout --seed 9200000 --repeats 4
python3 tests/pixel_tpu/run_trained_depth.py \
  --model /tmp/opencode/new-balanced-holdout/model.bin \
  --inputs /tmp/opencode/new-balanced-holdout/inputs.bin \
  --serial "$PIXEL_SERIAL" --output /tmp/opencode/new-balanced-device --repetitions 30
/tmp/opencode/pixel-depth-venv/bin/python tests/depth_cleanup/analyze.py \
  --training /tmp/opencode/new-balanced-holdout \
  --tpu /tmp/opencode/new-balanced-device/depth.tpu.bin \
  --cpu /tmp/opencode/new-balanced-device/depth.cpu.bin \
  --output /tmp/opencode/new-balanced-holdout/device-analysis.json --brief
/tmp/opencode/pixel-depth-venv/bin/python tests/depth_cleanup/check_reference.py \
  --device-run /tmp/opencode/new-balanced-device --device tpu
```

Without `--device tpu`, the reference check still tests both devices and reports
the CPU's two-code-tolerance failure. Model training now saves source snapshots
under its output's `sources/` directory and checks source hashes before/after.

Final evidence:

- `/tmp/opencode/pixel-depth-balanced-edge-dev-20261001/` — training, weights and source snapshots.
- `/tmp/opencode/pixel-depth-balanced-edge-holdout-20261001/` — fresh inputs/truth and host/device quality.
- `/tmp/opencode/pixel-depth-device-balanced-20261001/` — exact driver support, timings, buffers, hashes and cleanup.

No scanner code, capture files, settings or installed APK were modified by this
model work. [The production-path audit](../../docs/pixel-depth-integration.md)
identifies the remaining real-sensor boundary: full native raw depth/confidence,
stride-aware ownership and synchronized pose/RGB—not processed `.pcl` recordings.
Synchronous inference on the current GL/render-lock path is not acceptable.
