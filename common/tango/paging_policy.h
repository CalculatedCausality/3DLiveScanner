#ifndef SCANNER_PAGING_POLICY_H
#define SCANNER_PAGING_POLICY_H

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace oc {
    struct PagingPlan {
        uint64_t resident_bytes = 0;
        uint64_t backing_bytes = 0;
        uint32_t logical_chunks = 0;
        uint32_t update_chunks = 256;
        bool enabled = false;
    };

    // Caller policy only, not a claim about whole-process/GPU memory. The engine
    // enforces its exact chunk payload and backing-record accounting separately.
    inline PagingPlan PlanPaging(double resolution, uint64_t physicalMemory,
                                 uint64_t availableStorage) {
        const uint64_t MiB = 1024u * 1024u;
        PagingPlan plan;
        if (!std::isfinite(resolution) || resolution < .001 || resolution > 1) return plan;
        const uint64_t memoryShare = physicalMemory ? physicalMemory / 64 : 64 * MiB;
        plan.resident_bytes = std::max(32 * MiB, std::min(96 * MiB, memoryShare));

        // 4 cm is the baseline grid. Finer grids request more DISK coverage, not
        // an unbounded resident set. Clamp before converting to an integer.
        const double scale = std::pow(.04 / resolution, 3);
        const uint64_t desiredChunks = static_cast<uint64_t>(std::ceil(
                std::max(1024.0, std::min(32768.0, 1024.0 * scale))));
        // Conservative policy allowance: current voxel payload is 96 KiB/chunk;
        // leave metadata/transaction headroom rather than use every byte as data.
        const uint64_t perChunkAllowance = 128u * 1024u;
        const uint64_t transactionReserve = 64 * MiB;
        const uint64_t storageReserve = 1024 * MiB; // matches the capture UI guard
        if (availableStorage <= storageReserve) return plan;
        uint64_t desiredBytes = desiredChunks * perChunkAllowance + transactionReserve;
        desiredBytes = std::max(256 * MiB, std::min(1024 * MiB, desiredBytes));
        plan.backing_bytes = std::min(desiredBytes, (availableStorage - storageReserve) / 2);
        if (plan.backing_bytes < 256 * MiB) {
            plan.backing_bytes = 0;
            return plan;
        }
        plan.logical_chunks = static_cast<uint32_t>(std::min<uint64_t>(32768,
                (plan.backing_bytes - transactionReserve) / perChunkAllowance));
        // Paging bounds COW payloads independently. On larger resident budgets,
        // admit a wider frame while retaining the work cap and 64 MiB disk reserve.
        const uint64_t payloadBytes = 4096u * 6u * sizeof(float); // engine's checked layout
        plan.update_chunks = static_cast<uint32_t>(std::max<uint64_t>(256,
                std::min<uint64_t>(512, plan.resident_bytes / (2 * payloadBytes))));
        plan.enabled = true;
        return plan;
    }
}
#endif
