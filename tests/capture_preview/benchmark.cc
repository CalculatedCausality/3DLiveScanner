// Native preview workload: production core, shared synthetic capture generator.
// No app integration, camera acquisition, dataset access, or output simplifier.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main unusedCoreBenchmarkMain
#include "../reconstruction/core_benchmark.cc"
#undef main
#pragma GCC diagnostic pop
#include "../../reconstruction/paging.h"
#include <sys/resource.h>
#include <cstdint>
#include <string>

struct Bytes {
    std::vector<uint8_t> bytes;
    void data(const void* p, size_t n) {
        if (n) bytes.insert(bytes.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p)+n);
    }
    template<class T> void value(const T& v) { data(&v,sizeof(v)); }
    uint64_t hash() const {
        uint64_t h = UINT64_C(14695981039346656037);
        for (uint8_t b : bytes) { h ^= b; h *= UINT64_C(1099511628211); }
        return h;
    }
};

Tango3DR_CameraCalibration calibration() {
    Tango3DR_CameraCalibration cal{};
    cal.width = cal.height = 96; cal.fx = cal.fy = 88; cal.cx = cal.cy = 47.5;
    cal.calibration_type = TANGO_3DR_CALIBRATION_POLYNOMIAL_5_PARAMETERS;
    cal.distortion[0] = .015; cal.distortion[2] = .0005;
    return cal;
}

// Field-wise serialization avoids structure padding and pointer addresses.
Bytes recordedBytes(const std::vector<Frame>& frames, const Tango3DR_CameraCalibration& cal) {
    Bytes b;
    b.value(cal.width); b.value(cal.height); b.value(cal.fx); b.value(cal.fy);
    b.value(cal.cx); b.value(cal.cy); b.value(cal.calibration_type);
    b.data(cal.distortion,sizeof(cal.distortion));
    uint32_t count = uint32_t(frames.size()); b.value(count);
    for (size_t i = 0; i < frames.size(); ++i) {
        const Frame& f = frames[i];
        double timestamp = 1+i*.033; b.value(timestamp);
        uint32_t points = uint32_t(f.points.size()), stride = 296;
        b.value(points); b.data(f.points.data(),f.points.size()*sizeof(Point));
        b.value(cal.width); b.value(cal.height); b.value(stride);
        auto format = TANGO_3DR_HAL_PIXEL_FORMAT_RGB_888; b.value(format);
        b.data(f.rgb.data(),f.rgb.size()); // Includes RGB row padding.
        b.data(f.pose.translation,sizeof(f.pose.translation));
        b.data(f.pose.orientation,sizeof(f.pose.orientation));
        b.data(f.color_pose.translation,sizeof(f.color_pose.translation));
        b.data(f.color_pose.orientation,sizeof(f.color_pose.orientation));
    }
    return b;
}

struct Result {
    std::vector<double> updates, extracts, statuses, dirty_counts;
    uint64_t vertices = 0, faces = 0, segments = 0, nonempty = 0, mesh_bytes = 0;
    uint64_t extracted_vertices = 0, extracted_faces = 0;
    uint64_t resident = 0, peak_resident = 0, chunks = 0;
    double final_extract_ms = 0;
    long rss_kib = 0;
};

void validMesh(const Tango3DR_Mesh& mesh) {
    REQUIRE(mesh.num_vertices == 0 || (mesh.vertices && mesh.normals && mesh.colors));
    REQUIRE(mesh.num_faces == 0 || mesh.faces);
    for (uint32_t i = 0; i < mesh.num_vertices; ++i)
        for (int j = 0; j < 3; ++j)
            REQUIRE(std::isfinite(mesh.vertices[i][j]) && std::isfinite(mesh.normals[i][j]));
    for (uint32_t i = 0; i < mesh.num_faces; ++i) {
        const auto& f = mesh.faces[i];
        REQUIRE(f[0] < mesh.num_vertices && f[1] < mesh.num_vertices && f[2] < mesh.num_vertices);
        REQUIRE(f[0] != f[1] && f[1] != f[2] && f[0] != f[2]);
        glm::dvec3 a, b, c;
        for (int j = 0; j < 3; ++j) { a[j]=mesh.vertices[f[0]][j]; b[j]=mesh.vertices[f[1]][j]; c[j]=mesh.vertices[f[2]][j]; }
        auto normal = glm::cross(b-a,c-a);
        REQUIRE(glm::dot(normal,normal) > 0);
    }
}

Result run(std::vector<Frame>& frames, Tango3DR_CameraCalibration& cal,
           const Bytes& original, double resolution, bool live, const char* dump) {
    auto cfg = Tango3DR_Config_create(TANGO_3DR_CONFIG_RECONSTRUCTION); REQUIRE(cfg);
    REQUIRE(Tango3DR_Config_setDouble(cfg,"resolution",resolution) == 0);
    REQUIRE(Tango3DR_Config_setDouble(cfg,"min_depth",.1) == 0);
    REQUIRE(Tango3DR_Config_setDouble(cfg,"max_depth",5) == 0);
    REQUIRE(Tango3DR_Config_setBool(cfg,"use_space_clearing",true) == 0);
    REQUIRE(Tango3DR_Config_setBool(cfg,"generate_color",true) == 0);
    REQUIRE(Tango3DR_Config_setInt32(cfg,"min_num_vertices",1) == 0);
    auto context = Tango3DR_ReconstructionContext_create(cfg); REQUIRE(context);
    REQUIRE(Tango3DR_Config_destroy(cfg) == 0);
    REQUIRE(Tango3DR_ReconstructionContext_setColorCalibration(context,&cal) == 0);
    Result result;
    std::set<Key> all;
    for (size_t i = 0; i < frames.size(); ++i) {
        Frame& f = frames[i];
        Tango3DR_PointCloud cloud = {1+i*.033,uint32_t(f.points.size()),reinterpret_cast<Tango3DR_Vector4*>(f.points.data())};
        Tango3DR_ImageBuffer image = {96,96,296,cloud.timestamp,TANGO_3DR_HAL_PIXEL_FORMAT_RGB_888,f.rgb.data()};
        Tango3DR_GridIndexArray dirty{};
        auto begin = Clock::now();
        auto status = Tango3DR_updateFromPointCloud(context,&cloud,&f.pose,&image,&f.color_pose,&dirty);
        result.updates.push_back(elapsed(begin)); result.statuses.push_back(status);
        if (status != 0) {
            ScannerReconstruction_PagingStats stats{};
            ScannerReconstruction_getPagingStats(context,&stats);
            std::fprintf(stderr,"frame=%zu resolution=%g status=%d failure=%u\n",i,resolution,int(status),stats.last_failure);
        }
        REQUIRE(status == 0); // No dropped/rejected frames may improve timings.
        result.dirty_counts.push_back(dirty.num_indices);
        double ms = 0;
        for (uint32_t j = 0; j < dirty.num_indices; ++j) {
            Key key{{dirty.indices[j][0],dirty.indices[j][1],dirty.indices[j][2]}};
            all.insert(key);
            if (live) {
                Tango3DR_Mesh mesh{};
                begin = Clock::now();
                auto extracted = Tango3DR_extractMeshSegment(context,key.data(),&mesh);
                ms += elapsed(begin);
                REQUIRE(extracted == 0); validMesh(mesh);
                result.extracted_vertices += mesh.num_vertices; result.extracted_faces += mesh.num_faces;
                REQUIRE(Tango3DR_Mesh_destroy(&mesh) == 0);
            }
        }
        result.extracts.push_back(ms);
        REQUIRE(Tango3DR_GridIndexArray_destroy(&dirty) == 0);
        // Stronger than hash-only: compare every recorded byte after each update.
        REQUIRE(recordedBytes(frames,cal).bytes == original.bytes);
    }
    ScannerReconstruction_PagingStats stats{};
    REQUIRE(ScannerReconstruction_getPagingStats(context,&stats) == 0);
    REQUIRE(stats.last_failure == 0 && !stats.requires_replay);
    result.resident = stats.resident_bytes; result.peak_resident = stats.peak_resident_bytes;
    result.chunks = stats.logical_chunks;
    // Separate final full-volume snapshot: excluded from preview extraction totals.
    Output output(dump);
    uint32_t count = uint32_t(all.size()); output.value(count);
    for (const Key& key : all) {
        Tango3DR_Mesh mesh{};
        auto begin = Clock::now();
        auto status = Tango3DR_extractMeshSegment(context,key.data(),&mesh);
        result.final_extract_ms += elapsed(begin);
        REQUIRE(status == 0); validMesh(mesh);
        output.data(key.data(),sizeof(Tango3DR_GridIndex)); output.mesh(mesh);
        ++result.segments; result.nonempty += mesh.num_faces > 0;
        result.vertices += mesh.num_vertices; result.faces += mesh.num_faces;
        result.mesh_bytes += uint64_t(mesh.num_vertices)*(2*sizeof(Tango3DR_Vector3)+sizeof(Tango3DR_Color)) +
                             uint64_t(mesh.num_faces)*sizeof(Tango3DR_Face);
        REQUIRE(Tango3DR_Mesh_destroy(&mesh) == 0);
    }
    REQUIRE(result.vertices > 0 && result.faces > 0);
    REQUIRE(recordedBytes(frames,cal).bytes == original.bytes);
    rusage usage{}; REQUIRE(getrusage(RUSAGE_SELF,&usage) == 0); result.rss_kib = usage.ru_maxrss;
    REQUIRE(Tango3DR_ReconstructionContext_destroy(context) == 0);
    return result;
}

int main(int argc, char** argv) {
    REQUIRE(argc == 6);
    double resolution = std::atof(argv[1]);
    int count = std::atoi(argv[2]);
    std::string mode(argv[3]);
    REQUIRE((resolution == .01 || resolution == .02 || resolution == .05) && count >= 1 && count <= 12);
    REQUIRE(mode == "preview" || mode == "replay" || mode == "after-coarse");
    std::vector<Frame> frames;
    for (int i = 0; i < count; ++i) {
        Frame f = capture(i);
        // Miniature version of the existing room/box/sphere. No point decimation.
        for (auto& p : f.points) for (int j = 0; j < 3; ++j) p[j] *= .35f;
        for (int j = 0; j < 3; ++j) { f.pose.translation[j] *= .35; f.color_pose.translation[j] *= .35; }
        frames.push_back(f);
    }
    auto cal = calibration();
    const Bytes original = recordedBytes(frames,cal);
    { Output input(argv[5]); input.data(original.bytes.data(),original.bytes.size()); }
    if (mode == "after-coarse") {
        // The coarse context is destroyed before a fresh fine context is created.
        run(frames,cal,original,.05,true,"-");
    }
    Result r = run(frames,cal,original,resolution,mode == "preview",argv[4]);
    Bytes after = recordedBytes(frames,cal);
    REQUIRE(original.bytes == after.bytes && original.hash() == after.hash());
    std::printf("{\"resolution_m\":%.2f,\"frames\":%d,\"mode\":\"%s\",\"scene_scale\":0.35,\"points_per_frame\":9216,",resolution,count,mode.c_str());
    std::printf("\"input_bytes\":%llu,\"input_fnv1a_before\":\"%016llx\",\"input_fnv1a_after\":\"%016llx\",\"input_unchanged\":true,\"finite_valid_geometry\":true,",
        static_cast<unsigned long long>(original.bytes.size()),static_cast<unsigned long long>(original.hash()),static_cast<unsigned long long>(after.hash()));
    numbers("update_ms",r.updates); std::printf(","); numbers("extract_ms",r.extracts);
    std::printf(","); numbers("statuses",r.statuses); std::printf(","); numbers("dirty_segments",r.dirty_counts);
    std::printf(",\"final_extract_ms\":%.6f,\"peak_process_rss_kib\":%ld",r.final_extract_ms,r.rss_kib);
#define FIELD(name, value) std::printf(",\"" name "\":%llu",static_cast<unsigned long long>(value))
    FIELD("final_vertices",r.vertices); FIELD("final_faces",r.faces);
    FIELD("final_segments",r.segments); FIELD("final_nonempty_segments",r.nonempty);
    FIELD("final_mesh_payload_bytes",r.mesh_bytes); FIELD("preview_extracted_vertices",r.extracted_vertices);
    FIELD("preview_extracted_faces",r.extracted_faces); FIELD("resident_chunk_bytes",r.resident);
    FIELD("peak_resident_chunk_bytes",r.peak_resident); FIELD("logical_chunks",r.chunks);
#undef FIELD
    std::printf("}\n");
    return 0;
}
