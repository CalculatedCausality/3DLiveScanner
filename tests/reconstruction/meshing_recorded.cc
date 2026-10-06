// Private A/B extraction benchmark. Raw-volume cache is test-only: fusion runs
// once with the frozen source; both extractors read precisely the same voxels.
#ifndef MESHING_CORE_SOURCE
#define MESHING_CORE_SOURCE "../../reconstruction/core.cc"
#endif
#include MESHING_CORE_SOURCE
#define main unusedRecordedMain
#include "recorded_replay.cc"
#undef main
#include <iostream>
#include <ctime>

template<class T> void readValue(std::istream& input, T& value) {
    input.read(reinterpret_cast<char*>(&value),sizeof(value)); require(bool(input),"Truncated test volume");
}
template<class T> void writeValue(std::ostream& output, const T& value) {
    output.write(reinterpret_cast<const char*>(&value),sizeof(value)); require(bool(output),"Test volume write failed");
}
int main(int argc, char** argv) try {
    require(argc == 5,"fixture cache resolution output-mesh");
    double resolution = std::stod(argv[3]);
    _Tango3DR_Config config(TANGO_3DR_CONFIG_RECONSTRUCTION);
    config.values[recon::Resolution] = resolution;
    config.values[recon::MinDepth] = 0; config.values[recon::MaxDepth] = 15;
    config.values[recon::MinVertices] = 0; config.values[recon::GenerateColor] = 0;
    config.values[recon::Clearing] = 1;
    // Match the existing 2 cm recorded RAM comparison configuration. This is
    // an explicit host benchmark setting, not a change to any production cap.
    config.values[recon::MaxChunks] = resolution == .02 ? 4096 : 1024;
    _Tango3DR_ReconstructionContext context(config);
    std::vector<V> samples;
    uint32_t accepted = 0, rejected = 0;
    std::ifstream input(argv[2],std::ios::binary);
    if (input) {
        uint32_t magic = 0, chunks = 0, sampleCount = 0; double h = 0;
        readValue(input,magic); readValue(input,h); readValue(input,chunks);
        readValue(input,accepted); readValue(input,rejected); readValue(input,sampleCount);
        require(magic == 0x4d455331 && h == resolution && chunks <= config.values[recon::MaxChunks] && sampleCount <= 4096,"Wrong test cache");
        for (uint32_t i = 0; i < chunks; ++i) {
            recon::Index key; readValue(input,key);
            auto chunk = std::make_shared<recon::Chunk>(); readValue(input,*chunk);
            require(context.volume.emplace(key,chunk).second,"Duplicate chunk");
        }
        samples.resize(sampleCount); for (V& sample : samples) readValue(input,sample);
        require(input.peek() == std::char_traits<char>::eof(),"Trailing test cache bytes");
    } else {
        oc::Dataset dataset(argv[1]); int count = 0, width = 0, height = 0; double cx,cy,fx,fy;
        dataset.ReadState(count,width,height,cx,cy,fx,fy); require(count > 0 && count <= 1000,"Invalid fixture count");
        size_t perFrame = std::max(size_t(1),size_t(4096/count));
        for (int frame = 0; frame < count; ++frame) {
            std::vector<glm::mat4> matrices;
            require(dataset.ReadPose(frame,matrices) && matrices.size() == oc::MAX_CAMERA,"Pose read failed");
            require(oc::geometry::RigidPose(matrices[oc::COLOR_CAMERA]),"Nonrigid pose");
            Tango3DR_Pose pose = recordedPose(matrices[oc::COLOR_CAMERA]);
            Cloud cloud; cloud.value = dataset.ReadPointCloud(frame); Indices dirty;
            require(cloud.value.points && cloud.value.num_points,"Empty cloud");
            auto status = Tango3DR_updateFromPointCloud(&context,&cloud.value,&pose,nullptr,nullptr,&dirty.value);
            if (status == 0) ++accepted; else ++rejected;
            std::vector<uint32_t> eligible;
            for (uint32_t j = 0; j < cloud.value.num_points; ++j) if (cloud.value.points[j][2] <= 15) eligible.push_back(j);
            size_t n = std::min(perFrame,eligible.size());
            glm::dquat q = glm::normalize(glm::dquat(pose.orientation[3],pose.orientation[0],pose.orientation[1],pose.orientation[2]));
            V translation(pose.translation[0],pose.translation[1],pose.translation[2]);
            for (size_t j = 0; j < n; ++j)
                samples.push_back(q*vec(cloud.value.points[eligible[(2*j+1)*eligible.size()/(2*n)]])+translation);
        }
        std::ofstream output(argv[2],std::ios::binary); require(bool(output),"Cannot create test cache");
        writeValue(output,uint32_t(0x4d455331)); writeValue(output,resolution); writeValue(output,uint32_t(context.volume.size()));
        writeValue(output,accepted); writeValue(output,rejected); writeValue(output,uint32_t(samples.size()));
        for (const auto& entry : context.volume) { writeValue(output,entry.first); writeValue(output,*entry.second); }
        for (const V& sample : samples) writeValue(output,sample);
    }
    Meshes meshes; uint64_t vertices = 0, faces = 0;
    std::vector<double> times, cpuTimes;
    for (int repeat = 0; repeat < 5; ++repeat) {
        auto start = Clock::now();
        auto cpuStart = std::clock();
        for (const auto& entry : context.volume) {
            Key key{{entry.first.x,entry.first.y,entry.first.z}};
            Mesh mesh(new Tango3DR_Mesh{});
            require(Tango3DR_extractMeshSegment(&context,key.data(),mesh.get()) == 0,"Extraction failed");
            if (!repeat && mesh->num_vertices) {
                vertices += mesh->num_vertices; faces += mesh->num_faces;
                meshes.emplace(key,std::move(mesh));
            }
        }
        times.push_back(elapsed(start));
        cpuTimes.push_back(1000.0*(std::clock()-cpuStart)/CLOCKS_PER_SEC);
    }
    for (const auto& entry : meshes) finiteMesh(*entry.second); // outside the timed extraction loop
    size_t seamPairs = 0, seamDifferent = 0;
    std::vector<double> seamAngles;
    {
        std::map<PositionKey,V> normals;
        for (const auto& entry : meshes) {
            const auto& mesh = *entry.second;
            for (uint32_t i = 0; i < mesh.num_vertices; ++i) {
                V p = vec(mesh.vertices[i]), n = vec(mesh.normals[i]);
                if (!chunkPlane(p,p,entry.first,16*resolution)) continue;
                auto inserted = normals.emplace(positionKey(mesh.vertices[i]),n);
                if (inserted.second) continue;
                ++seamPairs; seamDifferent += inserted.first->second != n;
                double dot = glm::dot(glm::normalize(inserted.first->second),glm::normalize(n));
                seamAngles.push_back(std::acos(std::max(-1.,std::min(1.,dot)))*180/3.141592653589793);
            }
        }
    }
    std::sort(seamAngles.begin(),seamAngles.end());
    Writer writer(argv[4]); const char magic[] = "RecordedMeshV1"; writer.raw(magic,sizeof(magic));
    writer.value(uint32_t(meshes.size()));
    for (const auto& entry : meshes) writer.mesh(entry.first,*entry.second);
    writer.out.close();
    std::vector<recorded::Triangle> triangles;
    MeshStats stats = analyze(meshes,resolution,triangles);
    recorded::BVH bvh(triangles); std::vector<double> residuals;
    for (const V& sample : samples) residuals.push_back(bvh.distance(sample));
    std::cout << std::setprecision(12) << "{\"resolution\":" << resolution << ",\"accepted\":" << accepted
        << ",\"max_chunks\":" << config.values[recon::MaxChunks] << ",\"max_update_chunks\":256,\"max_update_work\":32000000"
        << ",\"rejected\":" << rejected << ",\"chunks\":" << context.volume.size() << ",\"vertices\":" << vertices
        << ",\"faces\":" << faces << ",\"attribute_bytes\":" << vertices*24+faces*12 << ",\"rss_kib\":" << rss()
        << ",\"extract_ms\":[";
    for (size_t j = 0; j < times.size(); ++j) { if (j) std::cout << ','; std::cout << times[j]; }
    std::cout << "],\"extract_cpu_ms\":"; array(std::cout,cpuTimes);
    std::cout << ",\"residuals\":"; residualStats(std::cout,residuals,resolution);
    std::cout << ",\"seam_normals\":{\"pairs\":" << seamPairs << ",\"nonidentical\":" << seamDifferent
        << ",\"max_deg\":" << (seamAngles.empty()?0:seamAngles.back())
        << ",\"p95_deg\":" << (seamAngles.empty()?0:seamAngles[size_t(.95*(seamAngles.size()-1))]) << "}";
    std::cout << ",\"topology\":{\"nonmanifold\":" << stats.nonmanifold
        << ",\"inconsistent_winding\":" << stats.inconsistent_winding
        << ",\"degenerate\":" << stats.degenerate << ",\"near_degenerate\":" << stats.near_degenerate
        << ",\"boundary\":" << stats.boundary << ",\"unpaired_planes\":" << stats.unpaired_planes
        << ",\"cross_chunk\":" << stats.cross_chunk << ",\"zero_normals\":" << stats.zero_normals
        << ",\"max_normal_error\":" << stats.max_normal_error << "}}\n";
    return 0;
} catch (const std::exception& error) { std::fprintf(stderr,"%s\n",error.what()); return 1; }
