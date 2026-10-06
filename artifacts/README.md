# Scanner build archive

## Current installed and user-validated baseline: original reconstruction engine

`3DLiveScanner-modernized-debug-2026-09-29.apk`

- SHA-256: `0b1e81ecfcaabcf04494de38ff98754b1ca0e4abf7d99cdfdebb9476b1f51d0e`
- Restored in place on 2026-10-01 after the experimental builds continued to
  produce oversized scans, floating fragments and damaged geometry on revisits.
- Contains the original vendor `libtango_3d_reconstruction.so`; this is the
  archived earlier modernization APK, not the replacement reconstruction engine.
- Backed up 1.25 GB of private files/preferences. Installed identity, private
  capture-state preservation and all 11 saved-library entries were verified.
- Restored 2 cm resolution, 4 m camera measurement range, clearing on and the
  original offset setting. No uninstall or data clearing was performed.
- The user completed a fresh scan and reported **“Quality is back.”** Post-test
  PID 9857 had an empty scoped crash buffer.
- **Leave this baseline installed. The experimental working tree is retained
  for investigation and must not be automatically redeployed.**

See [rollback evidence, backups and acceptance](../docs/quality-rollback.md).

## Separate original-engine Quality Test candidate

`3DLiveScanner-quality-isolated-debug-2026-10-01.apk`

- SHA-256: `70d06bac1170e3067bd5aa77e6d7ffcc00277f7d150b55a0583136e6e2731bbe`.
- Separate package `com.lvonasek.arcore3dscanner.quality`, separate private
  capture/library storage; the original baseline remains installed.
- All nine original native libraries are byte-identical. The bridge adapts the
  Java/JNI boundary; experimental reconstruction and TPU correction are inactive.
- User reports **“The same quality feels stuttery.”** Quality feedback is
  positive; smoothness and complete save/reopen/export acceptance are outstanding.
- The `quality` flavor is now the default build. Debug frame-stage profiling is
  described in [candidate evidence](../docs/quality-modernization.md).

### Quality Test pacing diagnostics

`3DLiveScanner-quality-pacing-debug-2026-10-02.apk`

- SHA-256: `66fc40e2f3f213a27ed1f90c97a520afd11111855096a50f6bed86818a6c9f90`.
- Debug-only bounded frame timings for the Java callback, native draw and EGL
  swap. This is a diagnostic update, not a claimed stutter fix.
- All ten native libraries, including the compatibility bridge, match the
  preceding isolated candidate byte-for-byte. Timing/JNI tests, build, immutable
  library audit, signature and ZIP alignment checks passed.
- Installation and device verification are recorded in the matching JSON.

The entries below are historical experiments, not the current quality baseline.

## Exact normal cache and NNAPI lifetime fix (superseded)

`3DLiveScanner-modern-normal-cache-debug-2026-10-01.apk`

- SHA-256: `df460256bb3aeb04e01d4378c3f08ad2d0a1741eadd9cf8abdd81c11e06f370d`
- Installed after the user's saved-scan confirmation. Installed identity and
  capture commit-state preservation passed; startup reached FileManager. The
  scoped crash buffer for PID 27034 was empty.
- Caches exact field gradients within each extraction and skips cells lacking
  a required shared diagonal endpoint. The accepted partial-cell output is
  preserved: 186,582,666 generated mesh bytes and a 484-frame real replay match.
- Pixel 24-frame / 2 cm / clearing-off ABBA measured 4.35% less extraction time
  and 2.82% less total native reconstruction work. This is not a full-scan FPS claim.
- Retains a process-lifetime optional NNAPI library mapping, fixing a reproduced
  teardown crash in the standalone TPU probe. Runtime/oracle/ABI tests passed.
- TPU correction, coarse preview and clearing remain off; capture-first remains
  on. Tested small-operation TPU mesh offload was slower than CPU and is not
  integrated. No new neural model was trained or enabled.
- Core, paging, partial-cell and runtime sanitizer checks, recorded-data
  comparison, modern build and package audits passed.

See [normal-cache evidence](../docs/normal-cache-performance.md) and
[TPU capture-path measurements](../docs/tpu-capture-acceleration.md).

## Partial-cell meshing repair (superseded; later moving-scan quality failed)

`3DLiveScanner-modern-partial-mesh-debug-2026-10-01.apk`

- SHA-256: `5b95a748b16c98d387ceec38c655bcd47561ad01bb3946610022573d7e8e29fa`
- Installed after the user's saved-scan confirmation. Installed identity and
  capture commit-state preservation passed; startup reached FileManager. The
  scoped crash buffer for PID 9667 was empty.
- Keeps fully observed tetrahedra even when other corners of their containing
  cube lack data. Unknown/under-threshold required corners still prevent meshing.
- The 484-frame recorded replay recovered about 10% more surface and reduced
  unpaired chunk-plane edges by about 33%, without new nonmanifold edges. It is
  not a watertight reconstruction; some analytic boundary errors increase modestly.
- The user tested a fresh live scan and reported **“Clearly improved.”**
- Full-detail preview, TPU correction off, clearing off and capture-first saving
  remain selected. Core/paging/partial-cell sanitizers, analytic scenes, build
  and package checks passed.

See [repair, evidence, tradeoffs and user acceptance](../docs/partial-cell-meshing.md).

## Preview stability trial (superseded; insufficient by itself)

`3DLiveScanner-modern-preview-stability-debug-2026-10-01.apk`

- SHA-256: `b82c43b162a0bd364084b619301bf177740aaa3c3cda5920a6101d23cf451fae`
- Installed after the user's saved-scan confirmation. Installed identity and
  capture commit-state preservation were verified; startup reached FileManager.
- Responds to visible holes/fragmentation in the live preview: coarse coverage
  preview defaults off; the installed trial has TPU correction and free-space
  clearing off, with capture-first saving retained.
- Native libraries are byte-identical to the preceding fusion-cache build;
  geometry-equivalent speed improvements remain.
- A 484-frame geometry replay with clearing off retained about 22% more area at
  2 cm, but open-edge count also increased. This is a stabilization trial, **not
  a confirmed complete repair of the visible holes**.
- Build, capture-policy and package audits passed. Fresh-scan visual acceptance
  failed: the user reported the preview was still badly fragmented.

See [diagnostic evidence and trial limits](../docs/preview-stability-trial.md).

## Fusion voxel-access update (previous build)

`3DLiveScanner-modern-fusion-cache-debug-2026-10-01.apk`

- SHA-256: `133ea10f8ecec16f204cd500255d36980701eb37b3c26a7d66241b021953e7f5`
- Installed in place after the user's saved-scan confirmation. Installed bytes
  matched, the capture commit-state check passed, and startup reached FileManager.
  The scoped crash buffer for PID 27067 was empty.
- Reuses the existing voxel-chunk pin instead of repeating eviction bookkeeping
  for neighboring accesses. No new cache memory, changed fusion arithmetic or
  reduced recording quality.
- Pixel 24-frame / 5 cm / CPU-5 ABBA: fusion time fell 16.68%; combined fusion and
  extraction fell 14.43%. All 38,861,461 output bytes matched. These are native
  reconstruction measurements, not whole-app FPS gains.
- A 451-frame / 4.7-million-point recorded replay produced identical geometry and
  paging statistics. Core/paging sanitizers, fault/rollback tests, build and all
  package audits passed. A larger coordinate-cache experiment was rejected.
- Includes the preceding capture-loop lock separation, coverage-preview mode,
  capture-first saving, TPU cleanup, 8 m preview range and privacy-popup removal.

See [implementation, measurements and reproducible checks](../docs/fusion-pin-reuse.md).

## Live capture-loop update (previous build)

`3DLiveScanner-modern-scan-loop-debug-2026-10-01.apk`

- SHA-256: `774723b75df88daf335a26691f600289a835f418bbf8c0cfe8234584c5333653`
- Installed after the user saved the previous scan. Installed bytes matched,
  the capture commit-state comparison passed, and startup reached FileManager.
- Separates completed capture publication from the camera/display lock. The
  preceding build spent about 15–27 ms/capture waiting for that lock while the
  actual mesh merge took about 0.1 ms. This also delayed next-frame admission.
- Adds actual committed-capture cadence telemetry (`interval`, `capture_hz`).
- Captured data, quality settings, TPU backups and commit ordering are retained.
- The next live scan confirmed 2 cm requested detail / 5 cm coverage preview and
  active native-size TPU cleanup. Publication wait fell to 0–0.01 ms; ordinary
  windows reported 6.8–14 captures/s. A separate long-gap window reported 0.72 Hz.
  This is an uncontrolled observation, not a fixed FPS-gain claim. The scoped
  process crash buffer was empty.
- Production-worker and preview-buffer concurrency tests, modern/legacy failure
  checks, sanitizers, lifecycle, native contracts, assembly and package audits
  passed. Live results and remaining reconstruction costs are documented below.

See [capture-loop implementation and evidence](../docs/live-capture-throughput.md).

## Capture-first speed update (previous build)

`3DLiveScanner-modern-capture-first-debug-2026-10-01.apk`

- SHA-256: `b4e938a73fb9fdf05ba1cb93dee2e7e398120429bc7bd0a9b43cae95bbd5941e`
- Installed in place on the Pixel 9 Pro XL after the user confirmed the capture
  was saved. Installed APK bytes matched; the capture commit-state check passed.
  Startup reached FileManager. The scoped crash buffer for PID 18223 was empty.
- Fast coverage preview, capture-first saving and experimental TPU cleanup are
  enabled. A subsequent live scan logged `google-edgetpu active native 160x90`.
- New supported captures use a 5 cm live proxy while retaining requested-resolution
  measurement preparation and recorded inputs. Full model generation uses the
  requested detail. Raw dataset saving skips the redundant OBJ export.
- Exact TPU preprocessing uses a row cache; the tested feature bytes, masks and
  corrected points match the previous implementation. Original-input backups and
  unchanged-depth fallback remain enabled.
- Includes the 8 m preview range and removal of the automatic privacy popup;
  privacy information is available manually in Settings.
- The controlled Pixel preview-core benchmark measured 55.16% less combined work
  at 1 cm versus 5 cm. This is not a whole-app FPS measurement. Live logs confirm
  execution, not a controlled performance or accuracy comparison.
- Modern assembly, legacy Java, capture/save/lifecycle/NDK checks, static 16 KiB
  package audit, zipalign and signing checks passed. Live save/reopen/full-detail
  processing of a new coverage capture remains an end-to-end acceptance step.

See [implementation, measurements and installed-device checks](../docs/capture-speed-update.md).
The entries below describe earlier artifacts and their verification at the time.

## Experimental live TPU build

`3DLiveScanner-modern-tpu-test-debug-2026-10-01.apk`

- SHA-256: `393530bb7bb74229af3257c9a0d285ad720232fc278073e55fc9a111022811cc`
- Installed in place and **enabled** at the user's request. Installed identity and
  capture/dataset preservation were verified; startup reached FileManager.
- Actual live logs confirm `google-edgetpu active native 160x90`, with thousands
  of corrected measured points per frame. A committed frame-1,247 backup contains
  the expected model hash, original/corrected world points and packed model input.
- Worker-only inference, same-frame point mapping, generation invalidation,
  bounded residuals and original-depth fallbacks. No depth resizing or new hole
  filling. Modified frames retain original-input sidecars before fusion.
- Toggle: Settings → Realtime mode → Scan parameters → Experimental TPU depth cleanup.
- This is an explicitly enabled experiment, not a production-quality promotion.
  See [implementation, live evidence and backup format](../docs/experimental-tpu-test.md).
- Modern build, legacy Java, runtime/capture/lifecycle/NDK tests, 16 KiB package
  checks, zipalign and signature verification passed. NNAPI remains optional at
  load time; no mandatory NNAPI dependency was added.

## Captured-dataset export repair

`3DLiveScanner-modern-export-fix-debug-2026-10-01.apk`

- SHA-256: `32d9a4561ce864aa7dc0cee49702c9104e193afe1327d5e3391d59511a0f8040`
- Installed in place on the Pixel. Installed bytes matched, private capture state
  and saved-dataset/model fingerprints were preserved, startup reached FileManager,
  and the scoped crash query for PID 3341 was empty.
- Default modern captured-data export uses disk-streaming projective texturing,
  bypassing the old 32 MiB OBJ, 500k-face and small-material-count loader limits.
  The real 724 MB / 263-frame source exported all 2,519,369 triangles in a native
  on-device test, with about 56 MiB peak process RSS and 75 seconds backend time.
- Photo assignment reached about 74% of faces / 71% of surface area; remaining
  faces retain neutral material. Four 2048² pages required 182×323 photo tiles.
  Output has five material groups, avoiding per-triangle render batches.
- Source geometry is retained. Atomic final publication includes orientation
  validation; failed exports remove generated staging/resources and keep previous
  output. Errors distinguish reconstruction from texturing failures. Offline
  texturing releases the render lock and prevents camera restart while running.
- Includes the finalized, correctness-verified normal dirty-write guard, core
  `548b87ef7a4cdeab4587f38ce96cbe2895c48eefbcf3cd77510a4089b1a88842`.
  No additional Pixel speedup is claimed for that guard. All implementation agents
  have completed their handoffs.
- Final sanitizer/core/paging/visibility/failure tests, actual device backend export,
  modern/legacy API and Java checks, complete modern build, static 16 KiB audit,
  zip alignment and signature checks passed. Full user-operated UI export and a
  true 16 KiB-kernel run are separate acceptance steps.
- TPU experiments remain standalone research; this APK contains no neural model.

See [export implementation, evidence and limits](../docs/large-model-texturing.md).

## Model-generation candidate

`3DLiveScanner-modern-generation-debug-2026-10-01.apk`

- SHA-256: `b739face1d44b808d6c529e3cacdd616dd9ccd9bff6db7ec42f0cb036b495458`
- Installed in place on the Pixel 9 Pro XL at the user's request. Installed APK
  digest matched; the private 501-frame capture commit state was byte-identical
  before and after installation. Startup reached `main.Main` (PID 14303), and
  the scoped crash-buffer query for that process was empty.
- Includes conservative mesh reduction, direct lattice-edge indexing, shared
  TSDF-gradient shading normals and paged-only per-ray fusion batching.
- The Pixel generated paged workload measured about 9.3% lower fusion/extraction
  sequence time. Nine-scene extraction totals fell about 30% at 2 cm and 26% at
  4 cm, but RAM-only total times regressed; a separate update-overhead refinement
  is still being worked on and is **not included in this installed artifact**.
- Both variants accepted all 501 frames of the frozen real recording within the
  96 MiB voxel-residency budget. Exact open-edge geometry matched all 1,972 mesh
  segments, with zero exact-degenerate/nonmanifold/winding errors. Triangle count
  fell only 0.53%; the separate 500,000-face texturing limit remains.
- Independent analytic/sanitizer checks, real NDK/JNI caller checks, full modern
  assembly, all-library static 16 KiB/dependency audit, zipalign and signature
  verification passed. No true 16 KiB-kernel runtime test is claimed.

See [model-generation measurements and tradeoffs](../docs/model-generation.md).

## Disk-backed reconstruction candidate

`3DLiveScanner-modern-paging-debug-2026-09-30.apk`

- SHA-256: `2783ed30308ebe950b3b010854057e621f8cd61babcc578ddc9f7bc86e22e8ab`
- Installed in place on the Pixel at the user's request; Android reported success
  and a cold launch reached FileManager. A subsequent 2 cm scan committed 501
  frames and 1,951 nonempty mesh segments. An open paging file with 197.06 MiB
  extent confirmed real offloading beyond the old cap. Timing/cache summaries
  aged out, and a separate pre-capture FileManager ANR was recorded; see trial notes.
- Pages inactive TSDF/color chunks to private, disposable disk backing and loads
  them on revisit. Payload residency includes staging/loading/pins and is bounded.
  The 2 cm / ample-storage profile uses 96 MiB resident, at most 1 GiB backing,
  7,680 logical chunks and a budgeted 512-changed-chunks/frame allowance.
- Rejected frames back off before GPU readback/integration; explicit Resume and
  successful updates reset the delay. Corrupt/unreadable backing requests history
  replay, whereas safe transactional rejection retains the valid volume.
- The fixed 451-frame 2 cm comparison accepted all frames and matched the equally
  configured RAM reference byte-for-byte, with 2,342 logical chunks and 96 MiB
  peak voxel payload. Replay-process RSS fell about 45%; host replay took about
  22% longer. These are not whole-app RAM/FPS claims.
- The Pixel ARM64 generated test crossed 1,024 chunks, held payload to 6 MiB,
  forced eviction/reload and matched RAM-only geometry. No user scan was modified.
- Engine normal/sanitizer/fault tests, caller/recovery/admission checks, generated
  JNI contract, both Java flavors, modern APK build and all-library 16 KiB,
  dependency, zipalign and signature checks passed.
- Preview meshes/export still consume memory. The separate texturing face budget
  and end-to-end large-scan/export validation remain outstanding.

See [integration and measured limits](../docs/reconstruction-paging.md) and
[pager mechanics and fault tests](../tests/reconstruction/paging_README.md).

## Internal-workspace performance candidate

`3DLiveScanner-modern-workspace-debug-2026-09-30.apk`

- SHA-256: `3e4c1555a0c8d7f849bdd2c7628acebc6f93518e8b7c574baaeb8c47db7f3a74`
- Installed in place on the Pixel at the user's request; Android reported success
  and a cold launch reached FileManager. Installed hash matched. A later capture
  committed 227 frames in the private workspace and reported 34.13 ms persistence/
  publication, but also hit the 1,024-chunk reconstruction cap at a current 2 cm
  setting. Overall capture acceptance is incomplete; see the workspace trial notes.
- Fresh modern captures use persistent private working storage; finished models
  keep the existing library. Existing public/private pending captures are found
  before new working storage is selected, without automatic migration/deletion.
- Raw dataset finalization stages a byte-verified copy in the library, publishes
  it by rename, and only then releases the source. Editor/share scratch and
  thumbnail publication handle the filesystem boundary separately.
- Both flavors' Java/resources compiled. Modern APK assembly, 16 KiB/dependency
  audit, zipalign and signature verification passed. The native library matches
  the preceding build byte-for-byte.
- Host storage/selection/failure regressions passed. Synthetic phone I/O measured
  private writes at 6–8 ms/frame, versus 81–106 ms through shared storage in the
  shell comparison. These omit AR, JPEG encoding and rendering, and do not prove
  a capture-FPS gain. Full-app capture/save/recovery acceptance remains pending.
- A separately frozen real recording (451 frames, 4.7 million observations) now
  provides a repeatable geometry baseline. Two optimized replays and a full
  sanitizer replay produced the same ordered mesh digest. The benchmark uses an
  explicit experimental 4 cm configuration and measures self-consistency, not
  ground-truth scan accuracy or texture quality.

See [workspace design, measurements and tradeoffs](../docs/capture-workspace.md)
and [the fixed geometry replay harness](../tests/reconstruction/recorded_README.md).

## Modern parallel-persistence performance update

`3DLiveScanner-modern-parallel-io-debug-2026-09-30.apk`

- SHA-256: `2444df0c07967828f20002488e1a3deb7506f4a868b730c03dd9f66244605c14`
- One joined image/metadata writer overlaps independent preview/cloud writes.
  State is published only after both succeed; capture inputs remain owned until
  the task joins. Thread/allocation failure falls back to serial execution.
- Device-side synthetic persistence measurements averaged 81.069 → 69.545 ms
  (14.2% lower). The probe runs as adb shell and is not a whole-app FPS benchmark.
- Writer concurrency/fallback checks, modern and legacy worker failure checks,
  real NDK/SDK compilation, full modern APK assembly, all-library 16 KiB audit,
  dependency isolation, zipalign and signature verification passed.
- Installed in place after the user confirmed the prior scan was saved; installed
  hash matched. A subsequent user capture committed 43 frames and paused normally.
  One 30-frame report measured persistence/publication at 122.05 ms with
  `io_parallel=100%`, versus 136.98 ms across the prior trial's three reports.
  Input point counts differed, so this is not a controlled whole-app comparison.
- Additional local-Z cache experiments were rejected for inconclusive gains.
  The reconstruction core remains byte-identical to the preceding repair source.

See [persistence evidence and timing semantics](../tests/capture_io/parallel_writes.md),
[rejected core experiments](../tests/reconstruction/core_localz.md), and
[modern/legacy feature tradeoffs](../docs/scanner-build-variants.md#feature-tradeoffs).

## Modern recovery/performance repair

`3DLiveScanner-modern-recovery-debug-2026-09-30.apk`

- SHA-256: `9332601242df7cc6e1698b6c5bcdc748104ecd02ff19df430550c82d1f54a90d`
- Keeps the valid owned volume after transactional frame rejection instead of
  replaying history into the same resource limit. The UI reports a skipped frame;
  extraction/persistence failures after successful integration still need recovery.
- Activity pause now prevents background camera restart, including after GL
  surface recreation, while keeping offline GL work available.
- Bounded chunk/neighborhood caches preserve exact mesh output in the tested
  host comparisons. Core call time fell 17.87% with clearing/color and 35.03%
  without them. These are host measurements, not phone FPS. See
  [the full methodology and results](../tests/reconstruction/core_performance.md).
- Modern/legacy worker rejection tests, control/save and camera-admission tests,
  analytic/rollback core tests, exact-output differential tests, real-codec
  texturing and legacy-format synthetic dataset integration passed.
- Full modern build, all four packaged libraries' strict 16 KiB audit, dependency
  isolation, Android zipalign and APK signature checks passed. The phone reports
  4 KiB kernel pages; a true 16 KiB-kernel run remains separate.
- Installed in place on the Pixel after the user confirmed the previous scan was
  saved. The installed APK hash matches the artifact. Startup showed no system
  compatibility/crash dialog. A user-operated short capture committed **98 frames**
  and paused normally, with no reconstruction/resource-limit/AndroidRuntime error
  in the scoped observation. Three timing windows measured about **2.90 committed
  frames/s** between reports; this is not a controlled speedup comparison. See
  [device timing details](../tests/capture_io/README.md#modern-repair-device-check).

## Modern 16 KiB migration candidate

`3DLiveScanner-modern-16kb-debug-2026-09-30.apk`

- SHA-256: `76f52ade49615b7fb1cfe487a70a0665675fa9828d11e1db402d9a7161c5cefb`
- Source-built reconstruction/texturing, ARCore 1.56.0, NDK r28c; modern variant
  excludes the retired Tango/Huawei/GVR native dependencies.
- Full modern APK build, strict four-library LOAD/RELRO/ZIP/dependency audit,
  Android zipalign and APK signature verification passed.
- Installed in place through wireless ADB on the Pixel 9 Pro XL (Android 17).
  Startup reached the library; UI inspection found no system compatibility/crash
  dialog and the app-specific crash/linker log query was empty.
- The phone was reporting 4 KiB system pages. This startup check does not establish
  a true 16 KiB kernel run, capture quality or end-to-end textured export. Those
  remain device-validation steps. See `reconstruction/MIGRATION.md` and backend
  test documentation for implementation bounds and experimental quality status.

All implementation subagents finished before the installation handoff closed.

## Capture-speed update

`3DLiveScanner-capture-speed-debug-2026-09-29.apk`

- SHA-256: `5a8aedba5ca9e0608916c547202335427d1c26c0dce5452d587d6f4da74ef4a0`
- Same application ID, version metadata and development signing certificate as
  the earlier test APK below; APK signature verification passed.
- Native C/C++ rebuilt with effective `-O2`, including reconstruction and JPEG
  codec code. Debug symbols/assertions are retained; fast-math is not enabled.
- Dense capture defers unused sparse projection work, disabled wall estimators
  skip unused preparation, and packed YUV conversion avoids discarded chroma work.
- Capture settings, point density, confidence thresholds and JPEG quality are
  unchanged. `CAPTURE_MS` logs provide aggregate timings for 30 committed frames.
- Full `:app:assembleDebug`, geometry/recovery, image/scene and control/save
  regressions passed. Same-session Retango differential/sanitizer checks and
  image conversion equivalence checks passed.

See [capture checks and timing definitions](../tests/capture_speed/README.md) and
[Retango measurements](../tests/capture_speed/retango_README.md). Host component
timings are not Pixel FPS results. No device was attached for this build's
end-to-end capture measurement.

## Earlier modernization build

`3DLiveScanner-modernized-debug-2026-09-29.apk`

- Application ID: `com.lvonasek.arcore3dscanner`
- Build: debug, ARM64, minimum Android API 24
- Existing version metadata retained: `220022` / `2022-build_0022`
- APK signature verification: passed (v2, development signing)
- SHA-256: `0b1e81ecfcaabcf04494de38ff98754b1ca0e4abf7d99cdfdebb9476b1f51d0e`

The final `:app:assembleDebug` passed using Gradle 8.0, AGP 8.1.1, JDK 17,
Android SDK 33 and NDK 25.1.8937393. Build outputs and Gradle state were relocated
to Linux temporary storage for verification because the Windows-mounted output
directory produced a generated-resource read error. Production source and
dependency versions were not changed for that workaround.

Host geometry, mesh, alignment, performance, storage, thumbnail, lookup and
control/save regressions passed. The seven Android sharing boundary tests and
their NewApi lint check also passed. See the repository README for test commands
and mocked-boundary limitations.

This APK has not been installed or tested on the reported Pixel 9 Pro. It is a
functional test build, not a measured production-performance result. A development
signature does not establish compatibility with an existing store-signed install.
App-owned libraries follow Android's app-data lifecycle; use local model sharing
for copies outside the app.

Existing build warnings remain, including deprecated SDK calls and duplicate
Huawei native-library inputs. The Huawei inputs have different build IDs, and
Gradle currently selects the application's native input. Huawei runtime
compatibility has not been established by these checks.
