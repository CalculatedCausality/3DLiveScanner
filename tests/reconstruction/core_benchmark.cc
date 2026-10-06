// Deterministic 96x96 depth captures of a room, box and sphere from moving poses.
// Compile this SAME fixture against frozen/current core sources independently.
#include <tango_3d_reconstruction_api.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <set>
#include <vector>

#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); std::abort(); } } while (0)
using Clock = std::chrono::steady_clock;
using Point = std::array<float,4>;
using Key = std::array<int,3>;
struct Frame {
    std::vector<Point> points;
    std::vector<uint8_t> rgb;
    Tango3DR_Pose pose{}, color_pose{};
};
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double,std::milli>(Clock::now()-start).count();
}
struct Output {
    FILE* file = nullptr;
    uint64_t bytes = 0;
    explicit Output(const char* path) {
        if (std::strcmp(path,"-") != 0) { file = std::fopen(path,"wb"); REQUIRE(file); }
    }
    ~Output() { if (file) REQUIRE(std::fclose(file) == 0); }
    void data(const void* p, size_t n) {
        if (file && n) REQUIRE(std::fwrite(p,1,n,file) == n);
        bytes += n;
    }
    template<class T> void value(const T& t) { data(&t,sizeof(t)); }
    void mesh(const Tango3DR_Mesh& m) {
        value(m.timestamp); value(m.num_vertices); value(m.num_faces); value(m.num_textures);
        value(m.max_num_vertices); value(m.max_num_faces); value(m.max_num_textures);
        uint8_t flags = (m.normals ? 1 : 0) | (m.colors ? 2 : 0) |
            (m.texture_coords ? 4 : 0) | (m.texture_ids ? 8 : 0) | (m.textures ? 16 : 0);
        value(flags);
        data(m.vertices,size_t(m.num_vertices)*sizeof(Tango3DR_Vector3));
        if (m.normals) data(m.normals,size_t(m.num_vertices)*sizeof(Tango3DR_Vector3));
        if (m.colors) data(m.colors,size_t(m.num_vertices)*sizeof(Tango3DR_Color));
        data(m.faces,size_t(m.num_faces)*sizeof(Tango3DR_Face));
        REQUIRE(!m.num_textures && !m.texture_coords && !m.texture_ids);
        for (uint32_t i = 0; i < m.num_vertices; ++i)
            for (int j = 0; j < 3; ++j) REQUIRE(std::isfinite(m.vertices[i][j]) && std::isfinite(m.normals[i][j]));
        for (uint32_t i = 0; i < m.num_faces; ++i)
            for (int j = 0; j < 3; ++j) REQUIRE(m.faces[i][j] < m.num_vertices);
    }
};
double boxHit(glm::dvec3 o, glm::dvec3 d, glm::dvec3 low, glm::dvec3 high) {
    double near = 0, far = 100;
    for (int j = 0; j < 3; ++j) {
        if (std::abs(d[j]) < 1e-12) {
            if (o[j] < low[j] || o[j] > high[j]) return 100;
            continue;
        }
        double a = (low[j]-o[j])/d[j], b = (high[j]-o[j])/d[j];
        if (a > b) std::swap(a,b);
        near = std::max(near,a); far = std::min(far,b);
    }
    return near < far && near > 0 ? near : 100;
}
double depth(glm::dvec3 o, glm::dvec3 ray) {
    double t = (2.8-o.z)/ray.z; // back wall
    for (int axis = 0; axis < 2; ++axis) {
        double bound = axis == 0 ? 1.8 : 1.2;
        double hit = ((ray[axis] > 0 ? bound : -bound)-o[axis])/ray[axis];
        if (hit > 0) t = std::min(t,hit);
    }
    t = std::min(t,boxHit(o,ray,glm::dvec3(.3,-.35,1.25),glm::dvec3(.8,.5,1.9)));
    glm::dvec3 relative = o-glm::dvec3(-.48,.08,1.9);
    double b = glm::dot(relative,ray), a = glm::dot(ray,ray);
    double discriminant = b*b-a*(glm::dot(relative,relative)-.34*.34);
    if (discriminant > 0) {
        double hit = (-b-std::sqrt(discriminant))/a;
        if (hit > 0) t = std::min(t,hit);
    }
    REQUIRE(t > .2 && t < 5);
    return t;
}
Frame capture(int frame) {
    Frame f;
    glm::dvec3 translation(.65*std::sin(frame*.19),.09*std::sin(frame*.31),-.05+.1*std::cos(frame*.17));
    glm::dquat q = glm::angleAxis(.28*std::sin(frame*.23),glm::dvec3(0,1,0)) *
                   glm::angleAxis(.08*std::cos(frame*.27),glm::dvec3(1,0,0));
    for (int j = 0; j < 3; ++j) f.pose.translation[j] = translation[j];
    f.pose.orientation[0] = q.x; f.pose.orientation[1] = q.y;
    f.pose.orientation[2] = q.z; f.pose.orientation[3] = q.w;
    f.color_pose = f.pose; f.color_pose.translation[0] += .018;
    f.rgb.resize(96*296,231); // 288 visible RGB bytes + 8 bytes row padding
    for (int y = 0; y < 96; ++y) for (int x = 0; x < 96; ++x) {
        glm::dvec3 ray((x-47.5)/88.,(y-47.5)/88.,1);
        double z = depth(translation,q*ray) + .0015*std::sin(x*13+y*17+frame*.7);
        float confidence = ((x+3*y+frame)%43 == 0) ? 0.f : .3f+.7f*((x+y)%7)/6.f;
        f.points.push_back(Point{{float(ray.x*z),float(ray.y*z),float(z),confidence}});
        size_t index = size_t(y)*296+x*3;
        f.rgb[index] = uint8_t((2*x+frame*3)%256);
        f.rgb[index+1] = uint8_t((2*y+frame*5)%256);
        f.rgb[index+2] = uint8_t((x+y+frame)%256);
    }
    return f;
}
void numbers(const char* name, const std::vector<double>& v) {
    std::printf("\"%s\":[",name);
    for (size_t i = 0; i < v.size(); ++i) std::printf("%s%.6f",i?",":"",v[i]);
    std::printf("]");
}
int main(int argc, char** argv) {
    REQUIRE(argc == 4);
    int frames = std::atoi(argv[2]), which = std::atoi(argv[3]);
    REQUIRE(frames > 0 && frames <= 128 && which >= 0 && which <= 6);
    Output output(argv[1]);
    std::vector<Frame> inputs;
    for (int i = 0; i < frames; ++i) inputs.push_back(capture(i));
    auto config = Tango3DR_Config_create(TANGO_3DR_CONFIG_RECONSTRUCTION); REQUIRE(config);
    REQUIRE(Tango3DR_Config_setDouble(config,"resolution",which == 2 ? .02 : which == 1 ? .03 : .04) == 0);
    REQUIRE(Tango3DR_Config_setDouble(config,"min_depth",.1) == 0);
    REQUIRE(Tango3DR_Config_setDouble(config,"max_depth",5) == 0);
    REQUIRE(Tango3DR_Config_setBool(config,"use_space_clearing",which != 1) == 0);
    REQUIRE(Tango3DR_Config_setBool(config,"generate_color",which != 1) == 0);
    REQUIRE(Tango3DR_Config_setBool(config,"use_clockwise_winding_order",which == 1) == 0);
    REQUIRE(Tango3DR_Config_setInt32(config,"min_num_vertices",which == 1 ? 1000000 : 1) == 0);
    if (which == 2) REQUIRE(Tango3DR_Config_setInt32(config,"max_chunks",160) == 0);
    if (which == 3) REQUIRE(Tango3DR_Config_setInt32(config,"max_update_work",200000) == 0);
    if (which == 4) REQUIRE(Tango3DR_Config_setInt32(config,"max_update_chunks",4) == 0);
    auto context = Tango3DR_ReconstructionContext_create(config); REQUIRE(context);
    REQUIRE(Tango3DR_Config_destroy(config) == 0);
    Tango3DR_CameraCalibration cal = {};
    cal.width = cal.height = 96; cal.fx = cal.fy = 88; cal.cx = cal.cy = 47.5;
    cal.calibration_type = TANGO_3DR_CALIBRATION_POLYNOMIAL_5_PARAMETERS;
    cal.distortion[0] = .015; cal.distortion[2] = .0005;
    REQUIRE(Tango3DR_ReconstructionContext_setColorCalibration(context,&cal) == 0);
    std::set<Key> all;
    auto indices = [&](const Tango3DR_GridIndexArray& a) {
        output.value(a.num_indices);
        output.data(a.indices,size_t(a.num_indices)*sizeof(Tango3DR_GridIndex));
        for (uint32_t i = 0; i < a.num_indices; ++i) all.insert(Key{{a.indices[i][0],a.indices[i][1],a.indices[i][2]}});
    };
    std::vector<Point> seed;
    for (int y = 0; y < 15; ++y) for (int x = 0; x < 15; ++x)
        seed.push_back(Point{{.13f+x*.01f,.13f+y*.01f,1.2f,1}});
    Tango3DR_PointCloud seed_cloud = {0,uint32_t(seed.size()),reinterpret_cast<Tango3DR_Vector4*>(seed.data())};
    Tango3DR_Pose identity = {}; identity.orientation[3] = 1;
    Tango3DR_GridIndexArray seed_indices = {};
    REQUIRE(Tango3DR_updateFromPointCloud(context,&seed_cloud,&identity,nullptr,nullptr,&seed_indices) == 0);
    indices(seed_indices); Tango3DR_GridIndexArray_destroy(&seed_indices);
    std::vector<double> updates, extracts, statuses;
    uint64_t segments = 0, faces = 0;
    auto extract = [&](const Key& key, double& ms) {
        Tango3DR_Mesh mesh = {};
        auto begin = Clock::now();
        REQUIRE(Tango3DR_extractMeshSegment(context,key.data(),&mesh) == 0);
        ms += elapsed(begin);
        output.data(key.data(),sizeof(Tango3DR_GridIndex)); output.mesh(mesh);
        ++segments; faces += mesh.num_faces;
        REQUIRE(Tango3DR_Mesh_destroy(&mesh) == 0);
    };
    for (int frame = 0; frame < frames; ++frame) {
        Frame& f = inputs[frame];
        if (which == 6) f.points.back()[0] = std::numeric_limits<float>::quiet_NaN();
        Tango3DR_PointCloud cloud = {1+frame*.033,uint32_t(f.points.size()),reinterpret_cast<Tango3DR_Vector4*>(f.points.data())};
        if (which == 5) cloud.num_points = 1000001; // rejected before buffer access
        Tango3DR_ImageBuffer image = {96,96,296,cloud.timestamp,TANGO_3DR_HAL_PIXEL_FORMAT_RGB_888,f.rgb.data()};
        Tango3DR_GridIndexArray dirty = {};
        auto start = Clock::now();
        auto status = Tango3DR_updateFromPointCloud(context,&cloud,&f.pose,&image,&f.color_pose,&dirty);
        updates.push_back(elapsed(start)); statuses.push_back(status);
        output.value(status); indices(dirty);
        REQUIRE(status == 0 || status == TANGO_3DR_INSUFFICIENT_SPACE || status == TANGO_3DR_INVALID);
        if (which <= 1) REQUIRE(status == 0);
        double ms = 0;
        if (status == 0) {
            for (uint32_t i = 0; i < dirty.num_indices; ++i)
                extract(Key{{dirty.indices[i][0],dirty.indices[i][1],dirty.indices[i][2]}},ms);
        } else {
            REQUIRE(!dirty.num_indices && !dirty.indices);
            // Capture the previously committed mesh after every rejection,
            // then demonstrate that a subsequent small valid update can commit.
            for (const Key& key : all) extract(key,ms);
            seed_cloud.timestamp = 100+frame;
            Tango3DR_GridIndexArray recovered = {};
            auto accepted = Tango3DR_updateFromPointCloud(context,&seed_cloud,&identity,nullptr,nullptr,&recovered);
            REQUIRE(accepted == 0); output.value(accepted); indices(recovered);
            REQUIRE(Tango3DR_GridIndexArray_destroy(&recovered) == 0);
        }
        extracts.push_back(ms);
        REQUIRE(Tango3DR_GridIndexArray_destroy(&dirty) == 0);
    }
    // Compare all segments accumulated over the sequence, not just the last view.
    uint32_t count = uint32_t(all.size()); output.value(count);
    double ignored = 0;
    for (const Key& key : all) extract(key,ignored);
    REQUIRE(faces > 0);
    REQUIRE(Tango3DR_ReconstructionContext_destroy(context) == 0);
    std::printf("{"); numbers("update_ms",updates); std::printf(",");
    numbers("extract_ms",extracts); std::printf(","); numbers("statuses",statuses);
    std::printf(",\"points_per_frame\":9216,\"segments\":%llu,\"faces\":%llu,\"bytes\":%llu}\n",
        static_cast<unsigned long long>(segments),static_cast<unsigned long long>(faces),
        static_cast<unsigned long long>(output.bytes));
}
