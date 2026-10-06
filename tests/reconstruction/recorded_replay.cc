// Geometry-only replay using the real Dataset readers and production pose body.
#include "data/dataset.h"
#include "arcore/geometry_validation.h"
#include "recorded_geometry.h"
#include "paging.h"
#include <glm/gtc/quaternion.hpp>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <sys/resource.h>

Tango3DR_Pose recordedPose(glm::mat4 matrix); // generated from actual Extract3DRPose body
using Clock=std::chrono::steady_clock;
using Key=std::array<int,3>;
using V=recorded::V;
static_assert(oc::MAX_CAMERA==3 && oc::COLOR_CAMERA==0,"Dataset pose schema changed");
static_assert(sizeof(float)==4 && std::numeric_limits<float>::is_iec559,"IEEE float32 required");
void require(bool valid,const char* reason) { if (!valid) throw std::runtime_error(reason); }
double elapsed(Clock::time_point start) { return std::chrono::duration<double,std::milli>(Clock::now()-start).count(); }
size_t rss() { rusage usage{}; getrusage(RUSAGE_SELF,&usage); return size_t(usage.ru_maxrss); }
struct Context {
    Tango3DR_ReconstructionContext value=nullptr;
    ~Context() { if(value) Tango3DR_ReconstructionContext_destroy(value); }
};
struct Config {
    Tango3DR_Config value=nullptr;
    ~Config() { if(value) Tango3DR_Config_destroy(value); }
};
struct Cloud {
    Tango3DR_PointCloud value{};
    ~Cloud() { Tango3DR_PointCloud_destroy(&value); }
};
struct Indices {
    Tango3DR_GridIndexArray value{};
    ~Indices() { Tango3DR_GridIndexArray_destroy(&value); }
};
struct MeshDelete { void operator()(Tango3DR_Mesh* m) const { Tango3DR_Mesh_destroy(m); delete m; } };
using Mesh=std::unique_ptr<Tango3DR_Mesh,MeshDelete>;
using Meshes=std::map<Key,Mesh>;
V vec(const float* p) { return V(p[0],p[1],p[2]); }
void finiteMesh(const Tango3DR_Mesh& m) {
    require(m.num_vertices<=m.max_num_vertices && m.num_faces<=m.max_num_faces,"Invalid mesh capacities");
    require(!m.num_vertices || m.vertices,"Missing vertices");
    require(!m.num_faces || m.faces,"Missing faces");
    require(std::isfinite(m.timestamp),"Nonfinite mesh timestamp");
    for(uint32_t i=0;i<m.num_vertices;++i) for(int j=0;j<3;++j) {
        require(std::isfinite(m.vertices[i][j]),"Nonfinite output vertex");
        if(m.normals) require(std::isfinite(m.normals[i][j]),"Nonfinite output normal");
    }
    for(uint32_t i=0;i<m.num_faces;++i) for(int j=0;j<3;++j)
        require(m.faces[i][j]<m.num_vertices,"Invalid face index");
}
struct Writer {
    std::ofstream out;
    explicit Writer(const char* path):out(path,std::ios::binary) { require(bool(out),"Cannot create ordered mesh"); }
    void raw(const void* p,size_t bytes) { if(bytes) out.write(static_cast<const char*>(p),bytes); require(bool(out),"Mesh output write failed"); }
    template<class T> void value(const T& v) { raw(&v,sizeof(v)); }
    void mesh(const Key& key,const Tango3DR_Mesh& m) {
        for(int k:key) { int32_t v=k; value(v); }
        value(m.timestamp); value(m.num_vertices); value(m.num_faces);
        value(m.max_num_vertices); value(m.max_num_faces);
        uint32_t flags=(m.normals?1:0)|(m.colors?2:0); value(flags);
        raw(m.vertices,size_t(m.num_vertices)*12);
        if(m.normals) raw(m.normals,size_t(m.num_vertices)*12);
        if(m.colors) raw(m.colors,size_t(m.num_vertices)*4);
        raw(m.faces,size_t(m.num_faces)*12);
    }
};
using PositionKey=std::array<uint32_t,3>;
PositionKey positionKey(const float* p) {
    PositionKey key;
    for(int j=0;j<3;++j) { float v=p[j]==0 ? 0.f : p[j]; std::memcpy(&key[j],&v,4); }
    return key;
}
bool chunkPlane(V a,V b,const Key& key,double width) {
    for(int j=0;j<3;++j) {
        double low=float(key[j]*width),high=float((key[j]+1)*width);
        if((a[j]==low && b[j]==low)||(a[j]==high && b[j]==high)) return true;
    }
    return false;
}
struct MeshStats {
    uint64_t vertices=0,faces=0,unique=0,degenerate=0,near_degenerate=0;
    uint64_t boundary=0,unpaired_planes=0,cross_chunk=0,nonmanifold=0,inconsistent_winding=0;
    uint64_t zero_normals=0,outside_segment=0;
    double area=0,max_normal_error=0;
    recorded::Box bounds;
};
MeshStats analyze(const Meshes& meshes,double resolution,std::vector<recorded::Triangle>& triangles) {
    MeshStats stats;
    std::map<PositionKey,uint32_t> welded;
    struct Edge { size_t count=0; int winding=0; Key owner{}; bool cross=false,on_plane=false; };
    std::map<std::pair<uint32_t,uint32_t>,Edge> edges;
    for(const auto& item:meshes) {
        const auto& key=item.first; const auto& m=*item.second;
        stats.vertices+=m.num_vertices; stats.faces+=m.num_faces;
        std::vector<uint32_t> ids(m.num_vertices);
        for(uint32_t i=0;i<m.num_vertices;++i) {
            V p=vec(m.vertices[i]); stats.bounds.add(p);
            auto inserted=welded.emplace(positionKey(m.vertices[i]),uint32_t(welded.size()));
            ids[i]=inserted.first->second;
            for(int j=0;j<3;++j) {
                double low=key[j]*16*resolution,high=(key[j]+1)*16*resolution;
                double tolerance=1e-5+1e-6*std::max(std::abs(low),std::abs(high));
                if(p[j]<low-tolerance || p[j]>high+tolerance) ++stats.outside_segment;
            }
            if(m.normals) {
                double length=glm::length(vec(m.normals[i]));
                if(length==0) ++stats.zero_normals;
                stats.max_normal_error=std::max(stats.max_normal_error,std::abs(length-1));
            }
        }
        for(uint32_t i=0;i<m.num_faces;++i) {
            const uint32_t* f=m.faces[i];
            recorded::Triangle t{vec(m.vertices[f[0]]),vec(m.vertices[f[1]]),vec(m.vertices[f[2]])};
            double area=.5*glm::length(glm::cross(t.b-t.a,t.c-t.a));
            require(std::isfinite(area),"Nonfinite face area");
            stats.area+=area;
            if(area==0 || f[0]==f[1] || f[1]==f[2] || f[0]==f[2]) ++stats.degenerate;
            if(area<=1e-12) ++stats.near_degenerate;
            triangles.push_back(t);
            for(int j=0;j<3;++j) {
                uint32_t a=ids[f[j]],b=ids[f[(j+1)%3]];
                bool forward=a<b;
                auto& e=edges[std::minmax(a,b)];
                if(!e.count) e.owner=key;
                else if(e.owner!=key) e.cross=true;
                ++e.count; e.winding+=forward?1:-1;
                e.on_plane|=chunkPlane(vec(m.vertices[f[j]]),vec(m.vertices[f[(j+1)%3]]),key,16*resolution);
            }
        }
    }
    stats.unique=welded.size();
    for(const auto& item:edges) {
        const auto& e=item.second;
        if(e.count==1) { ++stats.boundary; if(e.on_plane) ++stats.unpaired_planes; }
        if(e.count==2 && e.cross) ++stats.cross_chunk;
        if(e.count>2) ++stats.nonmanifold;
        if(e.count==2 && e.winding!=0) ++stats.inconsistent_winding;
    }
    return stats;
}
void seamSelfTest() {
    Meshes meshes;
    const float positions[2][3][3] = {
        {{.5f,.2f,1},{.64f,.2f,1},{.64f,.4f,1}},
        {{.64f,.2f,1},{.8f,.3f,1},{.64f,.4f,1}}
    };
    for(int i=0;i<2;++i) {
        Mesh m(new Tango3DR_Mesh{});
        require(Tango3DR_Mesh_init(3,1,false,false,false,false,0,0,0,m.get())==0,"Seam test allocation failed");
        m->num_vertices=3; m->num_faces=1;
        std::memcpy(m->vertices,positions[i],sizeof(positions[i]));
        for(int j=0;j<3;++j) m->faces[0][j]=j;
        meshes.emplace(Key{{i,0,1}},std::move(m));
    }
    std::vector<recorded::Triangle> triangles;
    auto stats=analyze(meshes,.04,triangles);
    require(stats.cross_chunk==1 && stats.boundary==4 && stats.unpaired_planes==0 &&
            stats.inconsistent_winding==0 && stats.nonmanifold==0 && stats.unique==4 && stats.degenerate==0,
            "Seam topology self-test failed");
    auto& right=*meshes.find(Key{{1,0,1}})->second;
    std::swap(right.faces[0][1],right.faces[0][2]);
    triangles.clear(); stats=analyze(meshes,.04,triangles);
    require(stats.inconsistent_winding==1,"Winding diagnostic self-test failed");
    right.faces[0][1]=right.faces[0][0];
    triangles.clear(); stats=analyze(meshes,.04,triangles);
    require(stats.degenerate==1,"Degenerate diagnostic self-test failed");
}
struct Sample { V position; bool accepted; };
void array(std::ostream& out,const std::vector<double>& values) {
    out<<'['; for(size_t i=0;i<values.size();++i) { if(i) out<<','; out<<values[i]; } out<<']';
}
void vector(std::ostream& out,V p) { out<<'['<<p.x<<','<<p.y<<','<<p.z<<']'; }
void residualStats(std::ostream& out,std::vector<double> values,double resolution) {
    out<<"{\"count\":"<<values.size();
    if(values.empty()) { out<<",\"mean_m\":null,\"rms_m\":null,\"median_m\":null,\"p95_m\":null,\"max_m\":null}"; return; }
    std::sort(values.begin(),values.end());
    double sum=0,squares=0; size_t one=0,two=0;
    for(double d:values) { sum+=d; squares+=d*d; if(d<=resolution) ++one; if(d<=2*resolution) ++two; }
    out<<",\"mean_m\":"<<sum/values.size()<<",\"rms_m\":"<<std::sqrt(squares/values.size())
       <<",\"median_m\":"<<values[values.size()/2]<<",\"p95_m\":"<<values[size_t(.95*(values.size()-1))]
       <<",\"max_m\":"<<values.back()<<",\"within_one_voxel\":"<<one<<",\"within_two_voxels\":"<<two<<'}';
}
int run(int argc,char** argv) {
    require(argc==4 || argc==10 || argc==11 || argc==14,"usage: recorded_replay FIXTURE OUTPUT RESOLUTION [RAM_CAP CACHE_OR_DASH RESIDENT BACKING LOGICAL ANALYSIS_FACES [FRAME_CAP [MAX_DEPTH MIN_VERTICES CLEARING]]]");
    int ram_cap=argc>=10?std::stoi(argv[4]):1024;
    bool paging=argc>=10 && std::strcmp(argv[5],"-")!=0;
    uint64_t resident=argc>=10?std::stoull(argv[6]):0, backing=argc>=10?std::stoull(argv[7]):0;
    uint32_t logical=argc>=10?uint32_t(std::stoul(argv[8])):1024;
    uint64_t analysis_faces=argc>=10?std::stoull(argv[9]):1000000;
    int frame_cap=argc>=11?std::stoi(argv[10]):256;
    double max_depth=argc==14?std::stod(argv[11]):15;
    int min_vertices=argc==14?std::stoi(argv[12]):0;
    int clearing=argc==14?std::stoi(argv[13]):1;
    require(std::isfinite(max_depth)&&max_depth>0&&max_depth<=100&&min_vertices>=0&&min_vertices<=1000000&&(clearing==0||clearing==1),"Invalid geometry policy");
    require(frame_cap>=1 && frame_cap<=1024,"Invalid per-frame chunk limit");
    require(ram_cap>=1 && ram_cap<=4096 && analysis_faces>=1 && analysis_faces<=4000000,"Invalid comparison limits");
    uint32_t endian=1; require(*reinterpret_cast<uint8_t*>(&endian)==1,"Little-endian host required");
    recorded::geometrySelfTest();
    seamSelfTest();
    auto total_start=Clock::now();
    double resolution=std::stod(argv[3]); require(std::isfinite(resolution)&&resolution>=.001&&resolution<=1,"Invalid resolution");
    oc::Dataset dataset(argv[1]);
    int count=0,width=0,height=0; double cx=0,cy=0,fx=0,fy=0;
    dataset.ReadState(count,width,height,cx,cy,fx,fy);
    require(count>0 && count<=1000 && width>0 && height>0 && width<=8192 && height<=8192,"Invalid dataset state");
    Config owner; owner.value=Tango3DR_Config_create(TANGO_3DR_CONFIG_RECONSTRUCTION);
    auto config=owner.value; require(config,"Config allocation failed");
    auto real=[&](const char* key,double value) { require(Tango3DR_Config_setDouble(config,key,value)==0,"Unsupported fixed config"); };
    auto integer=[&](const char* key,int value) { require(Tango3DR_Config_setInt32(config,key,value)==0,"Unsupported fixed config"); };
    auto boolean=[&](const char* key,bool value) { require(Tango3DR_Config_setBool(config,key,value)==0,"Unsupported fixed config"); };
    real("resolution",resolution); real("min_depth",0); real("max_depth",max_depth); real("min_confidence",0); real("min_voxel_weight",1);
    integer("max_voxel_weight",16383); integer("min_num_vertices",min_vertices); integer("update_method",0);
    integer("max_chunks",ram_cap); integer("max_update_chunks",frame_cap); integer("max_update_work",32000000);
    boolean("generate_color",false); boolean("use_space_clearing",clearing!=0); boolean("use_parallel_integration",false);
    boolean("use_clockwise_winding_order",false); boolean("rectify_color_image",true);
    Context context; context.value=Tango3DR_ReconstructionContext_create(config);
    Tango3DR_Config_destroy(config); owner.value=nullptr;
    require(context.value,"Context allocation failed");
    if(paging) require(ScannerReconstruction_enablePaging(context.value,argv[5],resident,backing,logical)==0,"Paging enable failed");
    Meshes meshes;
    std::vector<double> read_ms,update_ms,extract_ms,statuses,point_counts,dirty_counts;
    std::vector<Sample> samples;
    uint64_t total_points=0,eligible_points=0,vertices=0,faces=0;
    const size_t samples_per_frame=std::max(size_t(1),size_t(4096/count));
    for(int frame=0;frame<count;++frame) {
        auto start=Clock::now();
        std::vector<glm::mat4> matrices;
        require(dataset.ReadPose(frame,matrices) && matrices.size()==oc::MAX_CAMERA,"Pose read failed");
        for(const auto& m:matrices) require(oc::geometry::Finite(m),"Nonfinite pose matrix");
        require(oc::geometry::RigidPose(matrices[oc::COLOR_CAMERA]),"Nonrigid color camera pose");
        Tango3DR_Pose pose=recordedPose(matrices[oc::COLOR_CAMERA]);
        Cloud cloud; cloud.value=dataset.ReadPointCloud(frame);
        require(cloud.value.points && cloud.value.num_points && cloud.value.num_points<=1000000,"Point cloud read failed");
        require(cloud.value.timestamp==0,"Unexpected Dataset point-cloud timestamp semantics");
        std::vector<size_t> eligible;
        for(uint32_t i=0;i<cloud.value.num_points;++i) {
            const float* p=cloud.value.points[i];
            require(oc::geometry::Point(glm::vec4(p[0],p[1],p[2],p[3])) && p[2]>0,"Invalid XYZC point");
            if(p[2]<=max_depth) eligible.push_back(i);
        }
        read_ms.push_back(elapsed(start));
        total_points+=cloud.value.num_points; eligible_points+=eligible.size(); point_counts.push_back(cloud.value.num_points);
        Indices dirty;
        start=Clock::now();
        auto status=Tango3DR_updateFromPointCloud(context.value,&cloud.value,&pose,nullptr,nullptr,&dirty.value);
        update_ms.push_back(elapsed(start)); statuses.push_back(status); dirty_counts.push_back(dirty.value.num_indices);
        if(status!=TANGO_3DR_SUCCESS) require(!dirty.value.num_indices && !dirty.value.indices,"Failed update published indices");
        require(!dirty.value.num_indices || dirty.value.indices,"Missing dirty array");
        glm::dquat q=glm::normalize(glm::dquat(pose.orientation[3],pose.orientation[0],pose.orientation[1],pose.orientation[2]));
        V translation(pose.translation[0],pose.translation[1],pose.translation[2]);
        size_t selected=std::min(samples_per_frame,eligible.size());
        for(size_t j=0;j<selected;++j) {
            size_t i=eligible[size_t((2*j+1)*uint64_t(eligible.size())/(2*selected))];
            samples.push_back(Sample{q*vec(cloud.value.points[i])+translation,status==0});
        }
        start=Clock::now();
        for(uint32_t i=0;i<dirty.value.num_indices;++i) {
            const auto& d=dirty.value.indices[i]; Key key{{d[0],d[1],d[2]}};
            Mesh mesh(new Tango3DR_Mesh{});
            require(Tango3DR_extractMeshSegment(context.value,d,mesh.get())==0,"Mesh extraction failed");
            finiteMesh(*mesh);
            auto old=meshes.find(key);
            if(old!=meshes.end()) { vertices-=old->second->num_vertices; faces-=old->second->num_faces; meshes.erase(old); }
            vertices+=mesh->num_vertices; faces+=mesh->num_faces;
            require(vertices<=2*analysis_faces && faces<=analysis_faces,"Analysis geometry budget exceeded (not silently truncated)");
            if(mesh->num_vertices) meshes.emplace(key,std::move(mesh));
        }
        extract_ms.push_back(elapsed(start));
    }
    double replay_wall_ms=elapsed(total_start); size_t replay_rss=rss();
    ScannerReconstruction_PagingStats paging_stats{};
    require(ScannerReconstruction_getPagingStats(context.value,&paging_stats)==0,"Paging stats failed");
    Tango3DR_ReconstructionContext_destroy(context.value); context.value=nullptr;
    auto analyze_start=Clock::now();
    Writer writer(argv[2]); const char magic[]="RecordedMeshV1"; writer.raw(magic,sizeof(magic));
    uint32_t segment_count=uint32_t(meshes.size()); writer.value(segment_count);
    for(const auto& item:meshes) writer.mesh(item.first,*item.second);
    writer.out.close(); require(bool(writer.out),"Mesh output close failed");
    std::vector<recorded::Triangle> triangles; triangles.reserve(size_t(faces));
    MeshStats stats=analyze(meshes,resolution,triangles); meshes.clear();
    recorded::BVH bvh(triangles);
    std::vector<double> all_residuals,accepted_residuals,rejected_residuals;
    if(!triangles.empty()) for(const auto& sample:samples) {
        double distance=bvh.distance(sample.position); require(std::isfinite(distance),"Nonfinite residual");
        all_residuals.push_back(distance);
        (sample.accepted?accepted_residuals:rejected_residuals).push_back(distance);
    }
    double analysis_ms=elapsed(analyze_start);
    std::ostringstream out; out<<std::setprecision(17);
    out<<"{\"config\":{\"resolution\":"<<resolution<<",\"min_depth\":0,\"max_depth\":"<<max_depth<<",\"generate_color\":false,"
       "\"use_space_clearing\":"<<(clearing?"true":"false")<<",\"use_parallel_integration\":false,\"use_clockwise_winding_order\":false,"
       "\"rectify_color_image\":true,\"max_voxel_weight\":16383,\"min_num_vertices\":"<<min_vertices<<",\"update_method\":0,"
       "\"max_chunks\":"<<ram_cap<<",\"max_update_chunks\":"<<frame_cap<<",\"max_update_work\":32000000,\"min_confidence\":0,\"min_voxel_weight\":1},"
       "\"statuses\":"; array(out,statuses);
    out<<",\"points\":"; array(out,point_counts); out<<",\"dirty_segments\":"; array(out,dirty_counts);
    out<<",\"total_points\":"<<total_points<<",\"eligible_points\":"<<eligible_points
       <<",\"timing\":{\"read_validate_ms\":"; array(out,read_ms);
    out<<",\"update_ms\":"; array(out,update_ms); out<<",\"extract_validate_replace_ms\":"; array(out,extract_ms);
    out<<",\"replay_wall_ms\":"<<replay_wall_ms<<",\"analysis_ms\":"<<analysis_ms<<"},"
       "\"memory\":{\"replay_peak_rss_kib\":"<<replay_rss<<",\"overall_peak_rss_kib\":"<<rss()<<"},"
       "\"paging\":{\"enabled\":"<<(paging?"true":"false")<<",\"max_logical_chunks\":"<<(paging?logical:uint32_t(ram_cap))
       <<",\"logical_chunks\":"<<paging_stats.logical_chunks<<",\"resident_chunks\":"<<paging_stats.resident_chunks
       <<",\"peak_resident_chunks\":"<<paging_stats.peak_resident_chunks<<",\"resident_bytes\":"<<paging_stats.resident_bytes
       <<",\"peak_resident_bytes\":"<<paging_stats.peak_resident_bytes<<",\"backing_bytes\":"<<paging_stats.backing_bytes
       <<",\"backing_live_bytes\":"<<paging_stats.backing_live_bytes<<",\"reads\":"<<paging_stats.reads
       <<",\"writes\":"<<paging_stats.writes<<",\"evictions\":"<<paging_stats.evictions
       <<",\"last_failure\":"<<paging_stats.last_failure<<",\"requires_replay\":"<<paging_stats.requires_replay
       <<",\"resident_budget_bytes\":"<<paging_stats.resident_budget_bytes<<",\"backing_budget_bytes\":"<<paging_stats.backing_budget_bytes
       <<",\"chunk_bytes\":"<<paging_stats.chunk_bytes<<"},\"analysis_max_faces\":"<<analysis_faces<<","
       "\"mesh\":{\"segments\":"<<segment_count<<",\"vertices\":"<<stats.vertices<<",\"faces\":"<<stats.faces
       <<",\"exact_welded_vertices\":"<<stats.unique<<",\"finite\":true,\"has_surface\":"<<(stats.faces?"true":"false")
       <<",\"degenerate_faces\":"<<stats.degenerate
       <<",\"near_degenerate_area_le_1e_12_m2\":"<<stats.near_degenerate<<",\"area_m2\":"<<stats.area
       <<",\"zero_normals\":"<<stats.zero_normals<<",\"maximum_normal_length_error\":"<<stats.max_normal_error
       <<",\"out_of_segment_coordinates\":"<<stats.outside_segment<<",\"boundary_edges\":"<<stats.boundary
       <<",\"unpaired_edges_on_chunk_planes\":"<<stats.unpaired_planes<<",\"paired_cross_chunk_edges\":"<<stats.cross_chunk
       <<",\"nonmanifold_edges\":"<<stats.nonmanifold<<",\"inconsistent_two_face_winding_edges\":"<<stats.inconsistent_winding
       <<",\"bounds_min\":";
    if(stats.vertices) vector(out,stats.bounds.low); else out<<"null";
    out<<",\"bounds_max\":"; if(stats.vertices) vector(out,stats.bounds.high); else out<<"null";
    out<<"},\"residuals\":{\"selected_input_samples\":"<<samples.size()<<",\"all_eligible_sampled_inputs\":";
    residualStats(out,all_residuals,resolution); out<<",\"accepted_frame_inputs\":"; residualStats(out,accepted_residuals,resolution);
    out<<",\"rejected_frame_inputs\":"; residualStats(out,rejected_residuals,resolution); out<<"}}\n";
    std::printf("%s",out.str().c_str());
    return 0;
}
int main(int argc,char** argv) {
    try { return run(argc,argv); }
    catch(const std::exception& error) { std::fprintf(stderr,"Replay failed: %s\n",error.what()); return 1; }
}
