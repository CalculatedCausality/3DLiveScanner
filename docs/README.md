# Documentation index

## Current scanner workflow

| Guide | Purpose |
| --- | --- |
| [Build variants](scanner-build-variants.md) | Toolchains, default isolated quality flavor, build and audit commands |
| [Quality modernization](quality-modernization.md) | Current candidate, original-native preservation, capture cadence and pacing diagnostics |
| [Quality rollback](quality-rollback.md) | Validated historical baseline, backups and restoration evidence |
| [Device validation](scanner-device-validation.md) | Real-device quality, performance and interaction checks |
| [Capture workspace](capture-workspace.md) | Capture ownership, recovery and storage selection |
| [Model generation](model-generation.md) | Dataset-to-model processing and validation boundaries |

## Experimental engine and performance research

These guides retain historical measurements and implementation evidence.
The experimental engine failed later moving/revisit scan tests. Numerical or
component-level passes do not establish parity with the restored baseline.
Check the [current status](quality-modernization.md) before using an old profile
or deployment command.

| Area | Guides |
| --- | --- |
| Capture pipeline | [Capture speed update](capture-speed-update.md), [Live throughput](live-capture-throughput.md), [Fusion pin reuse](fusion-pin-reuse.md) |
| Geometry and preview | [Partial-cell meshing](partial-cell-meshing.md), [Normal cache](normal-cache-performance.md), [Preview stability trial](preview-stability-trial.md), [Capture reference repair](capture-reference-repair.md) |
| Capacity and export | [Reconstruction paging](reconstruction-paging.md), [Large-model texturing](large-model-texturing.md) |
| Pixel accelerators | [Accelerator research](pixel-accelerators.md), [Depth integration](pixel-depth-integration.md), [Experimental TPU test](experimental-tpu-test.md), [TPU pipeline costs](tpu-capture-acceleration.md) |

## Related indexes

- [Repository overview](../README.md)
- [Test suites](../tests/README.md)
- [Audit tools](../tools/README.md)
- [Archived APKs and manifests](../artifacts/README.md)
- [Experimental engine migration](../reconstruction/MIGRATION.md)
