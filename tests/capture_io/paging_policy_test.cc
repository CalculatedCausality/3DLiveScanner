#include <tango/paging_policy.h>
#include <cassert>
#include <iostream>
#include <limits>

int main() {
    const uint64_t MiB = 1024u * 1024u, GiB = 1024 * MiB;
    auto coarse = oc::PlanPaging(.04, 16 * GiB, 32 * GiB);
    auto fine = oc::PlanPaging(.02, 16 * GiB, 32 * GiB);
    assert(coarse.enabled && fine.enabled);
    assert(coarse.resident_bytes == 96 * MiB && fine.resident_bytes == coarse.resident_bytes);
    assert(coarse.backing_bytes == 256 * MiB && coarse.logical_chunks == 1536);
    assert(fine.backing_bytes == GiB && fine.logical_chunks == 7680);
    assert(fine.update_chunks == 512);
    assert(oc::PlanPaging(.001, 16 * GiB, 32 * GiB).backing_bytes == GiB);
    assert(oc::PlanPaging(.02, 2 * GiB, 32 * GiB).resident_bytes == 32 * MiB);
    assert(oc::PlanPaging(.02, 2 * GiB, 32 * GiB).update_chunks == 256);
    assert(oc::PlanPaging(.02, 0, 32 * GiB).resident_bytes == 64 * MiB);
    auto lowStorage = oc::PlanPaging(.02, 16 * GiB, 2 * GiB);
    assert(lowStorage.enabled && lowStorage.backing_bytes == 512 * MiB);
    assert(!oc::PlanPaging(.02, 16 * GiB, GiB).enabled);
    assert(!oc::PlanPaging(.02, 16 * GiB, GiB + 511 * MiB).enabled);
    for (double bad : {0.0, -.02, .0001, 2.0, std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity()})
        assert(!oc::PlanPaging(bad, 16 * GiB, 32 * GiB).enabled);
    for (double resolution : {.001, .002, .005, .01, .02, .03, .04, .1, 1.0}) {
        for (uint64_t free : {0ULL, 1ULL << 30, 2ULL << 30, 8ULL << 30, 128ULL << 30}) {
            auto p = oc::PlanPaging(resolution, 4 * GiB, free);
            assert(p.resident_bytes >= 32 * MiB && p.resident_bytes <= 96 * MiB);
            if (p.enabled) {
                assert(p.backing_bytes <= GiB && p.backing_bytes <= (free-GiB)/2);
                assert(p.logical_chunks >= 1024 && p.logical_chunks <= 32768);
                assert(p.update_chunks >= 256 && p.update_chunks <= 512);
                assert(uint64_t(p.update_chunks)*128*1024 <= 64*MiB);
                assert(uint64_t(p.logical_chunks)*128*1024 + 64*MiB <= p.backing_bytes);
            }
        }
    }
    std::cout << "PASS: resolution-scaled backing, fixed resident cap, disk reserves and invalid inputs\n";
}
