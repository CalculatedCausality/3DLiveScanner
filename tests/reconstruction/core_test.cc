// Public-ABI analytic/recovery tests; no access to private volume structures.
#include <tango_3d_reconstruction_api.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <vector>

extern "C" void ReconstructionCore_testFailAfter(long);
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); std::abort(); } } while (0)
typedef std::array<int,3> Key;
typedef std::array<float,4> Point;
typedef std::vector<Point> Cloud;
Tango3DR_Pose identity() { Tango3DR_Pose p = {}; p.orientation[3] = 1; return p; }
Tango3DR_Pose pose(glm::dvec3 t, glm::dquat q) {
    Tango3DR_Pose p = {{t.x,t.y,t.z},{q.x,q.y,q.z,q.w}}; return p;
}
Tango3DR_PointCloud view(Cloud& p, double timestamp = 1) {
    Tango3DR_PointCloud c = {timestamp,uint32_t(p.size()),reinterpret_cast<Tango3DR_Vector4*>(p.data())}; return c;
}
Cloud plane(float z, double half = .36, double step = .015, float confidence = 1) {
    Cloud p;
    int n = int(std::round(half/step));
    for (int y = -n; y <= n; ++y) for (int x = -n; x <= n; ++x)
        p.push_back(Point{{float(x*step),float(y*step),z,confidence}});
    return p;
}
struct Config {
    Tango3DR_Config p;
    explicit Config(Tango3DR_ConfigType t = TANGO_3DR_CONFIG_RECONSTRUCTION) : p(Tango3DR_Config_create(t)) { CHECK(p); }
    ~Config() { CHECK(Tango3DR_Config_destroy(p) == TANGO_3DR_SUCCESS); }
    void real(const char* k, double v) { CHECK(Tango3DR_Config_setDouble(p,k,v) == TANGO_3DR_SUCCESS); }
    void integer(const char* k, int v) { CHECK(Tango3DR_Config_setInt32(p,k,v) == TANGO_3DR_SUCCESS); }
    void boolean(const char* k, bool v) { CHECK(Tango3DR_Config_setBool(p,k,v) == TANGO_3DR_SUCCESS); }
};
struct Context {
    Tango3DR_ReconstructionContext p;
    explicit Context(const Config& c) : p(Tango3DR_ReconstructionContext_create(c.p)) { CHECK(p); }
    ~Context() { CHECK(Tango3DR_ReconstructionContext_destroy(p) == TANGO_3DR_SUCCESS); }
};
struct Mesh {
    Tango3DR_Mesh p;
    Mesh() : p() {}
    ~Mesh() { CHECK(Tango3DR_Mesh_destroy(&p) == TANGO_3DR_SUCCESS); }
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
};
std::set<Key> update(Context& c, Cloud& points, const Tango3DR_Pose& p = identity(),
                     const Tango3DR_ImageBuffer* image = nullptr, const Tango3DR_Pose* color_pose = nullptr,
                     double timestamp = 1) {
    Tango3DR_PointCloud cloud = view(points,timestamp);
    Tango3DR_GridIndexArray dirty = {};
    CHECK(Tango3DR_updateFromPointCloud(c.p,&cloud,&p,image,color_pose,&dirty) == TANGO_3DR_SUCCESS);
    CHECK(!dirty.num_indices || dirty.indices);
    std::set<Key> result;
    for (uint32_t i = 0; i < dirty.num_indices; ++i) result.insert(Key{{dirty.indices[i][0],dirty.indices[i][1],dirty.indices[i][2]}});
    CHECK(result.size() == dirty.num_indices);
    CHECK(Tango3DR_GridIndexArray_destroy(&dirty) == TANGO_3DR_SUCCESS);
    CHECK(Tango3DR_GridIndexArray_destroy(&dirty) == TANGO_3DR_SUCCESS);
    return result;
}
glm::dvec3 v3(const float* p) { return glm::dvec3(p[0],p[1],p[2]); }
struct Snapshot {
    std::vector<glm::dvec3> positions, normals, colors;
    std::vector<std::array<uint32_t,3> > faces;
    double area = 0;
    uint64_t hash = 1469598103934665603ULL;
    void bytes(const void* data, size_t n) {
        const unsigned char* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < n; ++i) { hash ^= p[i]; hash *= 1099511628211ULL; }
    }
    void append(const Tango3DR_Mesh& m) {
        CHECK(m.num_vertices <= m.max_num_vertices && m.num_faces <= m.max_num_faces);
        CHECK(!m.num_vertices || (m.vertices && m.normals));
        bytes(&m.num_vertices,sizeof(m.num_vertices)); bytes(&m.num_faces,sizeof(m.num_faces));
        bytes(m.vertices,m.num_vertices*sizeof(Tango3DR_Vector3));
        bytes(m.normals,m.num_vertices*sizeof(Tango3DR_Vector3));
        if (m.colors) bytes(m.colors,m.num_vertices*sizeof(Tango3DR_Color));
        bytes(m.faces,m.num_faces*sizeof(Tango3DR_Face));
        uint32_t start = positions.size();
        for (uint32_t i = 0; i < m.num_vertices; ++i) {
            glm::dvec3 p = v3(m.vertices[i]), n = v3(m.normals[i]);
            for (int j = 0; j < 3; ++j) CHECK(std::isfinite(p[j]) && std::isfinite(n[j]));
            CHECK(std::abs(glm::length(n)-1) < 1e-4);
            positions.push_back(p); normals.push_back(n);
            colors.push_back(m.colors ? glm::dvec3(m.colors[i][0],m.colors[i][1],m.colors[i][2]) : glm::dvec3(-1));
            if (m.colors) CHECK(m.colors[i][3] == 255);
        }
        for (uint32_t i = 0; i < m.num_faces; ++i) {
            const uint32_t* f = m.faces[i];
            CHECK(f[0] < m.num_vertices && f[1] < m.num_vertices && f[2] < m.num_vertices);
            glm::dvec3 n = glm::cross(v3(m.vertices[f[1]])-v3(m.vertices[f[0]]),v3(m.vertices[f[2]])-v3(m.vertices[f[0]]));
            CHECK(glm::length(n) > 1e-12);
            area += glm::length(n)*.5;
            faces.push_back(std::array<uint32_t,3>{{start+f[0],start+f[1],start+f[2]}});
        }
    }
};
Snapshot snapshot(Context& c, const std::set<Key>& keys) {
    Snapshot s;
    for (const Key& k : keys) {
        Mesh m;
        CHECK(Tango3DR_extractMeshSegment(c.p,k.data(),&m.p) == TANGO_3DR_SUCCESS);
        s.append(m.p);
    }
    return s;
}
void defaults(Config& c, double h = .04) { c.real("resolution",h); c.real("min_depth",0); c.real("max_depth",5); }
double rmsPlane(const Snapshot& s, glm::dvec3 n, double d, double* maximum = nullptr) {
    CHECK(!s.positions.empty()); double sum = 0, mx = 0;
    for (glm::dvec3 p : s.positions) { double e = std::abs(glm::dot(n,p)-d); sum += e*e; mx = std::max(mx,e); }
    if (maximum) *maximum = mx;
    return std::sqrt(sum/s.positions.size());
}

void configAndOwnership() {
    Config c;
    double d = -1; bool b = false; int32_t i = -1;
    CHECK(Tango3DR_Config_getDouble(c.p,"resolution",&d) == 0 && d == .03);
    CHECK(Tango3DR_Config_getBool(c.p,"generate_color",&b) == 0 && b);
    CHECK(Tango3DR_Config_getInt32(c.p,"max_voxel_weight",&i) == 0 && i == 16383);
    CHECK(Tango3DR_Config_getDouble(c.p,"unknown",&d) == TANGO_3DR_INVALID && d == .03);
    CHECK(Tango3DR_Config_getDouble(c.p,"resolution",nullptr) == TANGO_3DR_INVALID);
    CHECK(Tango3DR_Config_setBool(c.p,"resolution",true) == TANGO_3DR_INVALID);
    CHECK(Tango3DR_Config_setDouble(c.p,"resolution",NAN) == TANGO_3DR_INVALID);
    CHECK(Tango3DR_Config_setDouble(c.p,"resolution",0) == TANGO_3DR_INVALID);
    CHECK(Tango3DR_Config_setInt32(c.p,"update_method",TANGO_3DR_PROJECTIVE_UPDATE) == TANGO_3DR_INVALID);
    CHECK(Tango3DR_Config_setBool(c.p,"use_floorplan",true) == TANGO_3DR_INVALID);
    Config t(TANGO_3DR_CONFIG_TEXTURING);
    CHECK(Tango3DR_Config_getInt32(t.p,"texture_size",&i) == 0 && i == 2048);
    CHECK(Tango3DR_Config_getDouble(t.p,"bevel",&d) == 0 && d == 3);
    CHECK(Tango3DR_Config_setInt32(t.p,"texturing_backend",1) == TANGO_3DR_INVALID);
    CHECK(Tango3DR_ReconstructionContext_create(t.p) == nullptr);
    c.real("min_depth",5); c.real("max_depth",1);
    CHECK(Tango3DR_ReconstructionContext_create(c.p) == nullptr);
    Tango3DR_PointCloud* p = new Tango3DR_PointCloud;
    CHECK(Tango3DR_PointCloud_init(9,p) == 0 && p->num_points == 9);
    CHECK(Tango3DR_PointCloud_destroy(p) == 0 && !p->points);
    CHECK(Tango3DR_PointCloud_destroy(p) == 0); delete p;
    Tango3DR_Mesh* m = static_cast<Tango3DR_Mesh*>(std::malloc(sizeof(Tango3DR_Mesh)));
    CHECK(m);
    CHECK(Tango3DR_Mesh_init(10,3,true,true,true,true,3,4,6,m) == 0);
    CHECK(m->num_textures == 0 && m->max_num_textures == 3 && m->textures[2].data);
    CHECK(m->textures[0].stride == 16 && m->texture_ids[2] == -1);
    m->num_textures = 1; // cleanup must also release unused allocated slots
    CHECK(Tango3DR_Mesh_destroy(m) == 0 && !m->textures);
    CHECK(Tango3DR_Mesh_destroy(m) == 0); std::free(m);
    Mesh huge;
    CHECK(Tango3DR_Mesh_init(UINT32_MAX,UINT32_MAX,true,true,true,true,0,0,0,&huge.p) == TANGO_3DR_INSUFFICIENT_SPACE);
    std::puts("config / optional attributes / member-only repeat cleanup: pass");
}

void planesAndPose() {
    Config cfg; defaults(cfg);
    Context c(cfg);
    Cloud points = plane(1.03f,.8,.02);
    std::set<Key> keys = update(c,points);
    Snapshot s = snapshot(c,keys);
    double maxerr = 0, rms = rmsPlane(s,glm::dvec3(0,0,1),1.03,&maxerr);
    CHECK(s.faces.size() > 1000 && s.area > 2.35 && s.area < 2.9);
    std::printf("plane: %zu triangles, area %.6f m2, RMS %.9f m, max %.9f m\n",s.faces.size(),s.area,rms,maxerr);
    CHECK(maxerr < 2e-6);
    for (const auto& n : s.normals) CHECK(n.z < -.999);
    // Geometrically weld chunk-local vertices and count internal edge incidence.
    typedef std::array<long long,3> Quantized;
    std::map<Quantized,uint32_t> weld;
    std::vector<uint32_t> ids;
    std::vector<glm::dvec3> unique;
    for (glm::dvec3 p : s.positions) {
        Quantized q{{std::llround(p.x*1e6),std::llround(p.y*1e6),std::llround(p.z*1e6)}};
        auto found = weld.emplace(q,uint32_t(unique.size()));
        if (found.second) unique.push_back(p);
        ids.push_back(found.first->second);
    }
    std::map<std::pair<uint32_t,uint32_t>,int> edges;
    for (const auto& f : s.faces) for (int j = 0; j < 3; ++j) {
        uint32_t a = ids[f[j]], b = ids[f[(j+1)%3]];
        CHECK(a != b); if (b < a) std::swap(a,b); ++edges[std::make_pair(a,b)];
    }
    size_t internal = 0;
    for (const auto& e : edges) {
        glm::dvec3 a = unique[e.first.first], b = unique[e.first.second];
        CHECK(e.second <= 2);
        if (std::abs(a.x) < .72 && std::abs(a.y) < .72 && std::abs(b.x) < .72 && std::abs(b.y) < .72) {
            CHECK(e.second == 2); ++internal;
        }
    }
    CHECK(internal > 1000);
    std::printf("negative/positive chunk seams: %zu interior edges have incidence 2\n",internal);
    // Even an enormous local noise threshold must preserve a connected plane
    // crossing chunk boundaries. Boundaries such as +/-0.64 are inexact floats.
    Config boundary_cfg; defaults(boundary_cfg);
    boundary_cfg.integer("min_num_vertices",1000000);
    Context boundary_filtered(boundary_cfg);
    Snapshot boundary_mesh = snapshot(boundary_filtered,update(boundary_filtered,points));
    CHECK(boundary_mesh.hash == s.hash);
    // Destroy/replay and repeated integration preserve geometry and metric scale.
    Context replay(cfg);
    auto replay_keys = update(replay,points);
    CHECK(snapshot(replay,replay_keys).hash == s.hash);
    auto dirty2 = update(c,points,identity(),nullptr,nullptr,2);
    keys.insert(dirty2.begin(),dirty2.end());
    CHECK(rmsPlane(snapshot(c,keys),glm::dvec3(0,0,1),1.03) < 2e-6);
    CHECK(Tango3DR_clear(c.p) == 0 && Tango3DR_clear(c.p) == 0);
    CHECK(snapshot(c,keys).faces.empty());
    // 90 degree camera rotation + negative translation, independently computed world plane.
    double angle = .73;
    glm::dquat q = glm::angleAxis(angle,glm::normalize(glm::dvec3(1,2,.5)));
    glm::dvec3 t(-.83,.22,-.17), n = q*glm::dvec3(0,0,1);
    auto transformed = pose(t,q);
    auto transformed_keys = update(c,points,transformed);
    Snapshot moved = snapshot(c,transformed_keys);
    rms = rmsPlane(moved,n,glm::dot(n,t)+1.03,&maxerr);
    CHECK(moved.faces.size() > 500 && maxerr < .001);
    std::printf("rotated/translated plane: RMS %.9f m, max %.9f m\n",rms,maxerr);
    // A genuinely sloped surface, not just a change of world coordinates.
    Context tilted(cfg);
    for (auto& p : points) p[2] = 1.03f+.2f*p[0]-.15f*p[1];
    Snapshot slope = snapshot(tilted,update(tilted,points));
    n = glm::normalize(glm::dvec3(-.2,.15,1));
    rms = rmsPlane(slope,n,1.03/std::sqrt(1+.04+.0225),&maxerr);
    std::printf("sloped plane: RMS %.6f m, max %.6f m\n",rms,maxerr);
    // No normals are supplied. One-voxel ray splats extrapolate at the finite
    // patch perimeter; require RMS < 0.1 voxel and worst error < 0.5 voxel.
    CHECK(rms < .004 && maxerr < .02);
    Context distant_origin(cfg);
    Cloud distant_points = plane(1.03f,.2,.015);
    auto distant_pose = identity();
    distant_pose.translation[0] = 1000.1; distant_pose.translation[1] = -999.7;
    distant_pose.translation[2] = 250.41;
    Snapshot far_origin = snapshot(distant_origin,update(distant_origin,distant_points,distant_pose));
    CHECK(rmsPlane(far_origin,glm::dvec3(0,0,1),251.44) < .0001);
}

void cube() {
    Config cfg; defaults(cfg,.025);
    Context c(cfg), replay(cfg);
    std::set<Key> keys;
    glm::dvec3 axis[6] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
    Cloud face = plane(1,.2,.01);
    for (glm::dvec3 outward : axis) {
        glm::dvec3 z(0,0,1), target = -outward;
        glm::dquat q;
        if (glm::dot(z,target) < -.99) q = glm::angleAxis(3.141592653589793,glm::dvec3(0,1,0));
        else q = glm::normalize(glm::dquat(1+glm::dot(z,target),glm::cross(z,target)));
        Tango3DR_Pose p = pose(outward*1.2,q);
        auto dirty = update(c,face,p);
        keys.insert(dirty.begin(),dirty.end());
        update(replay,face,p);
    }
    Snapshot s = snapshot(c,keys);
    CHECK(s.hash == snapshot(replay,keys).hash);
    double sum = 0, maximum = 0;
    for (glm::dvec3 p : s.positions) {
        glm::dvec3 q = glm::abs(p)-glm::dvec3(.2);
        double d = glm::length(glm::max(q,glm::dvec3(0))) + std::min(0.,std::max(q.x,std::max(q.y,q.z)));
        sum += d*d; maximum = std::max(maximum,std::abs(d));
    }
    CHECK(!s.positions.empty());
    double rms = std::sqrt(sum/s.positions.size());
    std::printf("six-view 0.4 m cube: %zu triangles, area %.6f m2, RMS %.6f m, max %.6f m\n",s.faces.size(),s.area,rms,maximum);
    CHECK(s.faces.size() > 1000 && s.area > .75 && s.area < 1.2);
    CHECK(rms < .018 && maximum < .04);
    // Every face has reconstructed samples near its centre and corners.
    for (glm::dvec3 outward : axis) {
        double closest = 100;
        for (glm::dvec3 p : s.positions) closest = std::min(closest,glm::length(p-outward*.2));
        CHECK(closest < .04);
    }
}

Tango3DR_CameraCalibration calibration(int w = 80, int h = 60) {
    Tango3DR_CameraCalibration c = {};
    c.width = w; c.height = h; c.fx = c.fy = 60; c.cx = (w-1)*.5; c.cy = (h-1)*.5;
    return c;
}
void colors() {
    Config cfg; defaults(cfg);
    Tango3DR_CameraCalibration cal = calibration();
    auto p = identity();
    Cloud points = plane(1.03f,.3,.015);
    for (auto format : {TANGO_3DR_HAL_PIXEL_FORMAT_RGB_888,TANGO_3DR_HAL_PIXEL_FORMAT_RGBA_8888,
                       TANGO_3DR_HAL_PIXEL_FORMAT_YCrCb_420_SP}) {
        Context c(cfg);
        CHECK(Tango3DR_ReconstructionContext_setColorCalibration(c.p,&cal) == 0);
        CHECK(Tango3DR_ReconstructionContext_setDepthCalibration(c.p,&cal) == 0);
        unsigned channels = format == TANGO_3DR_HAL_PIXEL_FORMAT_RGB_888 ? 3 : format == TANGO_3DR_HAL_PIXEL_FORMAT_RGBA_8888 ? 4 : 1;
        unsigned stride = 80*channels+8;
        std::vector<uint8_t> data(stride*60*(channels == 1 ? 2 : 1),17);
        for (unsigned y = 0; y < 60; ++y) for (unsigned x = 0; x < 80; ++x) {
            uint8_t* rgb = &data[y*stride+x*channels];
            if (channels == 1) *rgb = 81;
            else { rgb[0] = 210; rgb[1] = 40; rgb[2] = 15; }
        }
        if (channels == 1) for (unsigned y = 0; y < 30; ++y) for (unsigned x = 0; x < 80; x += 2) {
            data[stride*60+y*stride+x] = 240; data[stride*60+y*stride+x+1] = 90;
        }
        Tango3DR_ImageBuffer image = {80,60,stride,1,format,data.data()};
        Snapshot s = snapshot(c,update(c,points,p,&image,&p));
        CHECK(!s.colors.empty());
        for (auto rgb : s.colors) {
            if (channels == 1) CHECK(rgb.r > 250 && rgb.g < 3 && rgb.b < 3);
            else CHECK(std::abs(rgb.r-210) <= 1 && std::abs(rgb.g-40) <= 1 && std::abs(rgb.b-15) <= 1);
        }
    }
    // Offset color camera + spatial gradient + radial/tangential calibration.
    Context c(cfg);
    cal.calibration_type = TANGO_3DR_CALIBRATION_POLYNOMIAL_5_PARAMETERS;
    cal.distortion[0] = .2; cal.distortion[1] = -.02; cal.distortion[2] = .01; cal.distortion[3] = -.01;
    CHECK(Tango3DR_ReconstructionContext_setColorCalibration(c.p,&cal) == 0);
    std::vector<uint8_t> data(80*60*3);
    for (int y = 0; y < 60; ++y) for (int x = 0; x < 80; ++x) {
        data[(y*80+x)*3] = uint8_t(x*3); data[(y*80+x)*3+1] = uint8_t(y*4); data[(y*80+x)*3+2] = 70;
    }
    Tango3DR_ImageBuffer image = {80,60,240,1,TANGO_3DR_HAL_PIXEL_FORMAT_RGB_888,data.data()};
    auto color_pose = identity(); color_pose.translation[0] = .1;
    Snapshot s = snapshot(c,update(c,points,p,&image,&color_pose));
    double maxerror = 0;
    for (size_t i = 0; i < s.positions.size(); ++i) {
        auto v = s.positions[i];
        double x = (v.x-.1)/v.z, y = v.y/v.z, r = x*x+y*y;
        double radial = 1+.2*r-.02*r*r;
        double u = 60*(x*radial+.02*x*y-.01*(r+2*x*x))+cal.cx;
        double vv = 60*(y*radial+.01*(r+2*y*y)-.02*x*y)+cal.cy;
        maxerror = std::max(maxerror,std::max(std::abs(s.colors[i].r-u*3),std::abs(s.colors[i].g-vv*4)));
    }
    CHECK(maxerror < 7);
    std::printf("RGB/RGBA/NV21 padded strides + calibrated offset color: max gradient error %.3f /255\n",maxerror);
}

void noiseConfidenceAndClearing() {
    Config cfg; defaults(cfg);
    Context c(cfg);
    std::mt19937 rng(12345);
    std::normal_distribution<float> noise(0,.008f);
    std::set<Key> keys;
    for (int frame = 0; frame < 8; ++frame) {
        Cloud p = plane(1.03f);
        for (auto& point : p) point[2] += noise(rng);
        auto dirty = update(c,p,identity(),nullptr,nullptr,frame);
        keys.insert(dirty.begin(),dirty.end());
    }
    double maximum = 0;
    double rms = rmsPlane(snapshot(c,keys),glm::dvec3(0,0,1),1.03,&maximum);
    std::printf("8-frame 8 mm Gaussian noise: mesh RMS %.6f m, max %.6f m\n",rms,maximum);
    CHECK(rms < .003 && maximum < .012);
    Context weighted(cfg), high(cfg);
    Cloud a = plane(1.02f), b = plane(1.06f,.36,.015,.05f);
    auto wk = update(weighted,a); auto bk = update(weighted,b); wk.insert(bk.begin(),bk.end());
    update(high,a); for (auto& point : b) point[3] = 1;
    auto hk = update(high,b); hk.insert(wk.begin(),wk.end());
    CHECK(rmsPlane(snapshot(weighted,wk),glm::dvec3(0,0,1),1.02) < rmsPlane(snapshot(high,hk),glm::dvec3(0,0,1),1.02)*.6);
    cfg.boolean("use_space_clearing",true); cfg.integer("max_voxel_weight",4);
    Context cleared(cfg);
    Cloud near = plane(1.03f,.24,.02), far = plane(1.5f,.4,.02);
    auto ck = update(cleared,near);
    CHECK(!snapshot(cleared,ck).faces.empty());
    for (int i = 0; i < 6; ++i) { auto dk = update(cleared,far); ck.insert(dk.begin(),dk.end()); }
    Snapshot cs = snapshot(cleared,ck);
    CHECK(!cs.faces.empty());
    for (auto p : cs.positions) CHECK(p.z > 1.4);
    // Clearing across negative chunk boundaries and with a reversed camera ray.
    Context reversed(cfg);
    glm::dquat rotation = glm::angleAxis(3.141592653589793,glm::dvec3(0,1,0));
    glm::dvec3 translation(-.64,-.64,.67);
    auto reverse_pose = pose(translation,rotation);
    auto rk = update(reversed,near,reverse_pose);
    CHECK(!snapshot(reversed,rk).faces.empty());
    for (int i = 0; i < 6; ++i) { auto dk = update(reversed,far,reverse_pose); rk.insert(dk.begin(),dk.end()); }
    Snapshot rs = snapshot(reversed,rk);
    CHECK(!rs.faces.empty());
    for (auto p : rs.positions) CHECK((glm::conjugate(rotation)*(p-translation)).z > 1.4);
    // Empty free space must not consume the budget at voxel-by-voxel cost.
    cfg.integer("max_update_work",100000);
    Context distant(cfg);
    Cloud distant_plane = plane(4.03f,.08,.02);
    auto dk = update(distant,distant_plane);
    auto dk2 = update(distant,distant_plane); dk.insert(dk2.begin(),dk2.end());
    CHECK(rmsPlane(snapshot(distant,dk),glm::dvec3(0,0,1),4.03) < .001);
    cfg.integer("max_update_work",32000000);
    // Small isolated component wholly inside a chunk can be removed.
    cfg.boolean("use_space_clearing",false); cfg.integer("min_num_vertices",1000000);
    Context filtered(cfg);
    Cloud patch = plane(1.1f,.04,.01);
    for (auto& point : patch) { point[0] += .25f; point[1] += .25f; }
    CHECK(snapshot(filtered,update(filtered,patch)).faces.empty());
    std::puts("confidence weighting / free-space clearing / local component filter: pass");
}

void invalidAndBounds() {
    Config cfg; defaults(cfg);
    Context c(cfg);
    Cloud p = plane(1.03f,.15,.02);
    auto keys = update(c,p);
    uint64_t initial = snapshot(c,keys).hash;
    auto camera = identity();
    Tango3DR_GridIndexArray dirty = {};
    auto reject = [&](Tango3DR_PointCloud cloud, Tango3DR_Pose cp, Tango3DR_Status expected) {
        CHECK(Tango3DR_updateFromPointCloud(c.p,&cloud,&cp,nullptr,nullptr,&dirty) == expected);
        CHECK(!dirty.indices && !dirty.num_indices);
        CHECK(snapshot(c,keys).hash == initial);
    };
    Cloud bad = p; bad.back()[0] = NAN; reject(view(bad),camera,TANGO_3DR_INVALID);
    bad = p; bad.back()[3] = 1.1f; reject(view(bad),camera,TANGO_3DR_INVALID);
    bad = p; bad.back()[0] = 1e30f; reject(view(bad),camera,TANGO_3DR_INVALID);
    auto invalid_pose = camera; invalid_pose.orientation[3] = 0;
    reject(view(p),invalid_pose,TANGO_3DR_INVALID);
    invalid_pose = camera; invalid_pose.translation[1] = INFINITY;
    reject(view(p),invalid_pose,TANGO_3DR_INVALID);
    auto cloud = view(p); cloud.timestamp = NAN; reject(cloud,camera,TANGO_3DR_INVALID);
    cloud = view(p); cloud.points = nullptr; reject(cloud,camera,TANGO_3DR_INVALID);
    cloud = view(p); cloud.num_points = 1000001; reject(cloud,camera,TANGO_3DR_INSUFFICIENT_SPACE);
    // Inputs are borrowed, never retained; subsequent mutation cannot change a volume.
    for (auto& point : p) point.fill(0);
    CHECK(snapshot(c,keys).hash == initial);
    Cloud good = plane(1.04f,.15,.02); update(c,good);
    CHECK(snapshot(c,keys).hash != initial);
    Config bounded; defaults(bounded); bounded.integer("max_chunks",1);
    Context limit(bounded);
    cloud = view(good);
    CHECK(Tango3DR_updateFromPointCloud(limit.p,&cloud,&camera,nullptr,nullptr,&dirty) == TANGO_3DR_INSUFFICIENT_SPACE);
    CHECK(snapshot(limit,keys).faces.empty());
    Cloud one = {{{.15f,.15f,1.03f,1}}}; update(limit,one);
    bounded.integer("max_chunks",1024); bounded.integer("max_update_chunks",1);
    Context update_limit(bounded);
    CHECK(Tango3DR_updateFromPointCloud(update_limit.p,&cloud,&camera,nullptr,nullptr,&dirty) == TANGO_3DR_INSUFFICIENT_SPACE);
    CHECK(snapshot(update_limit,keys).faces.empty());
    bounded.integer("max_update_chunks",256); bounded.integer("max_update_work",100);
    Context work_limit(bounded);
    CHECK(Tango3DR_updateFromPointCloud(work_limit.p,&cloud,&camera,nullptr,nullptr,&dirty) == TANGO_3DR_INSUFFICIENT_SPACE);
    CHECK(snapshot(work_limit,keys).faces.empty());
    Config depth; defaults(depth); depth.real("min_depth",1); depth.real("max_depth",2); depth.real("min_confidence",.2);
    Context filter(depth);
    Cloud rejected = {{{0,0,.5f,1}},{{0,0,3,1}},{{0,0,1.5f,0}},{{0,0,1.5f,.1f}},{{0,0,-1,1}}};
    CHECK(update(filter,rejected).empty());
    Cloud empty; CHECK(update(filter,empty).empty());
    // Color validation is also atomic, and valid calibration survives a bad setter.
    auto cal = calibration();
    std::vector<uint8_t> pixels(80*60*3,80);
    Tango3DR_ImageBuffer image = {80,60,240,1,TANGO_3DR_HAL_PIXEL_FORMAT_RGB_888,pixels.data()};
    cloud = view(good);
    initial = snapshot(c,keys).hash;
    CHECK(Tango3DR_updateFromPointCloud(c.p,&cloud,&camera,&image,&camera,&dirty) == TANGO_3DR_ERROR);
    CHECK(Tango3DR_ReconstructionContext_setColorCalibration(c.p,&cal) == 0);
    auto invalid_cal = cal; invalid_cal.fx = NAN;
    CHECK(Tango3DR_ReconstructionContext_setColorCalibration(c.p,&invalid_cal) == TANGO_3DR_INVALID);
    image.stride = 79;
    CHECK(Tango3DR_updateFromPointCloud(c.p,&cloud,&camera,&image,&camera,&dirty) == TANGO_3DR_INVALID);
    image.stride = 240; image.width = 79;
    CHECK(Tango3DR_updateFromPointCloud(c.p,&cloud,&camera,&image,&camera,&dirty) == TANGO_3DR_INVALID);
    CHECK(snapshot(c,keys).hash == initial);
    image.width = 80;
    update(c,good,camera,&image,&camera);
    std::puts("invalid frame / borrowed inputs / depth-confidence filters / chunk-work limits / next-good recovery: pass");
}

void incrementalAndOptions() {
    Config cfg; defaults(cfg,.05);
    Context c(cfg), replay(cfg);
    std::set<Key> all;
    std::map<Key,uint64_t> cache;
    Cloud patch = plane(1.0f,.25,.015);
    for (int frame = 0; frame < 4; ++frame) {
        auto p = identity(); p.translation[0] = -.5+frame*.35;
        auto dirty = update(c,patch,p);
        auto rdirty = update(replay,patch,p);
        CHECK(dirty == rdirty);
        all.insert(dirty.begin(),dirty.end());
        for (auto k : dirty) cache[k] = snapshot(c,std::set<Key>{k}).hash;
        for (auto k : all) CHECK(cache[k] == snapshot(c,std::set<Key>{k}).hash);
        CHECK(snapshot(c,all).hash == snapshot(replay,all).hash);
    }
    CHECK(snapshot(c,all).area > .5);
    cfg.boolean("generate_color",false); cfg.boolean("use_clockwise_winding_order",true);
    cfg.boolean("use_parallel_integration",true);
    Context options(cfg);
    auto keys = update(options,patch);
    Snapshot s = snapshot(options,keys);
    CHECK(!s.faces.empty());
    for (auto color : s.colors) CHECK(color.r == -1);
    for (auto f : s.faces) CHECK(glm::cross(s.positions[f[1]]-s.positions[f[0]],s.positions[f[2]]-s.positions[f[0]]).z > 0);
    for (auto n : s.normals) CHECK(n.z < -.99);
    // Context configuration is a snapshot, unaffected by subsequent config edits.
    cfg.real("resolution",.1);
    CHECK(snapshot(options,keys).hash == s.hash);
    std::puts("incremental dirty-cache vs full extraction / replay / exact lattice plane / winding / optional color: pass");
}

void allocationFailures() {
    // Sweep every allocation in optional-attribute + texture initialization.
    int mesh_failures = 0;
    for (int n = 0; n < 32; ++n) {
        Mesh m;
        ReconstructionCore_testFailAfter(n);
        auto status = Tango3DR_Mesh_init(8,4,true,true,true,true,2,8,8,&m.p);
        ReconstructionCore_testFailAfter(-1);
        if (status == 0) break;
        ++mesh_failures;
        CHECK(status == TANGO_3DR_ERROR && !m.p.vertices && !m.p.textures);
    }
    CHECK(mesh_failures == 9);
    Config cfg; defaults(cfg,.05);
    Context c(cfg);
    Cloud baseline = plane(1.07f,.07,.015);
    for (auto& p : baseline) { p[0] += .3f; p[1] += .3f; }
    auto keys = update(c,baseline);
    Snapshot original = snapshot(c,keys);
    CHECK(!original.faces.empty());
    Cloud changed = baseline; for (auto& p : changed) p[2] += .015f;
    // Exercise both copy-on-write existing chunks and insertion of new chunks.
    Cloud extra = changed; for (auto& p : extra) p[0] += 1.6f;
    changed.insert(changed.end(),extra.begin(),extra.end());
    auto potential = keys;
    for (auto k : keys) { k[0] += 2; potential.insert(k); }
    auto cloud = view(changed); auto camera = identity();
    int update_failures = 0;
    for (int n = 0; n < 512; ++n) {
        Tango3DR_GridIndexArray dirty = {};
        ReconstructionCore_testFailAfter(n);
        auto status = Tango3DR_updateFromPointCloud(c.p,&cloud,&camera,nullptr,nullptr,&dirty);
        ReconstructionCore_testFailAfter(-1);
        if (status == 0) { Tango3DR_GridIndexArray_destroy(&dirty); break; }
        ++update_failures;
        CHECK(status == TANGO_3DR_ERROR && !dirty.indices && !dirty.num_indices);
        CHECK(snapshot(c,keys).hash == original.hash);
        for (auto k : potential) if (!keys.count(k)) CHECK(snapshot(c,std::set<Key>{k}).faces.empty());
    }
    CHECK(update_failures > 5 && update_failures < 512);
    CHECK(snapshot(c,keys).hash != original.hash);
    // Sweep all extraction allocations on a nonempty segment.
    Key active;
    for (auto k : keys) { Mesh m; CHECK(Tango3DR_extractMeshSegment(c.p,k.data(),&m.p) == 0); if (m.p.num_faces) { active = k; break; } }
    int extract_failures = 0;
    auto before = snapshot(c,keys).hash;
    for (int n = 0; n < 4096; ++n) {
        Mesh m;
        ReconstructionCore_testFailAfter(n);
        auto status = Tango3DR_extractMeshSegment(c.p,active.data(),&m.p);
        ReconstructionCore_testFailAfter(-1);
        if (status == 0) break;
        ++extract_failures;
        CHECK(status == TANGO_3DR_ERROR && !m.p.vertices && !m.p.faces && !m.p.colors);
    }
    CHECK(extract_failures > 10 && extract_failures < 4096);
    CHECK(snapshot(c,keys).hash == before);
    std::printf("allocation sweep: %d mesh, %d update, %d extraction failure positions; rollback and cleanup pass\n",mesh_failures,update_failures,extract_failures);
}
void manyChunkAllocationRecovery() {
    Config cfg; defaults(cfg,.025);
    Context c(cfg);
    Cloud baseline = plane(1.03f,1.6,.02); // >64 chunks: forces transaction cache eviction
    auto keys = update(c,baseline);
    Snapshot before = snapshot(c,keys);
    CHECK(before.faces.size() > 1000);
    Cloud changed = baseline;
    for (auto& p : changed) p[2] += .01f;
    auto cloud = view(changed,2); auto camera = identity();
    for (long fail : {0L,150L,300L}) {
        Tango3DR_GridIndexArray dirty = {};
        ReconstructionCore_testFailAfter(fail);
        auto status = Tango3DR_updateFromPointCloud(c.p,&cloud,&camera,nullptr,nullptr,&dirty);
        ReconstructionCore_testFailAfter(-1);
        CHECK(status == TANGO_3DR_ERROR && !dirty.num_indices && !dirty.indices);
        CHECK(snapshot(c,keys).hash == before.hash);
    }
    auto dirty = update(c,changed,camera,nullptr,nullptr,2);
    keys.insert(dirty.begin(),dirty.end());
    CHECK(snapshot(c,keys).hash != before.hash);
    std::puts("multi-chunk cache eviction / early-mid-late allocation rollback / retry: pass");
}
int main() {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    configAndOwnership(); planesAndPose(); cube(); colors(); noiseConfidenceAndClearing();
    invalidAndBounds(); incrementalAndOptions(); allocationFailures(); manyChunkAllocationRecovery();
    std::puts("ALL CORE TESTS PASSED");
    return 0;
}
