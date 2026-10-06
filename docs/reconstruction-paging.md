# Disk-backed reconstruction integration

## Purpose and boundaries

The installed workspace build hit its RAM-only 1,024-chunk limit during a 2 cm
capture. The replacement integration enables a bounded resident voxel cache with
private disk backing, instead of coarsening the scan or making RAM grow with the
entire volume. The pager and its acceptance evidence are documented alongside the
engine tests; final integration/device results must be recorded before claiming
this candidate is validated on the phone.

The camera still draws nearby preview segments, but `TangoScan` currently retains
all accepted CPU-side preview meshes. Export also constructs model data in memory.
**Voxel residency is not a whole-process memory bound.** The existing 1.5 GiB /
40%-of-device-RAM process guard and low-storage guard remain relevant. Preview
payload accounting is reported separately rather than hidden in the paging claim.

A read-only on-device audit of the latest committed preview headers found 767
mesh segments, 663,313 vertices and 1,175,181 faces after 227 frames. The vertex/
normal/color/index arrays account for **31.16 MiB**, excluding containers, AR/GL
and export copies. At the old 1,024-chunk cap the voxel payload alone is 96 MiB.
This supports addressing voxel residency first, while keeping preview growth
visible. The mesh also exceeds the separate texturer face budget; paging does
not implement simplification or remove that export limit.

The audit skips vertex/image payloads and does not change capture data. It creates
and removes only a temporary helper dex under the existing debug app's cache:

```sh
JAVA_HOME=/path/to/jdk17 python3 tests/capture_io/preview_memory_audit.py \
  --serial DEVICE --sdk /path/to/android-sdk \
  --capture /data/user/0/com.lvonasek.arcore3dscanner/files/capture-dataset
```

## Caller policy

`common/tango/paging_policy.h` computes a plan at scan setup:

- Resident payload allowance is physical RAM / 64, clamped to 32–96 MiB. Unknown
  physical RAM uses 64 MiB. It does not increase when resolution is made finer.
- Finer resolution requests more backing capacity relative to a 4 cm grid. The
  backing-file ceiling is 1 GiB, and 1 GiB of free storage is reserved for the
  existing resource-pressure guard. At most half the remaining free space is
  assigned to backing.
- A conservative 128 KiB/chunk policy allowance and 64 MiB staging/metadata
  reserve determine the logical limit. The engine separately enforces its actual
  payload/record accounting. The policy does not preallocate the entire budget.
- Paging-enabled per-frame admission scales from 256 to at most 512 changed chunks
  with the resident budget. A 96 MiB working set uses 512; a 32 MiB working set
  retains 256. The 64 MiB staging allowance covers the upper policy choice. The
  32-million work cap and RAM-only defaults remain unchanged.
- With ample storage and at least 6 GiB physical RAM, the 4 cm plan is 96 MiB
  resident / 256 MiB backing / 1,536 logical chunks; at 2 cm it is 96 MiB resident /
  1 GiB backing / 7,680 logical chunks. Both 96 MiB profiles allow 512 changed
  chunks per frame; this is an application policy, not a global engine-default change.
- Less than 256 MiB affordable backing disables paging explicitly and retains the
  bounded RAM-only fallback. Once a paging-enabled plan has been selected, failure
  to initialize that pager fails context creation instead of pretending it worked.

Main passes the app-private scratch directory through the JNI capture-setup
contract. The native worker selects the plan while holding the reconstruction
binder. The plan is retained for Clear/replay; recomputing it from free disk while
replacing an existing cache could shrink it below already committed history.
Cache files are disposable; committed datasets and preview files remain the
recovery source.

## Failure and frame-admission behavior

Transactional frame rejections preserve the volume. An unreadable/corrupt cold
page reported with `requires_replay` instead marks the context for recovery from
committed history. Failure to obtain a valid storage-status snapshot also fails
conservatively into recovery. Extraction/persistence failures after integration
continue to require recovery.

Atomic rejections delay further capture attempts by 250, 500, 1,000 and then at
most 2,000 ms. `TryLockFrame` checks this **under the binder and before readback**,
using one requested-running snapshot. There is no sleeping capture thread or
unbounded queue. Camera preview and paused/offline GL work continue; the face path
does not use this volume cooldown. A successful update/commit or explicit Resume
resets it. Invalid/empty input does not falsely clear the previous rejection.

Wrapper rejection logs are rate-limited. `RECON_PAGING` records the chosen plan;
`RECON_CACHE` reports logical/resident chunks, resident/high-water payload bytes,
backing bytes, I/O/eviction counts and `preview_mesh_bytes` with each 30-committed-
frame timing window. Preview bytes exclude container overhead, AR/GL allocations,
image buffers and temporary export copies.

## Integration checks

```sh
python3 tests/capture_io/backoff_run.py
python3 tests/capture_io/context_paging_run.py
python3 tests/capture_io/cloud_run.py --modern
python3 tests/capture_io/cloud_run.py
python3 tests/control_save/run.py
python3 tests/texturing_flow/check_android.py
```

After the actual Android backend archive has been built, the target-device smoke
runner checks generated 2 cm planes in RAM-only and paged modes, including growth
beyond 1,024 chunks and reload/extraction under a small resident budget:

```sh
python3 tests/capture_io/android_paging_smoke.py --serial DEVICE \
  --sdk /path/to/android-sdk --archive /path/to/libscanner_reconstruction.a
```

This uses shell-UID isolated scratch and generated geometry only. It is not a
camera/whole-app speed test and does not access the user's captured datasets.

The Pixel ARM64 smoke test passed using the production backend archive: 1,560
logical chunks, 165,120 faces, **6 MiB peak resident payload**, 146.25 MiB backing
extent, 1,560 writes, 1,560 verified reads and 3,056 evictions. RAM and paged modes
produced the same geometry digest (`878d76a996ebb9b7`). The generated test took
523.92 ms in RAM mode and 863.61 ms with the deliberately tiny paged cache. It
proves target-side eviction/reload and memory accounting, not a capture-FPS gain.

Checks cover bounded cooldown/reset, admission lock release, paused/face work,
nonblocking UI controls, memory/storage planning, context-start failure cleanup,
fixed replay budgets, cache-read recovery classification and normal transactional
rejection. SDK boundaries in wrapper fixtures are explicit fakes; they do not
substitute for engine eviction/reload/rollback tests. The SDK/NDK check also compares
the generated Java capture JNI declaration against the actual native handler.

A second geometry-only snapshot of the cap-triggering private capture was frozen
without images or device mutations:

- `/tmp/opencode/recorded-private-227-capacity-20260930`
- 227 frames, 1,681,144 observations, 455 files, 27,001,751 bytes.
- SHA-256: `4870bbed7b93bcf280251797579364cdbebfe9fb07e9a28f59699570c347b375`.

Its rejected live frames were never committed, so that snapshot alone cannot
prove added coverage past the old cap. The older 451-frame fixture and forced-
growth tests are needed for that comparison. Its stored schema also lacks the
capture configuration; specify replay settings explicitly.

## Final caller-policy validation

The engine's original 256-changed-chunks/frame defaults accepted 443/451 frames
in the fixed 2 cm recording in both RAM and paged modes. The same eight frames
hit that separate limit. After selecting the budgeted 512-chunk allowance for the
96 MiB application profile, **all 451 frames were accepted by both modes**.

The comparison used a 4,096-logical-chunk RAM reference and the same cap for the
paged run; the application policy can permit 7,680 with sufficient free disk.
The work cap stayed at 32 million. This is the fixed experimental replay config
(color disabled, recorded original settings unknown), not a live-camera test.

- Final logical volume: **2,342 chunks**.
- Paged peak voxel payload: **96 MiB**; backing extent: **132.47 MiB**.
- **58,958,587 ordered mesh bytes matched exactly**, with 1,298,500 vertices and
  2,310,724 faces. This exceeds the separate current texturing budget; no textured
  export success is claimed.
- Digest: `0bd1ec9fb37e5f9ea406512e8f7305309d8dcc9077dfe318e199432a45fd5f9e`.
- Host replay-process peak RSS: RAM 298,652 KiB; paged 163,144 KiB (about 45% lower).
  Later mesh/BVH analysis peaks were approximately 575,600 KiB in both modes.
- Host replay time: RAM 153.951 s; paged 187.483 s (about 22% longer). This is a
  memory/coverage tradeoff, not a phone-FPS improvement claim.
- This trajectory caused 1,413 writes/evictions and no cold reads. Revisit/reload
  correctness was checked separately by host fault tests and the ARM64 smoke test.

Reports: `/tmp/opencode/paging-policy512-{ram,disk}-20260930/results.json`.
Reproduce with `recorded.py run --resolution 0.02 --max-chunks 4096
--max-update-chunks 512 --analysis-max-faces 2500000`; add `--paging
--resident-mib 96 --backing-mib 1024 --max-logical-chunks 4096` for the paged run.
`paging_compare.py` checks the complete ordered output, not just the digest.

The normal and sanitizer engine/fault suites, modern/legacy wrapper checks,
JNI/SDK/NDK contracts, modern APK assembly, signature, zipalign and static 16 KiB
package checks passed. A fresh normal scan on the candidate remains necessary to
measure end-to-end camera performance and whole-app memory on the Pixel.

## First installed scan: real offloading confirmed

The user installed APK `2783ed30308ebe950b3b010854057e621f8cd61babcc578ddc9f7bc86e22e8ab`
and performed another scan. Read-only inspection confirmed:

- **501 committed frames**, 360×640; current settings were 2 cm resolution,
  4 m depth, noise 9 and clearing enabled.
- Latest committed preview headers contained **1,951 nonempty mesh segments**,
  1,625,236 vertices and 2,853,241 faces: clearly beyond the previous 1,024-block
  volume ceiling. Preview vertex/normal/color/index payloads total **76.05 MiB**.
- Capture process PID 5856 retained an open, unlinked `scanner-tsdf-*` backing
  descriptor with **206,635,008-byte extent (197.06 MiB)**. Unlinking is deliberate;
  the live descriptor remains usable. Extent includes reusable slots and is not
  a count of currently cold payloads.
- First/last committed `.tms` acquisition times were 3.131435 and 189.514285 s:
  500 accepted-frame intervals across 186.382850 s, approximately **2.68/s**.
  This includes any pauses/tracking gaps, is not preview FPS, and is not an
  isolated before/after speed comparison.

`CAPTURE_MS`, `RECON_CACHE` and startup-policy lines had aged out of the retained
Android main-log buffer; only the last 17 image-write messages remained on the
first inspection. Exact stage times, live resident high-water and total eviction/
reload counters therefore cannot be claimed for this scan. The open backing file
and committed mesh inventory provide independent evidence that paging activated
and capture progressed beyond the old cap.

An inspected process memory sample included substantial swap after backgrounding,
so it is not a controlled PSS comparison with earlier foreground captures. Android
also recorded a separate FileManager input-dispatch ANR at 00:54:31.190 device time
(2026-10-01), PID 5257, before this capture process. PID 5856 remained alive during
inspection. Historical crash-buffer entries from earlier builds are not attributed
to this scan.

The 2.85-million-face mesh exceeds the current 500,000-face texturing budget;
successful larger capture does not establish textured-export success. Follow-up
work should preserve bounded per-scan telemetry and address large-model export /
simplification as well as the separate library startup ANR.
