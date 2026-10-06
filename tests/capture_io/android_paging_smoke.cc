// ARM64 device smoke test for the built backend. Generated planes only; no app datasets.
#include <reconstruction/paging.h>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <set>
#include <stdexcept>
#include <vector>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static void hashBytes(uint64_t& hash, const void* pointer, size_t bytes) {
    const auto* data = static_cast<const unsigned char*>(pointer);
    for (size_t i = 0; i < bytes; ++i) { hash ^= data[i]; hash *= 1099511628211ULL; }
}
struct Result { uint64_t digest, faces; ScannerReconstruction_PagingStats stats; double milliseconds; };
static Result run(const char* directory, bool paged) {
    const auto start = std::chrono::steady_clock::now();
    Tango3DR_Config config = Tango3DR_Config_create(TANGO_3DR_CONFIG_RECONSTRUCTION);
    require(config != nullptr, "config");
    require(Tango3DR_Config_setDouble(config, "resolution", .02) == TANGO_3DR_SUCCESS, "resolution");
    require(Tango3DR_Config_setDouble(config, "min_depth", 0) == TANGO_3DR_SUCCESS, "min depth");
    require(Tango3DR_Config_setDouble(config, "max_depth", 2) == TANGO_3DR_SUCCESS, "max depth");
    require(Tango3DR_Config_setBool(config, "generate_color", false) == TANGO_3DR_SUCCESS, "color");
    require(Tango3DR_Config_setBool(config, "use_space_clearing", false) == TANGO_3DR_SUCCESS, "clearing");
    require(Tango3DR_Config_setInt32(config, "min_num_vertices", 0) == TANGO_3DR_SUCCESS, "component filter");
    require(Tango3DR_Config_setInt32(config, "max_chunks", 2048) == TANGO_3DR_SUCCESS, "reference cap");
    auto context = Tango3DR_ReconstructionContext_create(config);
    Tango3DR_Config_destroy(config);
    require(context != nullptr, "context");
    struct Owner {
        Tango3DR_ReconstructionContext value;
        ~Owner() { Tango3DR_ReconstructionContext_destroy(value); }
    } owner{context};
    const uint64_t resident = 64ULL * 98304ULL;
    if (paged) require(ScannerReconstruction_enablePaging(context, directory, resident, 256ULL<<20, 2048)
                       == TANGO_3DR_SUCCESS, "enable paging");
    std::vector<std::array<float, 4>> points;
    for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x)
        points.push_back({{(x-7.5f)*.03f, (y-7.5f)*.03f, 1.f, 1.f}});
    Tango3DR_PointCloud cloud{};
    cloud.num_points = points.size();
    cloud.points = reinterpret_cast<Tango3DR_Vector4*>(points.data());
    Tango3DR_Pose pose{}; pose.orientation[3] = 1;
    std::set<std::array<int32_t,3>> segments;
    for (int plane = 0; plane < 160; ++plane) {
        pose.translation[0] = plane;
        for (int pass = 0; pass < 3; ++pass) {
            cloud.timestamp = plane*3 + pass;
            Tango3DR_GridIndexArray dirty{};
            require(Tango3DR_updateFromPointCloud(context, &cloud, &pose, nullptr, nullptr, &dirty)
                    == TANGO_3DR_SUCCESS, "generated update");
            for (uint32_t i = 0; i < dirty.num_indices; ++i)
                segments.insert({{dirty.indices[i][0], dirty.indices[i][1], dirty.indices[i][2]}});
            Tango3DR_GridIndexArray_destroy(&dirty);
        }
    }
    Result result{}; result.digest = 1469598103934665603ULL;
    for (const auto& key : segments) {
        Tango3DR_GridIndex index = {key[0], key[1], key[2]};
        Tango3DR_Mesh mesh{};
        require(Tango3DR_extractMeshSegment(context, index, &mesh) == TANGO_3DR_SUCCESS, "reload/extract");
        hashBytes(result.digest, index, sizeof(index));
        hashBytes(result.digest, &mesh.num_vertices, sizeof(mesh.num_vertices));
        hashBytes(result.digest, &mesh.num_faces, sizeof(mesh.num_faces));
        if (mesh.num_vertices) {
            hashBytes(result.digest, mesh.vertices, mesh.num_vertices*sizeof(Tango3DR_Vector3));
            hashBytes(result.digest, mesh.normals, mesh.num_vertices*sizeof(Tango3DR_Vector3));
        }
        if (mesh.num_faces) hashBytes(result.digest, mesh.faces, mesh.num_faces*sizeof(Tango3DR_Face));
        result.faces += mesh.num_faces;
        Tango3DR_Mesh_destroy(&mesh);
    }
    require(ScannerReconstruction_getPagingStats(context, &result.stats) == TANGO_3DR_SUCCESS, "stats");
    require(result.stats.logical_chunks > 1024 && result.faces > 0, "insufficient coverage fixture");
    if (paged) require(result.stats.peak_resident_bytes <= resident && result.stats.evictions && result.stats.reads,
                       "residency/reload contract");
    result.milliseconds = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    return result;
}
int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: paging_smoke private-scratch-directory");
        const auto memory = run(argv[1], false);
        const auto paged = run(argv[1], true);
        require(memory.digest == paged.digest && memory.faces == paged.faces, "paged geometry differs");
        printf("PASS ARM64 generated paging: digest=%016llx faces=%llu logical=%llu peak_resident_bytes=%llu backing_bytes=%llu reads=%llu writes=%llu evictions=%llu ram_ms=%.2f paged_ms=%.2f\n",
               (unsigned long long)paged.digest, (unsigned long long)paged.faces,
               (unsigned long long)paged.stats.logical_chunks, (unsigned long long)paged.stats.peak_resident_bytes,
               (unsigned long long)paged.stats.backing_bytes, (unsigned long long)paged.stats.reads,
               (unsigned long long)paged.stats.writes, (unsigned long long)paged.stats.evictions,
               memory.milliseconds, paged.milliseconds);
    } catch (const std::exception& error) { fprintf(stderr, "FAIL: %s\n", error.what()); return 1; }
}
