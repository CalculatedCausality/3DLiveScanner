// Private normal-stencil, dirty-dependency, paging and angular-quality checks.
#ifndef MESHING_CORE_SOURCE
#define MESHING_CORE_SOURCE "../../reconstruction/core.cc"
#endif
#include MESHING_CORE_SOURCE
#include <functional>
#include <iostream>
#include <iomanip>
#include <ctime>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); std::abort(); } } while (0)
using namespace recon;
using Point = std::array<float,3>;
static uint64_t bytes(uint64_t h, const void* data, size_t count) {
    auto p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < count; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}
struct Snapshot {
    uint64_t geometry = 1469598103934665603ULL, normals = 1469598103934665603ULL;
    size_t vertices = 0, faces = 0, seams = 0, mismatches = 0;
    double angles = 0, maxAngle = 0;
    double extractionCpuMs = 0;
    std::map<Point,Point> shared;
    void add(const Tango3DR_Mesh& mesh, const std::function<glm::dvec3(glm::dvec3)>& truth) {
        geometry = bytes(geometry,&mesh.num_vertices,sizeof(mesh.num_vertices));
        geometry = bytes(geometry,&mesh.num_faces,sizeof(mesh.num_faces));
        geometry = bytes(geometry,mesh.vertices,mesh.num_vertices*12);
        geometry = bytes(geometry,mesh.faces,mesh.num_faces*12);
        normals = bytes(normals,mesh.normals,mesh.num_vertices*12);
        vertices += mesh.num_vertices; faces += mesh.num_faces;
        for (uint32_t i = 0; i < mesh.num_vertices; ++i) {
            Point p{{mesh.vertices[i][0],mesh.vertices[i][1],mesh.vertices[i][2]}};
            Point n{{mesh.normals[i][0],mesh.normals[i][1],mesh.normals[i][2]}};
            glm::dvec3 normal(n[0],n[1],n[2]);
            CHECK(std::isfinite(glm::length(normal)) && std::abs(glm::length(normal)-1) < 1e-6);
            auto found = shared.emplace(p,n);
            if (!found.second) { ++seams; mismatches += found.first->second != n; }
            double dot = glm::dot(glm::normalize(normal),glm::normalize(truth(glm::dvec3(p[0],p[1],p[2]))));
            double angle = std::acos(clamp(dot,-1,1))*180/3.141592653589793;
            angles += angle; maxAngle = std::max(maxAngle,angle);
        }
    }
};
Snapshot snapshot(_Tango3DR_ReconstructionContext& c, const std::function<glm::dvec3(glm::dvec3)>& truth) {
    Snapshot out;
    auto one = [&](const Index& key) {
        Tango3DR_GridIndex index = {key.x,key.y,key.z}; Tango3DR_Mesh mesh{};
        auto start = std::clock();
        CHECK(Tango3DR_extractMeshSegment(&c,index,&mesh) == 0);
        out.extractionCpuMs += 1000.*(std::clock()-start)/CLOCKS_PER_SEC;
        out.add(mesh,truth);
        start = std::clock(); CHECK(Tango3DR_Mesh_destroy(&mesh) == 0);
        out.extractionCpuMs += 1000.*(std::clock()-start)/CLOCKS_PER_SEC;
    };
    if (c.pager) for (const auto& entry : c.paged_volume) one(entry.first);
    else for (const auto& entry : c.volume) one(entry.first);
    return out;
}
void populate(_Tango3DR_ReconstructionContext& c, const std::function<double(glm::dvec3)>& sdf) {
    for (int z = -1; z <= 1; ++z) for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
        auto chunk = std::make_shared<Chunk>();
        for (int k = 0; k < 4096; ++k) {
            glm::dvec3 p(16*x+(k&15),16*y+((k>>4)&15),16*z+(k>>8));
            chunk->voxels[k].sdf = float(sdf(p*c.config.values[Resolution]));
            chunk->voxels[k].weight = 2;
        }
        c.volume.emplace(Index(x,y,z),chunk);
    }
}
void analytic(const char* label, double h, const std::function<double(glm::dvec3)>& sdf,
              const std::function<glm::dvec3(glm::dvec3)>& truth) {
    _Tango3DR_Config cfg(TANGO_3DR_CONFIG_RECONSTRUCTION); cfg.values[Resolution] = h;
    cfg.values[GenerateColor] = 0; cfg.values[MinVertices] = 0;
    _Tango3DR_ReconstructionContext c(cfg); populate(c,sdf);
    Snapshot result = snapshot(c,truth);
#ifndef MESHING_BEFORE_NORMALS
    CHECK(result.mismatches == 0);
#endif
    CHECK(result.vertices && result.seams);
    std::cout << std::setprecision(12) << "{\"scene\":\"" << label << "\",\"resolution\":" << h
        << ",\"geometry_hash\":" << result.geometry << ",\"vertices\":" << result.vertices << ",\"faces\":" << result.faces
        << ",\"angle_mean_deg\":" << result.angles/result.vertices << ",\"angle_max_deg\":" << result.maxAngle
        << ",\"shared_positions\":" << result.seams << ",\"nonidentical_shared_normals\":" << result.mismatches
        << ",\"extract_cpu_ms\":" << result.extractionCpuMs << "}\n";
}

void stencilAndDirty() {
#ifndef MESHING_BEFORE_NORMALS
    _Tango3DR_Config cfg(TANGO_3DR_CONFIG_RECONSTRUCTION); cfg.values[Resolution] = .02;
    _Tango3DR_ReconstructionContext c(cfg); populate(c,[](glm::dvec3 p){ return p.x+2*p.y+3*p.z; });
    FieldNormals field(c,Index()); glm::dvec3 n(99);
    CHECK(field.edge(Index(),Index(1,0,0),0,n));
    CHECK(glm::length(n-glm::dvec3(.02,.04,.06)) < 1e-8);
    auto node = [](int x,int y,int z) { return (x+1)+19*(y+1)+361*(z+1); };
    field.values[node(-1,0,0)].weight = .5f; // below the evidence threshold
    CHECK(field.edge(Index(),Index(1,0,0),0,n)); CHECK(std::abs(n.x-.02) < 1e-8);
    field.values[node(1,0,0)].weight = 0;
    CHECK(!field.edge(Index(),Index(1,0,0),0,n) && glm::length(n) == 0);
    FieldNormals exact(c,Index()); glm::dvec3 a,b;
    CHECK(exact.edge(Index(0,0,0),Index(1,0,0),0,a));
    CHECK(exact.edge(Index(0,0,0),Index(0,1,0),0,b)); CHECK(a == b);
    for (auto& s : exact.values) { s.sdf = 0; s.weight = 2; }
    CHECK(!exact.edge(Index(),Index(1,0,0),.5,n) && glm::length(n) == 0);
    for (int z = 0; z < 16; ++z) for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x) {
        Transaction tx(c); Index p(x,y,z);
        auto& cache = tx.cached(Index());
        Set<Index> expected;
        for (int k = -1; k <= 0; ++k) for (int j = -1; j <= 0; ++j) for (int i = -1; i <= 0; ++i) {
            tx.dirty.insert(Index(i,j,k)); expected.insert(Index(i,j,k));
        }
        tx.normalDirty(p,cache);
        for (int axis = 0; axis < 3; ++axis) for (int direction : {-1,1}) {
            int q[3] = {x,y,z}; q[axis] += direction;
            for (int k = 0; k < 2; ++k) for (int j = 0; j < 2; ++j) for (int i = 0; i < 2; ++i)
                expected.insert(chunkOf(Index(q[0]-i,q[1]-j,q[2]-k)));
        }
        CHECK(expected.size() == tx.dirty.size());
        for (const auto& key : expected) CHECK(tx.dirty.count(key) == 1);
    }
    // A corner stencil adds four owners. Each failed insertion must leave only
    // the successfully inserted prefix marked, so a retry cannot skip an owner.
    for (int fail = 0; fail <= 4; ++fail) {
        Transaction tx(c); auto& cache = tx.cached(Index());
        for (int z = -1; z <= 0; ++z) for (int y = -1; y <= 0; ++y) for (int x = -1; x <= 0; ++x)
            tx.dirty.insert(Index(x,y,z));
        bool threw = false;
        ReconstructionCore_testFailAfter(fail);
        try { tx.normalDirty(Index(15,0,0),cache); } catch (const std::bad_alloc&) { threw = true; }
        ReconstructionCore_testFailAfter(-1);
        CHECK(threw == (fail < 4));
        CHECK(cache.normal_dirty == (1u<<fail)-1 && tx.dirty.size() == size_t(8+fail));
        tx.normalDirty(Index(15,0,0),cache);
        CHECK(cache.normal_dirty == 15 && tx.dirty.size() == 12);
        // A direct-cache collision must reset the mask for the new world chunk,
        // then recover the old chunk's already published dirty set on revisit.
        auto& collision = tx.cached(Index(64,0,0));
        tx.normalDirty(Index(64*16+15,0,0),collision);
        CHECK(collision.normal_dirty == 15 && tx.dirty.size() == 16);
        auto& revisit = tx.cached(Index()); tx.normalDirty(Index(15,0,0),revisit);
        CHECK(revisit.normal_dirty == 15 && tx.dirty.size() == 16);
    }
    for (int shift : {0,-16}) {
        _Tango3DR_ReconstructionContext volume(cfg);
        populate(volume,[](glm::dvec3 p){ return p.z-2.65*.02; });
        if (shift) volume.volume.emplace(Index(-2,0,0),std::make_shared<Chunk>(*volume.volume.at(Index(-1,0,0))));
        Index target(shift/16,0,0); Tango3DR_GridIndex key = {target.x,0,0};
        Tango3DR_Mesh before{}, after{};
        CHECK(Tango3DR_extractMeshSegment(&volume,key,&before) == 0);
        Transaction tx(volume);
        Voxel* v = tx.writable(Index(shift-1,3,3),false); CHECK(v); v->sdf += .02f;
        CHECK(tx.dirty.count(target) == 1 && tx.dirty.size() == 9);
        volume.volume.swap(tx.next);
        CHECK(Tango3DR_extractMeshSegment(&volume,key,&after) == 0);
        CHECK(before.num_vertices == after.num_vertices && before.num_faces == after.num_faces && before.num_vertices);
        CHECK(!std::memcmp(before.vertices,after.vertices,before.num_vertices*12));
        CHECK(!std::memcmp(before.faces,after.faces,before.num_faces*12));
        CHECK(std::memcmp(before.normals,after.normals,before.num_vertices*12));
        Tango3DR_Mesh_destroy(&before); Tango3DR_Mesh_destroy(&after);
    }
    std::puts("normal stencils, exact-zero consistency, exhaustive selective dirty ownership, and opposite-chunk refresh: pass");
#endif
}
void paging(const char* directory) {
    _Tango3DR_Config cfg(TANGO_3DR_CONFIG_RECONSTRUCTION); cfg.values[Resolution] = .02;
    cfg.values[GenerateColor] = 0; cfg.values[MinVertices] = 0;
    _Tango3DR_ReconstructionContext ram(cfg), paged(cfg);
    const glm::dvec3 center(.137,.113,.129);
    auto truth = [center](glm::dvec3 p) { return p-center; };
    populate(ram,[center](glm::dvec3 p) { return glm::length(p-center)-.24; });
    CHECK(ScannerReconstruction_enablePaging(&paged,directory,8*sizeof(Chunk),64u*1024u*1024u,64) == 0);
    for (const auto& entry : ram.volume) paged.paged_volume.emplace(entry.first,paged.pager->create(entry.second.get()));
    ScannerReconstruction_PagingStats initial{}, final{};
    CHECK(ScannerReconstruction_getPagingStats(&paged,&initial) == 0);
    auto start = std::clock(); Snapshot a = snapshot(ram,truth); double ramMs = 1000.*(std::clock()-start)/CLOCKS_PER_SEC;
    start = std::clock(); Snapshot b = snapshot(paged,truth); double pagedMs = 1000.*(std::clock()-start)/CLOCKS_PER_SEC;
    CHECK(a.geometry == b.geometry && a.normals == b.normals);
    CHECK(ScannerReconstruction_getPagingStats(&paged,&final) == 0);
    CHECK(final.peak_resident_bytes <= 8*sizeof(Chunk));
    for (const auto& entry : paged.paged_volume) CHECK(entry.second->pins == 0);
    std::cout << "{\"paging_ram_cpu_ms\":" << ramMs << ",\"paging_cpu_ms\":" << pagedMs
        << ",\"ram_extract_cpu_ms\":" << a.extractionCpuMs << ",\"paged_extract_cpu_ms\":" << b.extractionCpuMs
        << ",\"reads\":" << final.reads-initial.reads << ",\"writes\":" << final.writes-initial.writes
        << ",\"peak_resident_bytes\":" << final.peak_resident_bytes << "}\n";
    std::vector<double> ramTimes, pagedTimes;
    for (int repeat = 0; repeat < 9; ++repeat) {
        Snapshot r = snapshot(ram,truth), p = snapshot(paged,truth);
        CHECK(r.geometry == a.geometry && r.normals == a.normals && p.geometry == a.geometry && p.normals == a.normals);
        ramTimes.push_back(r.extractionCpuMs); pagedTimes.push_back(p.extractionCpuMs);
    }
    std::sort(ramTimes.begin(),ramTimes.end()); std::sort(pagedTimes.begin(),pagedTimes.end());
    std::cout << "{\"warm_ram_extract_cpu_median_ms\":" << ramTimes[4]
        << ",\"warm_paged_extract_cpu_median_ms\":" << pagedTimes[4] << ",\"repeats\":9}\n";
#ifndef MESHING_BEFORE_NORMALS
    int failures = 0;
    for (long fail = 0; fail < 80; ++fail) {
        ReconstructionCore_testFailAfter(fail);
        Tango3DR_Mesh mesh{}; Tango3DR_GridIndex key = {0,0,0};
        auto status = Tango3DR_extractMeshSegment(&paged,key,&mesh);
        ReconstructionCore_testFailAfter(-1);
        for (const auto& entry : paged.paged_volume) CHECK(entry.second->pins == 0);
        if (status != 0) { ++failures; CHECK(!mesh.vertices && !mesh.faces && !mesh.num_vertices); }
        Tango3DR_Mesh_destroy(&mesh);
        CHECK(!paged.pager->broken());
        if (status == 0) break;
    }
    CHECK(failures > 8);
    paged.pager->setFault(2,0);
    Tango3DR_Mesh mesh{}; Tango3DR_GridIndex cold = {-1,-1,-1};
    CHECK(Tango3DR_extractMeshSegment(&paged,cold,&mesh) != 0);
    CHECK(!mesh.vertices && !mesh.faces && !mesh.num_vertices);
    for (const auto& entry : paged.paged_volume) CHECK(entry.second->pins == 0);
    std::printf("normal-halo pin cleanup: %d allocation failures and injected read failure passed\n",failures);
#endif
}
int main(int argc,char** argv) {
    CHECK(argc == 2);
    stencilAndDirty();
    for (double h : {.02,.04}) {
        glm::dvec3 n = glm::normalize(glm::dvec3(.31,-.23,1));
        analytic("plane",h,[n](glm::dvec3 p){return glm::dot(n,p)-.017;},[n](glm::dvec3){return n;});
        glm::dvec3 center(.137,.113,.129);
        analytic("sphere",h,[center](glm::dvec3 p){return glm::length(p-center)-.24;},[center](glm::dvec3 p){return p-center;});
    }
    paging(argv[1]);
}
