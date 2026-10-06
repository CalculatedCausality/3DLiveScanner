// Public ABI: each resource limit rolls back an existing, nonempty volume.
// Repeated failures across replacement contexts exercise process-wide logging.
#include <tango_3d_reconstruction_api.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); std::abort(); } } while (0)
using Point = std::array<float,4>;
std::vector<uint8_t> snapshot(Tango3DR_ReconstructionContext c) {
    Tango3DR_GridIndex key = {0,0,1};
    Tango3DR_Mesh mesh = {};
    REQUIRE(Tango3DR_extractMeshSegment(c,key,&mesh) == 0 && mesh.num_faces);
    std::vector<uint8_t> result;
    auto append = [&](const void* p, size_t size) {
        auto bytes = static_cast<const uint8_t*>(p); result.insert(result.end(),bytes,bytes+size);
    };
    append(&mesh.timestamp,sizeof(mesh.timestamp));
    append(&mesh.num_vertices,sizeof(mesh.num_vertices)); append(&mesh.num_faces,sizeof(mesh.num_faces));
    append(mesh.vertices,size_t(mesh.num_vertices)*sizeof(Tango3DR_Vector3));
    append(mesh.normals,size_t(mesh.num_vertices)*sizeof(Tango3DR_Vector3));
    append(mesh.colors,size_t(mesh.num_vertices)*sizeof(Tango3DR_Color));
    append(mesh.faces,size_t(mesh.num_faces)*sizeof(Tango3DR_Face));
    REQUIRE(Tango3DR_Mesh_destroy(&mesh) == 0);
    return result;
}
int main() {
    std::vector<Point> seed;
    for (int y = 0; y < 15; ++y) for (int x = 0; x < 15; ++x)
        seed.push_back(Point{{.13f+x*.01f,.13f+y*.01f,1.f,1.f}});
    std::vector<Point> larger = seed;
    for (Point p : seed) { p[0] += 1.5f; larger.push_back(p); }
    const char* keys[] = {"max_update_work","max_chunks","max_update_chunks","points"};
    const int values[] = {40000,1,1,0};
    Tango3DR_Pose pose = {}; pose.orientation[3] = 1;
    for (int kind = 0; kind < 4; ++kind) for (int replacement = 0; replacement < 2; ++replacement) {
        auto config = Tango3DR_Config_create(TANGO_3DR_CONFIG_RECONSTRUCTION); REQUIRE(config);
        REQUIRE(Tango3DR_Config_setDouble(config,"resolution",.04) == 0);
        if (kind != 3) REQUIRE(Tango3DR_Config_setInt32(config,keys[kind],values[kind]) == 0);
        auto c = Tango3DR_ReconstructionContext_create(config); REQUIRE(c);
        REQUIRE(Tango3DR_Config_destroy(config) == 0);
        Tango3DR_PointCloud good = {1,uint32_t(seed.size()),reinterpret_cast<Tango3DR_Vector4*>(seed.data())};
        Tango3DR_GridIndexArray dirty = {};
        REQUIRE(Tango3DR_updateFromPointCloud(c,&good,&pose,nullptr,nullptr,&dirty) == 0);
        REQUIRE(Tango3DR_GridIndexArray_destroy(&dirty) == 0);
        auto before = snapshot(c);
        Tango3DR_PointCloud bad = {2,kind == 3 ? 1000001u : uint32_t(larger.size()),
                                  reinterpret_cast<Tango3DR_Vector4*>(larger.data())};
        for (int retry = 0; retry < 4; ++retry) {
            REQUIRE(Tango3DR_updateFromPointCloud(c,&bad,&pose,nullptr,nullptr,&dirty) == TANGO_3DR_INSUFFICIENT_SPACE);
            REQUIRE(!dirty.num_indices && !dirty.indices);
            REQUIRE(snapshot(c) == before); // includes timestamp, normals and colors
        }
        good.timestamp = 3;
        REQUIRE(Tango3DR_updateFromPointCloud(c,&good,&pose,nullptr,nullptr,&dirty) == 0);
        REQUIRE(Tango3DR_GridIndexArray_destroy(&dirty) == 0);
        REQUIRE(snapshot(c) != before);
        REQUIRE(Tango3DR_ReconstructionContext_destroy(c) == 0);
    }
    std::puts("All four named limits preserve committed meshes/timestamps and accept a later valid frame.");
}
