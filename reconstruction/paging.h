// Optional modern-backend extension. No changes to the Tango compatibility ABI.
#ifndef SCANNER_RECONSTRUCTION_PAGING_H
#define SCANNER_RECONSTRUCTION_PAGING_H
#include <stdint.h>
#include <tango_3d_reconstruction_api.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum ScannerReconstruction_PagingFailure {
    SCANNER_RECONSTRUCTION_FAILURE_NONE = 0,
    SCANNER_RECONSTRUCTION_FAILURE_VOLUME_LIMIT = 1,
    SCANNER_RECONSTRUCTION_FAILURE_FRAME_LIMIT = 2,
    SCANNER_RECONSTRUCTION_FAILURE_WORK_LIMIT = 3,
    SCANNER_RECONSTRUCTION_FAILURE_POINT_LIMIT = 4,
    SCANNER_RECONSTRUCTION_FAILURE_MEMORY_LIMIT = 5,
    SCANNER_RECONSTRUCTION_FAILURE_BACKING_LIMIT = 6,
    SCANNER_RECONSTRUCTION_FAILURE_CACHE_WRITE = 7,
    SCANNER_RECONSTRUCTION_FAILURE_CACHE_READ = 8,
    SCANNER_RECONSTRUCTION_FAILURE_CACHE_OPEN = 9,
    SCANNER_RECONSTRUCTION_FAILURE_BAD_ARGUMENT = 10,
    SCANNER_RECONSTRUCTION_FAILURE_INTERNAL = 11
} ScannerReconstruction_PagingFailure;

typedef struct ScannerReconstruction_PagingStats {
    uint64_t logical_chunks;
    uint64_t resident_chunks;
    uint64_t peak_resident_chunks;
    uint64_t resident_bytes;
    uint64_t peak_resident_bytes;
    // File extent, including reusable free slots; never exceeds backing budget.
    uint64_t backing_bytes;
    uint64_t reads;
    uint64_t writes;
    uint64_t evictions;
    uint32_t last_failure;
    uint32_t requires_replay;
    // Payload budgets include old committed + COW staging + loading + pinned
    // chunks, but not record/map metadata, output meshes, or OS file cache.
    uint64_t chunk_bytes;
    uint64_t resident_budget_bytes;
    uint64_t backing_budget_bytes;
    uint64_t backing_live_bytes;
} ScannerReconstruction_PagingStats;

// Serialized context access is required, including destruction and stats reads.
// Enables only on an empty RAM context. Creates a 0600 mkstemp file in the
// caller's directory, immediately unlinks it, and retains only its descriptor.
// No dataset access, mmap, persistent-cache recovery, or automatic fallback.
// Minimum resident budget: eight chunk payloads (currently 8 * 98304 bytes),
// for safe extraction pins. maxLogicalChunks must be 1..32768. Frame/work and
// other existing settings remain unchanged. Failed enable leaves RAM mode intact.
Tango3DR_Status ScannerReconstruction_enablePaging(
    Tango3DR_ReconstructionContext ctx, const char* cacheDirectory,
    uint64_t residentBudgetBytes, uint64_t backingBudgetBytes,
    uint32_t maxLogicalChunks);

// Does not allocate, page, perform I/O, or reset failure flags. Read/checksum
// failure makes requires_replay sticky until explicit clear or destruction.
// Call immediately after a failed update/extraction to inspect last_failure.
Tango3DR_Status ScannerReconstruction_getPagingStats(
    Tango3DR_ReconstructionContext ctx, ScannerReconstruction_PagingStats* stats);
#ifdef __cplusplus
}
#endif
#endif
