# Fusion voxel-access optimization

## Removed work

The paged reconstruction path used to unpin and repin its current voxel chunk
for every read/write, even when consecutive nodes belonged to the same resident
record. This repeated residency checks, pin-counter updates and LRU bookkeeping
inside the fusion loop.

`recon::Transaction` now reuses its existing single pin when record identity is
unchanged. Missing records release the pin. Switching records still uses the
normal checked pin operation, and copy-on-write releases/reacquires the source
before allocating/replacing the record. Dirty marking and all fusion arithmetic
remain unchanged. The RAM-only path is unchanged.

No new cache or payload memory is allocated. This changes eviction bookkeeping,
not synchronization locks, measurement filtering or reconstruction quality.

Before core SHA-256:
`548b87ef7a4cdeab4587f38ce96cbe2895c48eefbcf3cd77510a4089b1a88842`

After core SHA-256:
`d6db98d7ef4040ec620b4cd5fc35d89754b3ba3fcf9105d2638f3614ca2d04cd`

## Pixel measurements

The same generated room/box/sphere points, colors and moving poses were processed
by frozen before/after cores, using ABBA order and fresh processes. The 24-frame
5 cm test was pinned to device CPU 5 to reduce scheduling variation; governors
and app settings were not changed. Each variant's full output matched byte-for-byte.

| Workload | Fusion before → after | Fusion reduction | Fusion + extraction reduction |
| --- | --- | ---: | ---: |
| 12 frames, 4 cm, normal scheduling | 1,011.69 → 830.37 ms | 17.92% | 15.06% |
| 12 frames, 5 cm, normal scheduling | 812.65 → 710.92 ms | 12.52% | 11.71% |
| 24 frames, 5 cm, CPU 5 | 2,702.89 → 2,251.98 ms | 16.68% | 14.43% |

These are medians of sequence totals, not per-frame costs or whole-app FPS.
The short unpinned 5 cm runs varied substantially (candidate fusion totals
620–802 ms), motivating the longer CPU-pinned check. The pinned totals were
2,688–2,718 ms before and 2,219–2,285 ms afterward. Its complete 38,861,461-byte
output had SHA-256 `2129d591fecc058752b62c3f967da35ce81e4c1861d1e6d6b4d59a8484ca1c82`
in all four runs. All frames were accepted.

Host alternating comparisons at 2 cm and 4 cm measured roughly 11–12% less paged
fusion time, with identical dirty-index arrays and final mesh bytes.

## Recorded-data and failure checks

- The frozen 451-frame recording (4,707,444 points) was replayed at 2 cm with
  paging before and after. Both accepted every frame and produced the same
  2,297,140 faces / 1,291,708 vertices, normals, topology and mesh digest:
  `3e260fb2ab9082d648c38110a0267807c1b1a397c379e84c36ceaca4747ee3ae`.
- Both held peak voxel payload to 96 MiB, with the same 2,342 logical chunks,
  1,413 backing writes/evictions and backing extent. This is not total-app RAM.
- Core sanitizer tests passed, including confidence, clearing, color, seams,
  replay, allocation failures, capacity rejection and subsequent recovery.
- Paged sanitizer tests passed: eight-chunk minimum, 1,100 logical chunks,
  partial writes, checksum/EOF failures, cold-load allocation failure, pin unwind,
  strict backing budgets, rollback and retry.
- A separate per-frame coordinate-cache candidate was rejected: it added memory
  and regressed RAM fusion without a convincing benefit over pin reuse alone.

## Reproduction and evidence

`tests/reconstruction/generation_quality_android.py` now accepts an explicit
`--paged-resolution .05`; omitting the option retains the original fixture's
resolution. It is only valid with `--paged`.

```sh
python3 tests/reconstruction/generation_quality_android.py \
  --before /tmp/opencode/recon-hotpath-20261001/before/core.cc \
  --sdk /tmp/opencode/pixelshare-sdk --serial DEVICE \
  --output /tmp/opencode/NEW-RESULT-DIRECTORY \
  --workload-case 0 --frames 24 --paged --paged-resolution .05 \
  --cpu 5 --compare-output
```

Results are under `/tmp/opencode/recon-hotpath-pin-{host,pixel}-20261001`,
`/tmp/opencode/recon-pin-coverage-pixel{-cpu5,}-20261001`, and
`/tmp/opencode/recon-pin-recorded-{before,after}-20261001`.

## Installed artifact

`artifacts/3DLiveScanner-modern-fusion-cache-debug-2026-10-01.apk`

SHA-256: `133ea10f8ecec16f204cd500255d36980701eb37b3c26a7d66241b021953e7f5`

Installed in place after the user confirmed the scan was saved. Installed bytes
matched, the capture commit-state check passed, startup reached FileManager,
and the scoped crash buffer for PID 27067 was empty. Capture-first, coverage
preview and experimental TPU cleanup remain enabled. Modern assembly, static
16 KiB/dependency audit, zipalign and signature checks passed. No whole-app
capture-rate increase is claimed from the standalone reconstruction benchmarks.
