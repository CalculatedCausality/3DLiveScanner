// Focused update-only A/B, reusing the existing generated room/box/sphere input.
// Complete dirty arrays and final meshes are serialized outside update timing.
#ifndef MESHING_CORE_SOURCE
#define MESHING_CORE_SOURCE "../../reconstruction/core.cc"
#endif
#include MESHING_CORE_SOURCE
// Renaming the fixture's unused main removes its implicit return-0 privilege.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main unusedCoreBenchmarkMain
#include "core_benchmark.cc"
#undef main
#pragma GCC diagnostic pop
#include <ctime>

int main(int argc, char** argv) {
    REQUIRE(argc == 5);
    Output output(argv[1]);
    double resolution = std::atof(argv[2]);
    int frames = std::atoi(argv[3]);
    REQUIRE((resolution == .02 || resolution == .04) && frames > 0 && frames <= 24);
    double writableCpu = 0;
    {
        _Tango3DR_Config config(TANGO_3DR_CONFIG_RECONSTRUCTION);
        _Tango3DR_ReconstructionContext c(config); recon::Transaction tx(c);
        std::array<recon::Index,4096> nodes;
        for (unsigned i = 0; i < nodes.size(); ++i) {
            nodes[i] = recon::Index(-16+int(i&15),-16+int((i>>4)&15),-16+int(i>>8));
            REQUIRE(tx.writable(nodes[i],true));
        }
        auto start = std::clock();
        for (unsigned i = 0; i < 4u*1024u*1024u; ++i) tx.writable(nodes[i&4095],false)->sdf += .000001f;
        writableCpu = 1000.*(std::clock()-start)/CLOCKS_PER_SEC;
        REQUIRE(tx.dirty.size() == 20 && tx.changed.size() == 1);
    }
    std::vector<Frame> inputs;
    for (int i = 0; i < frames; ++i) inputs.push_back(capture(i));
    auto cfg = Tango3DR_Config_create(TANGO_3DR_CONFIG_RECONSTRUCTION); REQUIRE(cfg);
    REQUIRE(Tango3DR_Config_setDouble(cfg,"resolution",resolution) == 0);
    REQUIRE(Tango3DR_Config_setDouble(cfg,"min_depth",.1) == 0);
    REQUIRE(Tango3DR_Config_setDouble(cfg,"max_depth",5) == 0);
    REQUIRE(Tango3DR_Config_setBool(cfg,"use_space_clearing",true) == 0);
    auto context = Tango3DR_ReconstructionContext_create(cfg); REQUIRE(context);
    Tango3DR_Config_destroy(cfg);
    bool paged = std::strcmp(argv[4],"-") != 0;
    if (paged) REQUIRE(ScannerReconstruction_enablePaging(context,argv[4],96u*1024u*1024u,512u*1024u*1024u,1024) == 0);
    Tango3DR_CameraCalibration cal{};
    cal.width = cal.height = 96; cal.fx = cal.fy = 88; cal.cx = cal.cy = 47.5;
    cal.calibration_type = TANGO_3DR_CALIBRATION_POLYNOMIAL_5_PARAMETERS;
    cal.distortion[0] = .015; cal.distortion[2] = .0005;
    REQUIRE(Tango3DR_ReconstructionContext_setColorCalibration(context,&cal) == 0);
    std::vector<double> wall, cpu;
    std::set<Key> all;
    for (int i = 0; i < frames; ++i) {
        Frame& f = inputs[i];
        Tango3DR_PointCloud cloud = {1+i*.033,uint32_t(f.points.size()),reinterpret_cast<Tango3DR_Vector4*>(f.points.data())};
        Tango3DR_ImageBuffer image = {96,96,296,cloud.timestamp,TANGO_3DR_HAL_PIXEL_FORMAT_RGB_888,f.rgb.data()};
        Tango3DR_GridIndexArray dirty{};
        auto start = Clock::now(); auto ticks = std::clock();
        auto status = Tango3DR_updateFromPointCloud(context,&cloud,&f.pose,&image,&f.color_pose,&dirty);
        cpu.push_back(1000.*(std::clock()-ticks)/CLOCKS_PER_SEC); wall.push_back(elapsed(start));
        REQUIRE(status == 0);
        output.value(status); output.value(dirty.num_indices);
        output.data(dirty.indices,dirty.num_indices*sizeof(Tango3DR_GridIndex));
        for (uint32_t j = 0; j < dirty.num_indices; ++j) all.insert(Key{{dirty.indices[j][0],dirty.indices[j][1],dirty.indices[j][2]}});
        Tango3DR_GridIndexArray_destroy(&dirty);
    }
    uint32_t count = uint32_t(all.size()); output.value(count);
    uint64_t vertices = 0, faces = 0;
    for (const auto& key : all) {
        Tango3DR_Mesh mesh{}; REQUIRE(Tango3DR_extractMeshSegment(context,key.data(),&mesh) == 0);
        output.data(key.data(),sizeof(Tango3DR_GridIndex)); output.mesh(mesh);
        vertices += mesh.num_vertices; faces += mesh.num_faces;
        Tango3DR_Mesh_destroy(&mesh);
    }
    ScannerReconstruction_PagingStats stats{};
    REQUIRE(ScannerReconstruction_getPagingStats(context,&stats) == 0);
    Tango3DR_ReconstructionContext_destroy(context);
    std::printf("{"); numbers("update_ms",wall); std::printf(","); numbers("update_cpu_ms",cpu);
    std::printf(",\"writable_cpu_ms\":%.6f,\"resolution\":%.2f,\"paged\":%s,\"frames\":%d,\"dirty_union\":%u,\"vertices\":%llu,\"faces\":%llu,\"bytes\":%llu}\n",
        writableCpu,resolution,paged?"true":"false",frames,count,static_cast<unsigned long long>(vertices),
        static_cast<unsigned long long>(faces),static_cast<unsigned long long>(output.bytes));
}
