// Same generated workload with paging enabled through the real public API.
#include <tango_3d_reconstruction_api.h>
#include <paging.h>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

static Tango3DR_ReconstructionContext generatedPagedContext(const Tango3DR_Config config) {
    if (const char* value = std::getenv("SCANNER_BENCH_CLEARING")) {
        if ((std::strcmp(value,"0") && std::strcmp(value,"1")) ||
            Tango3DR_Config_setBool(config,"use_space_clearing",!std::strcmp(value,"1")) != TANGO_3DR_SUCCESS)
            throw std::runtime_error("Invalid generated workload clearing policy");
    }
    // Explicit override for the live coverage-preview resolution. With no
    // override, retain the shared fixture's original per-case configuration.
    if (const char* value = std::getenv("SCANNER_BENCH_RESOLUTION")) {
        char* end = nullptr;
        const double resolution = std::strtod(value, &end);
        if (!end || *end || !(resolution >= .01 && resolution <= .05) ||
            Tango3DR_Config_setDouble(config, "resolution", resolution) != TANGO_3DR_SUCCESS)
            throw std::runtime_error("Invalid generated workload resolution");
    }
    auto context = Tango3DR_ReconstructionContext_create(config);
    if (!context) return nullptr;
    int32_t chunks = 1024;
    const char* directory = std::getenv("SCANNER_BENCH_CACHE");
    if (!directory || Tango3DR_Config_getInt32(config, "max_chunks", &chunks) != TANGO_3DR_SUCCESS
        || ScannerReconstruction_enablePaging(context, directory, 96ULL << 20, 1024ULL << 20,
                                              uint32_t(chunks)) != TANGO_3DR_SUCCESS) {
        Tango3DR_ReconstructionContext_destroy(context);
        throw std::runtime_error("Generated workload paging initialization failed");
    }
    return context;
}
#define Tango3DR_ReconstructionContext_create generatedPagedContext
#include "core_benchmark.cc"
