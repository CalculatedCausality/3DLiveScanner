# Bounded voxel paging: API, safety contract and evidence

Paging is **opt-in**. The RAM-only default, its `max_chunks` range/default,
resolution, fusion arithmetic, observation/traversal order, work/frame limits,
dirty ordering, extraction and output ownership are preserved. This implementation
pages actual TSDF/color payloads, not just preview meshes or placeholder geometry.

## Public extension (`reconstruction/paging.h`)

```c
Tango3DR_Status ScannerReconstruction_enablePaging(
    Tango3DR_ReconstructionContext ctx, const char* cacheDirectory,
    uint64_t residentBudgetBytes, uint64_t backingBudgetBytes,
    uint32_t maxLogicalChunks);

Tango3DR_Status ScannerReconstruction_getPagingStats(
    Tango3DR_ReconstructionContext ctx,
    ScannerReconstruction_PagingStats* stats);
```

Use only handles from the modern source backend. All access to a context, including
stats and destruction, must be serialized by the caller. These are C-compatible
interfaces; no private reconstruction/config structs are shared with callers.

Enable is accepted only for an **empty, not-already-paged context**. It does not
change calibration or ordinary config. The directory must exist; the engine does
not choose/create a directory or fall back automatically. Invalid arguments return
`INVALID`; cache-open or allocation failure returns `ERROR`, with RAM mode intact.
Callers decide fallback, free-storage policy and budgets.

Enable creates a private `mkstemp` file (0600), sets `FD_CLOEXEC`, immediately unlinks
it and retains only the descriptor. Creation/unlink failure is explicit. Context
destruction closes only that descriptor; it never accesses the dataset. There is
no mmap, persistent cache recovery, disk-index file, image I/O, or device mutation.

Stats require **no allocation, I/O or paging** and do not clear diagnostics:

- `logical_chunks`: committed logical keys, including disk-only records.
- `resident_chunks`, `peak_resident_chunks`, `resident_bytes`,
  `peak_resident_bytes`: live voxel payloads, including COW staging and page-load
  buffers/pins. Queries are at serialized API boundaries; peaks include transients.
- `backing_bytes`: observed file extent including reusable free slots/partial
  writes; `backing_live_bytes`: slots currently reserved by live records, including
  resident dirty records whose slot is not presently valid.
- `reads`, `writes`: completed, checked payload reads/writes; reads count only
  after checksum verification. `evictions`: successfully offloaded resident payloads.
- `last_failure`, `requires_replay`: see below.
- `chunk_bytes`, `resident_budget_bytes`, `backing_budget_bytes`: exact payload
  unit and caller budgets. In RAM mode backing/budgets/IO counters are zero, while
  resident counts/bytes and RAM COW peak are still reported.

Counters/peaks are cumulative within the enabled pager's lifetime. `clear` releases
all logical records/payloads and resets the replay/failure flags. It keeps the
descriptor, cumulative counters and freed slots/file extent for bounded reuse.

### Budgets

**`sizeof(Chunk) = 98,304 bytes`**, statically checked: 4096 voxels × six floats.
No quantization/compression is added. Resident capacity is the budget rounded down
to whole payloads. **Minimum: eight payloads = 786,432 bytes (0.75 MiB)**, enough
for the complete 8-neighbor extraction pin set. A COW copy needs two simultaneous
payloads, and both are charged. Loading buffers are charged before reads.

The byte budget is for voxel payload allocations, not allocator overhead,
record/map metadata, returned meshes, caller preview caches, Java/GPU memory or OS
file cache. Metadata is separately bounded by logical and per-frame limits.
This is **not a whole-app RAM/PSS bound**.

`maxLogicalChunks` accepts **1..32768** only in the explicit extension. Paging
uses this logical cap instead of the ordinary `max_chunks` config value. It does
not raise that config's RAM-only maximum of 4096/default of 1024. The 256 changed
chunks/frame and 32-million work defaults are unchanged.

Disk slots are one payload each. The slot arena is bounded by the smaller of the
backing budget and `(logical limit + max_update_chunks) * sizeof(Chunk)`; an offset
must also fit the platform's `off_t`. Thus at the hard logical/frame maxima it is
at most 33792 slots. File extent never exceeds the caller budget. Freed slots are
reused before extending the file. Backing must be at least one payload; a small
backing budget may legitimately reject operations even below the logical cap.

Old committed versions and new COW versions can coexist until commit. Disk and
resident budgets cover both; a logical cap alone does not promise every operation
will fit the supplied resident/backing budgets. No cap automatically increases.

## Storage, LRU and commit mechanics

- Paged contexts use stable shared records in the logical map. Payloads are
  separately owned and may be resident or backed by a checked slot.
- Intrusive LRU links move on payload access. Presence-only ray traversal consults
  records without loading payloads. Inactive records remain logical members after
  their payloads are offloaded. Revisit loads the exact saved bytes automatically.
- Transaction caches hold stable map values/records, not evictable raw payloads.
  A current-access pin protects an integration payload; a source pin protects
  the entire old payload while a COW copy is allocated/copied.
- Extraction pins all present neighbors for the lifetime of its 17³ halo and
  tetrahedron reads. No halo pointer can outlive its pin. Pins unwind on failure.
- Updates copy the metadata map and clone changed records. Old committed records
  remain authoritative until every operation and dirty-output allocation succeeds.
  Commit is the original no-throw map swap. Rollback destroys only staged versions,
  releasing their slots and payloads without allocations or disk reads.
- A staged version can itself be paged. The resident budget therefore does not
  have to pin an entire frame's changed set. Writes invalidate that version's old
  backing copy, while the distinct committed version remains available for rollback.
- Writes use checked positional `pwrite` loops, including short writes/EINTR.
  The engine never releases a resident victim before a full successful write.
  On write failure its valid RAM copy remains and its incomplete backing is invalid.
- Loads use checked positional `pread` loops and a 64-bit FNV-1a checksum over
  every payload byte before exposing it. EOF, read errors and checksum mismatches
  poison the disposable cache explicitly. FNV is corruption detection, not a
  cryptographic authenticity mechanism.

Cache writes are buffered and not fsynced: this is disposable, process-local
scratch, not authoritative storage. Dataset persistence/durability policy is not
changed. A crash closes the anonymous file; reconstruction must come from dataset
history. No successful current frame is reverted merely because residency changed.

## Failures and caller integration

The public enum defines these exact names (prefix
`SCANNER_RECONSTRUCTION_FAILURE_`):

| Suffix | Value | Typical status / interpretation |
| --- | ---: | --- |
| `NONE` | 0 | Last operation succeeded/no named paging failure |
| `VOLUME_LIMIT` | 1 | `INSUFFICIENT_SPACE`, logical/RAM chunk cap |
| `FRAME_LIMIT` | 2 | `INSUFFICIENT_SPACE`, changed-chunk cap |
| `WORK_LIMIT` | 3 | `INSUFFICIENT_SPACE`, work cap |
| `POINT_LIMIT` | 4 | `INSUFFICIENT_SPACE`, point-count cap |
| `MEMORY_LIMIT` | 5 | `ERROR` on allocation failure; `INSUFFICIENT_SPACE` on pin pressure; `INVALID` for too-small enable budget |
| `BACKING_LIMIT` | 6 | `INSUFFICIENT_SPACE`, no backing slot; `INVALID` for invalid enable budget |
| `CACHE_WRITE` | 7 | `ERROR`, valid resident data retained, update rolls back |
| `CACHE_READ` | 8 | `ERROR`, **requires_replay = 1** |
| `CACHE_OPEN` | 9 | `ERROR`, enable failed, RAM mode intact |
| `BAD_ARGUMENT` | 10 | `INVALID`, enable arguments/context unsuitable |
| `INTERNAL` | 11 | `ERROR`, unexpected exception |

Read/checksum failure makes `requires_replay` sticky. Further update/extraction
calls return `ERROR` without using the suspect cache until explicit clear or
destruction. Stats remain available. Main must rebuild from authoritative dataset
history, not repeatedly treat this as a transient skipped frame.

Write/allocation/budget failure during an **update** leaves committed geometry,
timestamp and logical keys intact; dirty output is empty. A later viable frame
can proceed without replay. Residency, file extent and IO counters may have changed.
Repeated failed/replaced versions reuse slots instead of appending garbage forever.

**Important multi-call boundary:** a successful update stays committed if a later
mesh extraction fails. `requires_replay=0` says the cache is usable; it does not
assert that the parent's dataset/preview publication succeeded. Main must retain
its admission transaction/retry/replay logic for that higher-level boundary.
Extraction failure returns an empty destroy-safe output and does not change
logical geometry. Query stats immediately after the failing call; a subsequent
successful update/extraction resets `last_failure` but not the sticky replay flag.

Memory-only config behavior and member-only destruction contracts remain as
documented in `core_README.md`. Preview-mesh memory and index ownership remain the
caller's responsibility. No new whole-volume enumeration/export API is added.

## Verification

Run `python3 tests/reconstruction/paging_run.py` and the same with `--sanitize`.
Both passed, using the actual source core and real temporary-file IO. Tests cover:

- C-compatible signatures, enable argument/failure behavior, no named cache file,
  0600 permissions, CLOEXEC, descriptor closure, unrelated-file preservation,
  and allocation-free stats under an active allocation failpoint.
- Exact ordered geometry/normals/colors/faces/timestamps/capacities and dirty arrays
  against RAM mode through forced eviction, pose movement, clearing and revisit.
- Eight-payload residency while extracting a full eight-neighbor halo.
- **1100 logical chunks with eight resident payloads**, compared exactly with RAM,
  including revisiting the beginning. Peak payload memory 786,432 bytes; final
  backing extent 108,134,400 bytes. The paged context kept RAM config `max_chunks=1024`.
- Partial failed writes, repeated rollback with stable high-water slot extent,
  successful retry, and strict backing-limit failure without geometry loss.
- **35 paged transactional allocation failure positions**, plus cold-load/extraction
  OOM recovery and unchanged committed output.
- Injected partial read failures; actual backing-byte corruption and actual file
  truncation/EOF; mid-update read failures; sticky replay flags and pin cleanup.
- Logical/frame/work limit classification and rollback.

Existing full core suites passed under normal optimized compilation and
ASan/UBSan/leak checking, including the original 9/23/101 allocation sweeps.
RAM-disabled differential comparison with the exact pre-edit source matched
**40,182,852 ordered bytes** across seven six-frame scenarios/status sequences.
Pre-edit snapshot: `/tmp/opencode/core-before-paging-20260930/core.cc`, SHA-256
`97a16e50220b76c3baf124071f9f2b6e323814d5208a43f83f8837f2eb093e71`.

## Fixed real recording, 2 cm

Fixture: `/tmp/opencode/recorded-pixel-451-geometry-20260930`, 451 frames,
4,707,444 points; fixture SHA-256
`d1e255691f7982d18037d8e76603ca9ce157a07b496d54cf6cf9a6e890da5c4f`.
Color was disabled, using the existing geometry-only recorded runner and pose
semantics. No device or dataset payload was modified.

Reference: RAM mode with an explicitly **test-only** 4096 logical cap. Comparison:
paging with the same 4096 logical cap, **96 MiB resident / 1 GiB backing**. Both
kept the production frame/work limits. The host-only analysis face cap was raised
to four million (two million+ faces at 2 cm); the work/frame limits were not
relaxed, and resolution stayed at 2 cm.

Results matched **58,609,231 complete ordered mesh bytes**, plus statuses, dirty
counts, bounds/topology/residual diagnostics. Final output had 1,290,898 vertices,
2,296,835 faces and **2329 logical chunks**. Ordered mesh SHA-256:

```text
68dd1ffa14d9047200ab59c7b4dfb59db58c2698104864727ea2498e549e7634
```

**443/451 frames were accepted by both.** Zero-based frames
258,259,260,262,263,264,265,266 were identically rejected at the unchanged
256-changed-chunks/frame limit. Paging caused no additional rejection. This is
not a claim that every recording frame was integrated.

| Host measurement | RAM reference | Paged |
| --- | ---: | ---: |
| Replay wall time | 123.131 s | 146.506 s |
| Sum of update calls | 100.496 s | 122.614 s |
| Extraction/validation/replacement | 22.307 s | 23.708 s |
| Peak voxel payload bytes | 238,288,896 | **100,663,296** |
| Replay-process peak RSS | 297,028 KiB | **162,936 KiB** |
| Overall RSS including later mesh/BVH analysis | 572,340 KiB | 572,284 KiB |

Paged end state: 929 resident chunks, 1024 peak; 1400 writes and evictions;
137,625,600 bytes (131.25 MiB) backing extent/live backing. **Zero reads in this
recording**: its evicted cold chunks were not revisited. Forced disk reloads are
covered by the unit scenarios (one sequence recorded 1290 reads, 944 writes,
1438 evictions). Last failure/replay flags ended at zero.

The single-run host comparison demonstrates bounded live voxel payload and exact
output, with a measured paging CPU/IO cost. It does **not** demonstrate phone FPS,
whole-app RAM bounds, storage endurance or Tango parity. Preview/analysis memory
can dominate even when core residency is bounded, as the RSS figures illustrate.

Reports and full source hashes:

- `/tmp/opencode/paging-recorded-2cm-ram-20260930/results.json`
- `/tmp/opencode/paging-recorded-2cm-disk-20260930/results.json`
- `/tmp/opencode/paging-ram-default-differential-20260930/results.json`

Current implementation hashes:

- `core.cc`: `c185babd6be001dc1997475c7a93e228c63d7c7438b1a2c7cb0073ae87d8d0c6`
- `paging.h`: `d5a738a400484caa1b5c567fd2c3f88513d803ea553fbc670b63464fa7d91a1c`
- `paging_store.h`: `bc6ab23e0c33bdb262b8ddec86c7ae8599324e7f7f173373f29a9bda054d61de`

## Runner extensions and reproduction

Only `recorded.py` and `recorded_replay.cc` were extended for this comparison:
`--paging`, `--resident-mib`, `--backing-mib`, `--max-logical-chunks`, the explicit
RAM-reference `--max-chunks`, and host diagnostic `--analysis-max-faces`. Defaults
retain the previous RAM/config/analysis behavior. Paged runs create a private
`cache` directory under their new output directory and verify no named files
remain. Source manifests include both paging headers. Results include paging
stats and retain the unchanged ordered mesh format.

```sh
python3 tests/reconstruction/recorded.py run \
  --fixture /tmp/opencode/recorded-pixel-451-geometry-20260930 \
  --output /tmp/opencode/paging-ram-repeat --resolution 0.02 \
  --max-chunks 4096 --analysis-max-faces 4000000 --repeats 1

python3 tests/reconstruction/recorded.py run \
  --fixture /tmp/opencode/recorded-pixel-451-geometry-20260930 \
  --output /tmp/opencode/paging-disk-repeat --resolution 0.02 \
  --max-chunks 4096 --max-logical-chunks 4096 --paging \
  --resident-mib 96 --backing-mib 1024 --analysis-max-faces 4000000 --repeats 1

python3 tests/reconstruction/paging_compare.py \
  /tmp/opencode/paging-ram-repeat /tmp/opencode/paging-disk-repeat
```

Main owns private-directory/free-storage policy, Java/JNI, admission/error UI,
preview-memory policy, NDK/16 KiB packaging and device profiling. Paging is
synchronous and may stall on slow/failing storage; no latency guarantee is made.
