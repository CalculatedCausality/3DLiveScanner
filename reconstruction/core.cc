// Copyright 2026 3DLiveScanner contributors. SPDX-License-Identifier: Apache-2.0
// Independent CPU reconstruction; public Tango ABI, no vendor implementation.
#include <tango_3d_reconstruction_api.h>
#include "paging.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <vector>
#include <cassert>
#include <cerrno>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace recon {
const size_t kAllocationLimit = 256u * 1024u * 1024u;
const uint32_t kPointLimit = 1000000;
const int kCoordinateLimit = 1000000; // lattice coordinates, not metres
#ifdef RECONSTRUCTION_CORE_TESTING
thread_local long fail_after = -1;
void allocationPoint() {
    if (fail_after == 0) throw std::bad_alloc();
    if (fail_after > 0) --fail_after;
}
#else
void allocationPoint() {}
#endif
template<class T> struct Allocator {
    typedef T value_type;
    Allocator() noexcept {}
    template<class U> Allocator(const Allocator<U>&) noexcept {}
    template<class U> struct rebind { typedef Allocator<U> other; };
    T* allocate(size_t n) {
        if (n > kAllocationLimit / sizeof(T)) throw std::bad_alloc();
        allocationPoint();
        return static_cast<T*>(::operator new(n * sizeof(T)));
    }
    void deallocate(T* p, size_t) noexcept { ::operator delete(p); }
};
template<class T, class U> bool operator==(const Allocator<T>&, const Allocator<U>&) { return true; }
template<class T, class U> bool operator!=(const Allocator<T>&, const Allocator<U>&) { return false; }
template<class T> using Vector = std::vector<T, Allocator<T> >;
template<class K, class V> using Map = std::map<K, V, std::less<K>, Allocator<std::pair<const K, V> > >;
template<class K> using Set = std::set<K, std::less<K>, Allocator<K> >;
template<class T> T* buffer(size_t n) {
    if (!n) return nullptr;
    if (n > kAllocationLimit / sizeof(T)) throw std::bad_alloc();
    allocationPoint();
    void* p = std::calloc(n, sizeof(T));
    if (!p) throw std::bad_alloc();
    return static_cast<T*>(p);
}
enum Key {
    Resolution, MinDepth, MaxDepth, GenerateColor, Clearing, Parallel, Clockwise,
    Rectify, MaxWeight, MinVertices, UpdateMethod, MaxChunks, MaxUpdateChunks,
    MaxWork, MinConfidence, MinSurfaceWeight, TextureBackend, Simplification, TextureSize,
    Bevel, MinResolution, MaxTextures, Downsample, KeyCount
};
enum Kind { Boolean, Double, Integer };
struct Setting { const char* name; Kind kind; int type; double value, low, high; };
const Setting settings[KeyCount] = {
    {"resolution", Double, 0, .03, .001, 1},
    {"min_depth", Double, 0, .60, 0, 100},
    {"max_depth", Double, 0, 3.50, .001, 100},
    {"generate_color", Boolean, 0, 1, 0, 1},
    {"use_space_clearing", Boolean, 0, 0, 0, 1},
    {"use_parallel_integration", Boolean, 0, 0, 0, 1}, // scheduling hint
    {"use_clockwise_winding_order", Boolean, 0, 0, 0, 1},
    {"rectify_color_image", Boolean, 0, 1, 0, 1},
    {"max_voxel_weight", Integer, 0, 16383, 1, 65535},
    {"min_num_vertices", Integer, 0, 1, 0, 1000000},
    {"update_method", Integer, 0, 0, 0, 0}, // traversal only
    {"max_chunks", Integer, 0, 1024, 1, 4096},
    {"max_update_chunks", Integer, 0, 256, 1, 1024},
    {"max_update_work", Integer, 0, 32000000, 1, 64000000},
    {"min_confidence", Double, 0, 0, 0, 1},
    {"min_voxel_weight", Double, 0, 1, .001, 65535},
    {"texturing_backend", Integer, 1, 0, 0, 0},
    {"mesh_simplification_factor", Integer, 1, 3, 1, 1000},
    {"texture_size", Integer, 1, 2048, 1, 8192},
    {"bevel", Double, 1, 3, 0, 256},
    {"min_resolution", Double, 1, 0, 0, 100},
    {"max_num_textures", Integer, 1, 0, 0, 256},
    {"downsample", Integer, 1, 1, 1, 1000000}
};
} // namespace recon
struct _Tango3DR_Config {
    Tango3DR_ConfigType type;
    double values[recon::KeyCount];
    explicit _Tango3DR_Config(Tango3DR_ConfigType t) : type(t) {
        for (int i = 0; i < recon::KeyCount; ++i) values[i] = recon::settings[i].value;
    }
};
namespace recon {
int findKey(Tango3DR_Config c, const char* name, Kind kind) {
    if (!c || !name) return -1;
    for (int i = 0; i < KeyCount; ++i)
        if (settings[i].kind == kind && settings[i].type == c->type &&
            std::strcmp(name, settings[i].name) == 0) return i;
    return -1;
}
Tango3DR_Status set(Tango3DR_Config c, const char* name, Kind kind, double v) {
    int k = findKey(c, name, kind);
    if (k < 0 || !std::isfinite(v) || v < settings[k].low || v > settings[k].high)
        return TANGO_3DR_INVALID;
    c->values[k] = v;
    return TANGO_3DR_SUCCESS;
}
template<class T> Tango3DR_Status get(Tango3DR_Config c, const char* name, Kind kind, T* v) {
    int k = findKey(c, name, kind);
    if (k < 0 || !v) return TANGO_3DR_INVALID;
    *v = static_cast<T>(c->values[k]);
    return TANGO_3DR_SUCCESS;
}
struct Index {
    int x, y, z;
    Index(int a = 0, int b = 0, int c = 0) : x(a), y(b), z(c) {}
    bool operator<(const Index& o) const {
        return x != o.x ? x < o.x : y != o.y ? y < o.y : z < o.z;
    }
};
int floor16(int x) { return x >= 0 ? x / 16 : (x - 15) / 16; }
Index chunkOf(const Index& p) { return Index(floor16(p.x), floor16(p.y), floor16(p.z)); }
int offset(const Index& p) {
    return (p.x - floor16(p.x) * 16) + 16 * (p.y - floor16(p.y) * 16) +
        256 * (p.z - floor16(p.z) * 16);
}
glm::dvec3 position(const Index& p) { return glm::dvec3(p.x, p.y, p.z); }
struct Voxel {
    float sdf, weight, color[3], color_weight;
    Voxel() : sdf(0), weight(0), color{0, 0, 0}, color_weight(0) {}
};
struct Chunk { Voxel voxels[4096]; };
static_assert(sizeof(Chunk) == 98304, "Paging payload/accounting ABI changed");
typedef Map<Index, std::shared_ptr<Chunk> > Volume;
#include "paging_store.h"
struct Pose {
    glm::dvec3 t;
    glm::dquat q;
    glm::dvec3 world(const glm::dvec3& p) const { return q * p + t; }
    glm::dvec3 local(const glm::dvec3& p) const { return glm::conjugate(q) * (p - t); }
};
bool pose(const Tango3DR_Pose* p, Pose& out) {
    if (!p) return false;
    double n = 0;
    for (int j = 0; j < 3; ++j) if (!std::isfinite(p->translation[j])) return false;
    for (int j = 0; j < 4; ++j) {
        if (!std::isfinite(p->orientation[j])) return false;
        n += p->orientation[j] * p->orientation[j];
    }
    if (std::abs(n - 1) >= .01) return false;
    out.t = glm::dvec3(p->translation[0], p->translation[1], p->translation[2]);
    out.q = glm::normalize(glm::dquat(p->orientation[3], p->orientation[0],
                                    p->orientation[1], p->orientation[2]));
    return true;
}
bool calibration(const Tango3DR_CameraCalibration* c) {
    if (!c || !c->width || !c->height || c->width > 8192 || c->height > 8192 ||
        !std::isfinite(c->fx) || !std::isfinite(c->fy) || c->fx <= 0 || c->fy <= 0 ||
        !std::isfinite(c->cx) || !std::isfinite(c->cy) ||
        c->calibration_type < 0 || c->calibration_type > 4) return false;
    for (double d : c->distortion) if (!std::isfinite(d)) return false;
    return c->calibration_type != TANGO_3DR_CALIBRATION_EQUIDISTANT || std::abs(c->distortion[0]) < 3.14;
}
bool imageValid(const Tango3DR_ImageBuffer* im, const Tango3DR_CameraCalibration& c) {
    if (!im || !im->data || im->width != c.width || im->height != c.height ||
        !std::isfinite(im->timestamp) || im->stride > 65536) return false;
    switch (im->format) {
        case TANGO_3DR_HAL_PIXEL_FORMAT_RGB_888: return im->stride >= im->width * 3;
        case TANGO_3DR_HAL_PIXEL_FORMAT_RGBA_8888: return im->stride >= im->width * 4;
        case TANGO_3DR_HAL_PIXEL_FORMAT_YCrCb_420_SP:
            return im->stride >= im->width && !(im->width % 2) && !(im->height % 2) && !(im->stride % 2);
        default: return false;
    }
}
double clamp(double x, double lo, double hi) { return std::max(lo, std::min(hi, x)); }
glm::dvec3 pixel(const Tango3DR_ImageBuffer& im, int x, int y) {
    const uint8_t* row = im.data + size_t(y) * im.stride;
    if (im.format != TANGO_3DR_HAL_PIXEL_FORMAT_YCrCb_420_SP) {
        row += x * (im.format == TANGO_3DR_HAL_PIXEL_FORMAT_RGB_888 ? 3 : 4);
        return glm::dvec3(row[0], row[1], row[2]);
    }
    const uint8_t* uv = im.data + size_t(im.stride) * im.height + size_t(y / 2) * im.stride + (x / 2) * 2;
    double yy = 1.16438356 * (int(row[x]) - 16), v = int(uv[0]) - 128, u = int(uv[1]) - 128;
    return glm::dvec3(clamp(yy + 1.596027 * v, 0, 255),
                      clamp(yy - .391762 * u - .812968 * v, 0, 255),
                      clamp(yy + 2.017232 * u, 0, 255));
}
bool sample(const Tango3DR_ImageBuffer& im, const Tango3DR_CameraCalibration& c,
            const Pose& camera, const glm::dvec3& world, bool distort, glm::dvec3& rgb) {
    glm::dvec3 p = camera.local(world);
    if (p.z <= 1e-8) return false;
    double x = p.x / p.z, y = p.y / p.z, r2 = x*x + y*y;
    if (distort) {
        const double* d = c.distortion;
        if (c.calibration_type == TANGO_3DR_CALIBRATION_EQUIDISTANT) {
            double r = std::sqrt(r2), w = d[0];
            double s = r > 1e-12 && std::abs(w) > 1e-12 ? std::atan(2*r*std::tan(w/2))/(w*r) : 1;
            x *= s; y *= s;
        } else if (c.calibration_type != TANGO_3DR_CALIBRATION_UNKNOWN) {
            double radial = 1 + r2 * (d[0] + r2 * (d[1] + r2 *
                (c.calibration_type == TANGO_3DR_CALIBRATION_POLYNOMIAL_3_PARAMETERS ? d[2] :
                 c.calibration_type == TANGO_3DR_CALIBRATION_POLYNOMIAL_5_PARAMETERS ? d[4] : 0)));
            double xx = x * radial, yy = y * radial;
            if (c.calibration_type == TANGO_3DR_CALIBRATION_POLYNOMIAL_5_PARAMETERS) {
                xx += 2*d[2]*x*y + d[3]*(r2 + 2*x*x);
                yy += d[2]*(r2 + 2*y*y) + 2*d[3]*x*y;
            }
            x = xx; y = yy;
        }
    }
    double u = c.fx*x + c.cx, v = c.fy*y + c.cy;
    if (!std::isfinite(u) || !std::isfinite(v) || u < 0 || v < 0 || u > im.width-1 || v > im.height-1) return false;
    int ix = int(u), iy = int(v), jx = std::min(ix+1, int(im.width)-1), jy = std::min(iy+1, int(im.height)-1);
    double a = u-ix, b = v-iy;
    rgb = (1-b)*((1-a)*pixel(im,ix,iy) + a*pixel(im,jx,iy)) +
          b*((1-a)*pixel(im,ix,jy) + a*pixel(im,jx,jy));
    return true;
}
const Voxel* lookup(const Volume& volume, const Index& p) {
    auto it = volume.find(chunkOf(p));
    return it == volume.end() ? nullptr : &it->second->voxels[offset(p)];
}
enum LimitKind { WorkLimit, VolumeLimit, FrameLimit, PointLimit, LimitKindCount };
struct Limit {
    LimitKind kind;
    size_t requested, capacity, work, touched, staged;
    Limit(LimitKind k, size_t request, size_t cap, size_t w, size_t t, size_t s)
        : kind(k), requested(request), capacity(cap), work(w), touched(t), staged(s) {}
};
void reportLimit(const Limit& limit, size_t committed, uint32_t points) noexcept {
    // Process-wide, per-reason throttling survives context replacement/replay.
    // No heap allocations or changes to reconstruction state. Contention drops
    // a diagnostic rather than waiting in an already rejected update.
    static std::atomic_flag busy = ATOMIC_FLAG_INIT;
    static bool reported[LimitKindCount] = {};
    static std::chrono::steady_clock::time_point last[LimitKindCount];
    if (busy.test_and_set(std::memory_order_acquire)) return;
    const auto now = std::chrono::steady_clock::now();
    const bool emit = !reported[limit.kind] || now-last[limit.kind] >= std::chrono::seconds(5);
    if (emit) { reported[limit.kind] = true; last[limit.kind] = now; }
    busy.clear(std::memory_order_release);
    if (!emit) return;
    static const char* names[LimitKindCount] = {
        "max_update_work", "max_chunks", "max_update_chunks", "max_points_per_frame"
    };
    const char* format = "resource_limit=%s requested=%zu limit=%zu work=%zu touched_chunks=%zu "
        "staged_chunks=%zu committed_chunks=%zu points=%u; frame rejected, volume unchanged\n";
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_WARN,"ScannerReconstruction",format,names[limit.kind],
        limit.requested,limit.capacity,limit.work,limit.touched,limit.staged,committed,points);
#else
    std::fprintf(stderr,"ScannerReconstruction: ");
    std::fprintf(stderr,format,names[limit.kind],limit.requested,limit.capacity,limit.work,
        limit.touched,limit.staged,committed,points);
#endif
}
} // namespace recon
struct _Tango3DR_ReconstructionContext {
    _Tango3DR_Config config;
    recon::Volume volume;
    // Records must be destroyed before their owning Pager/descriptor.
    std::unique_ptr<recon::Pager> pager;
    recon::PagedVolume paged_volume;
    uint32_t last_failure = SCANNER_RECONSTRUCTION_FAILURE_NONE;
    uint64_t ram_peak = 0;
    Tango3DR_CameraCalibration color, depth;
    bool has_color, has_depth;
    double timestamp;
    explicit _Tango3DR_ReconstructionContext(const _Tango3DR_Config& c)
        : config(c), color(), depth(), has_color(false), has_depth(false), timestamp(0) {}
};

extern "C" {
#ifdef RECONSTRUCTION_CORE_TESTING
// Test-only allocation failpoint. Absent from production builds/ABI.
void ReconstructionCore_testFailAfter(long n) { recon::fail_after = n; }
void ReconstructionCore_testPagingFault(Tango3DR_ReconstructionContext c, int kind, int64_t after) {
    if (c && c->pager) c->pager->setFault(kind,after);
}
int ReconstructionCore_testPagingFd(Tango3DR_ReconstructionContext c) {
    return c && c->pager ? c->pager->descriptor() : -1;
}
#endif
Tango3DR_Status ScannerReconstruction_enablePaging(Tango3DR_ReconstructionContext c,
    const char* directory, uint64_t resident, uint64_t backing, uint32_t logical) {
    if (!c) return TANGO_3DR_INVALID;
    c->last_failure = SCANNER_RECONSTRUCTION_FAILURE_NONE;
    if (!directory || !*directory || std::strlen(directory) > 4000 || c->pager || !c->volume.empty() ||
        !logical || logical > 32768) {
        c->last_failure = SCANNER_RECONSTRUCTION_FAILURE_BAD_ARGUMENT;
        return TANGO_3DR_INVALID;
    }
    if (resident/sizeof(recon::Chunk) < 8) {
        c->last_failure = SCANNER_RECONSTRUCTION_FAILURE_MEMORY_LIMIT;
        return TANGO_3DR_INVALID;
    }
    if (backing < sizeof(recon::Chunk) || backing > uint64_t(std::numeric_limits<off_t>::max())) {
        c->last_failure = SCANNER_RECONSTRUCTION_FAILURE_BACKING_LIMIT;
        return TANGO_3DR_INVALID;
    }
    try {
        recon::allocationPoint();
        std::unique_ptr<recon::Pager> pager(new recon::Pager(resident,backing,logical,
            uint32_t(c->config.values[recon::MaxUpdateChunks])));
        pager->open(directory);
        c->pager.swap(pager);
        return TANGO_3DR_SUCCESS;
    } catch (const recon::PagingFailure& e) { c->last_failure = e.reason; return e.status; }
      catch (...) { c->last_failure = SCANNER_RECONSTRUCTION_FAILURE_MEMORY_LIMIT; return TANGO_3DR_ERROR; }
}
Tango3DR_Status ScannerReconstruction_getPagingStats(Tango3DR_ReconstructionContext c,
    ScannerReconstruction_PagingStats* out) {
    if (!c || !out) return TANGO_3DR_INVALID;
    *out = ScannerReconstruction_PagingStats();
    out->chunk_bytes = sizeof(recon::Chunk);
    out->last_failure = c->last_failure;
    if (c->pager) {
        c->pager->stats(*out); out->logical_chunks = c->paged_volume.size();
    } else {
        out->logical_chunks = out->resident_chunks = c->volume.size();
        out->peak_resident_chunks = c->ram_peak;
        out->resident_bytes = c->volume.size()*sizeof(recon::Chunk);
        out->peak_resident_bytes = c->ram_peak*sizeof(recon::Chunk);
    }
    return TANGO_3DR_SUCCESS;
}
Tango3DR_Config Tango3DR_Config_create(Tango3DR_ConfigType type) {
    if (type != TANGO_3DR_CONFIG_RECONSTRUCTION && type != TANGO_3DR_CONFIG_TEXTURING) return nullptr;
    try { recon::allocationPoint(); return new _Tango3DR_Config(type); } catch (...) { return nullptr; }
}
Tango3DR_Status Tango3DR_Config_destroy(Tango3DR_Config c) {
    if (!c) return TANGO_3DR_INVALID;
    delete c; return TANGO_3DR_SUCCESS;
}
Tango3DR_Status Tango3DR_Config_setBool(Tango3DR_Config c, const char* k, bool v) { return recon::set(c,k,recon::Boolean,v); }
Tango3DR_Status Tango3DR_Config_setDouble(Tango3DR_Config c, const char* k, double v) { return recon::set(c,k,recon::Double,v); }
Tango3DR_Status Tango3DR_Config_setInt32(Tango3DR_Config c, const char* k, int32_t v) { return recon::set(c,k,recon::Integer,v); }
Tango3DR_Status Tango3DR_Config_getBool(const Tango3DR_Config c, const char* k, bool* v) { return recon::get(c,k,recon::Boolean,v); }
Tango3DR_Status Tango3DR_Config_getDouble(const Tango3DR_Config c, const char* k, double* v) { return recon::get(c,k,recon::Double,v); }
Tango3DR_Status Tango3DR_Config_getInt32(const Tango3DR_Config c, const char* k, int32_t* v) { return recon::get(c,k,recon::Integer,v); }
Tango3DR_Status Tango3DR_PointCloud_initEmpty(Tango3DR_PointCloud* c) {
    if (!c) return TANGO_3DR_INVALID;
    *c = Tango3DR_PointCloud(); return TANGO_3DR_SUCCESS;
}
Tango3DR_Status Tango3DR_PointCloud_init(uint32_t n, Tango3DR_PointCloud* c) {
    if (!c) return TANGO_3DR_INVALID;
    *c = Tango3DR_PointCloud();
    if (n > recon::kPointLimit) return TANGO_3DR_INSUFFICIENT_SPACE;
    try { c->points = recon::buffer<Tango3DR_Vector4>(n); c->num_points = n; return TANGO_3DR_SUCCESS; }
    catch (...) { return TANGO_3DR_ERROR; }
}
Tango3DR_Status Tango3DR_PointCloud_destroy(Tango3DR_PointCloud* c) {
    if (!c) return TANGO_3DR_INVALID;
    std::free(c->points); *c = Tango3DR_PointCloud(); return TANGO_3DR_SUCCESS;
}
Tango3DR_Status Tango3DR_GridIndexArray_destroy(Tango3DR_GridIndexArray* a) {
    if (!a) return TANGO_3DR_INVALID;
    std::free(a->indices); *a = Tango3DR_GridIndexArray(); return TANGO_3DR_SUCCESS;
}
Tango3DR_Status Tango3DR_Mesh_destroy(Tango3DR_Mesh* m) {
    if (!m) return TANGO_3DR_INVALID;
    std::free(m->vertices); std::free(m->faces); std::free(m->normals); std::free(m->colors);
    std::free(m->texture_coords); std::free(m->texture_ids);
    if (m->textures) {
        // All allocated slots are owned, including unused texture capacity.
        for (uint32_t i = 0; i < m->max_num_textures; ++i) std::free(m->textures[i].data);
        std::free(m->textures);
    }
    *m = Tango3DR_Mesh(); return TANGO_3DR_SUCCESS;
}
Tango3DR_Status Tango3DR_Mesh_init(uint32_t nv, uint32_t nf, bool normals, bool colors,
    bool uv, bool ids, uint32_t nt, uint32_t w, uint32_t h, Tango3DR_Mesh* m) {
    if (!m) return TANGO_3DR_INVALID;
    *m = Tango3DR_Mesh();
    if (nt && (!w || !h || w > 8192 || h > 8192)) return TANGO_3DR_INVALID;
    const uint64_t bytes = uint64_t(nv)*(12 + (normals?12:0) + (colors?4:0) + (uv?8:0)) +
        uint64_t(nf)*(12 + (ids?4:0)) + uint64_t(nt)*(sizeof(Tango3DR_ImageBuffer) + uint64_t(w)*h*4);
    if (nt > 256 || bytes > recon::kAllocationLimit) return TANGO_3DR_INSUFFICIENT_SPACE;
    try {
        m->vertices = recon::buffer<Tango3DR_Vector3>(nv);
        m->faces = recon::buffer<Tango3DR_Face>(nf);
        if (normals) m->normals = recon::buffer<Tango3DR_Vector3>(nv);
        if (colors) m->colors = recon::buffer<Tango3DR_Color>(nv);
        if (uv) m->texture_coords = recon::buffer<Tango3DR_TexCoord>(nv);
        if (ids) {
            m->texture_ids = recon::buffer<int32_t>(nf);
            for (uint32_t i = 0; i < nf; ++i) m->texture_ids[i] = -1;
        }
        m->textures = recon::buffer<Tango3DR_ImageBuffer>(nt);
        m->max_num_textures = nt;
        for (uint32_t i = 0; i < nt; ++i) {
            Tango3DR_ImageBuffer& im = m->textures[i];
            im.width = w; im.height = h; im.stride = w*4;
            im.format = TANGO_3DR_HAL_PIXEL_FORMAT_RGBA_8888;
            im.data = recon::buffer<uint8_t>(size_t(w)*h*4);
        }
        m->max_num_vertices = nv; m->max_num_faces = nf;
        return TANGO_3DR_SUCCESS;
    } catch (...) { Tango3DR_Mesh_destroy(m); return TANGO_3DR_ERROR; }
}
Tango3DR_ReconstructionContext Tango3DR_ReconstructionContext_create(const Tango3DR_Config c) {
    try {
        _Tango3DR_Config defaults(TANGO_3DR_CONFIG_RECONSTRUCTION);
        const _Tango3DR_Config& cfg = c ? *c : defaults;
        if (cfg.type != TANGO_3DR_CONFIG_RECONSTRUCTION ||
            cfg.values[recon::MinDepth] >= cfg.values[recon::MaxDepth] ||
            cfg.values[recon::MinSurfaceWeight] > cfg.values[recon::MaxWeight]) return nullptr;
        recon::allocationPoint();
        return new _Tango3DR_ReconstructionContext(cfg);
    } catch (...) { return nullptr; }
}
Tango3DR_Status Tango3DR_ReconstructionContext_destroy(Tango3DR_ReconstructionContext c) {
    if (!c) return TANGO_3DR_INVALID;
    delete c; return TANGO_3DR_SUCCESS;
}
Tango3DR_Status Tango3DR_clear(Tango3DR_ReconstructionContext c) {
    if (!c) return TANGO_3DR_INVALID;
    c->volume.clear(); c->paged_volume.clear();
    if (c->pager) c->pager->cleared();
    c->last_failure = SCANNER_RECONSTRUCTION_FAILURE_NONE;
    c->timestamp = 0; return TANGO_3DR_SUCCESS;
}
Tango3DR_Status Tango3DR_ReconstructionContext_setColorCalibration(const Tango3DR_ReconstructionContext c,
    const Tango3DR_CameraCalibration* cal) {
    if (!c || !recon::calibration(cal)) return TANGO_3DR_INVALID;
    c->color = *cal; c->has_color = true; return TANGO_3DR_SUCCESS;
}
Tango3DR_Status Tango3DR_ReconstructionContext_setDepthCalibration(const Tango3DR_ReconstructionContext c,
    const Tango3DR_CameraCalibration* cal) {
    if (!c || !recon::calibration(cal)) return TANGO_3DR_INVALID;
    c->depth = *cal; c->has_depth = true; return TANGO_3DR_SUCCESS;
}
} // extern C
namespace recon {
// Copy only map nodes and touched chunks. Commit is a no-throw swap AFTER
// every allocation (including the returned dirty list) succeeds.
struct Transaction {
    _Tango3DR_ReconstructionContext& context;
    Volume next;
    PagedVolume paged_next;
    Set<Index> changed, dirty;
    size_t work;
    struct CachedChunk {
        Index key;
        std::shared_ptr<Chunk>* slot;
        std::shared_ptr<PagedRecord>* paged_slot;
        bool valid, writable;
        unsigned normal_dirty;
        CachedChunk() : slot(nullptr), paged_slot(nullptr), valid(false), writable(false), normal_dirty(0) {}
    };
    // Bounded, transaction-local, direct-mapped cache. Map insertions preserve
    // pointers to mapped shared_ptrs. A slot is writable only AFTER its chunk
    // has been cloned and dirty-index allocations have all succeeded.
    CachedChunk cache[64];
    PagePin current_pin; // destroyed before paged_next records on rollback
    explicit Transaction(_Tango3DR_ReconstructionContext& c)
        : context(c), next(c.volume), paged_next(c.paged_volume), work(0) {}
    size_t size() const { return context.pager ? paged_next.size() : next.size(); }
    void tick() {
        if (++work > context.config.values[MaxWork])
            throw Limit(WorkLimit,work,size_t(context.config.values[MaxWork]),work,changed.size(),size());
    }
    CachedChunk& cached(const Index& key) {
        uint32_t hash = uint32_t(key.x)*73856093u ^ uint32_t(key.y)*19349663u ^ uint32_t(key.z)*83492791u;
        CachedChunk& entry = cache[hash&63];
        if (!entry.valid || entry.key.x != key.x || entry.key.y != key.y || entry.key.z != key.z) {
            entry.key = key;
            entry.normal_dirty = 0;
            if (context.pager) {
                auto found = paged_next.find(key);
                entry.paged_slot = found == paged_next.end() ? nullptr : &found->second;
                entry.writable = entry.paged_slot && changed.find(key) != changed.end();
            } else {
                auto found = next.find(key);
                entry.slot = found == next.end() ? nullptr : &found->second;
                entry.writable = entry.slot && changed.find(key) != changed.end();
            }
            entry.valid = true;
        }
        return entry;
    }
    bool hasChunk(const Index& key) {
        CachedChunk& entry = cached(key);
        return context.pager ? entry.paged_slot != nullptr : entry.slot != nullptr;
    }
    const Voxel* read(const Index& p) {
        CachedChunk& entry = cached(chunkOf(p));
        const Chunk* chunk = nullptr;
        if (context.pager) {
            if (entry.paged_slot) {
                if (current_pin.record != entry.paged_slot->get()) current_pin.set(**entry.paged_slot);
                chunk = current_pin.record->data;
            } else current_pin.reset();
        } else chunk = entry.slot ? entry.slot->get() : nullptr;
        return chunk ? &chunk->voxels[offset(p)] : nullptr;
    }
    void normalDirty(const Index& p, CachedChunk& entry) {
        // Existing eight negative neighbors cover geometry and all negative
        // stencil dependencies. Only a write at local coordinate 15 can also
        // affect a positive neighbor's node-zero gradient. Other coordinates
        // need a negative owner only at zero; no blind 27-segment invalidation.
        const int local[3] = {p.x-entry.key.x*16,p.y-entry.key.y*16,p.z-entry.key.z*16};
        for (int axis = 0; axis < 3; ++axis) if (local[axis] == 15) {
            int u = (axis+1)%3, v = (axis+2)%3;
            for (int a = 0; a <= (local[u] == 0); ++a) for (int b = 0; b <= (local[v] == 0); ++b) {
                unsigned bit = 1u<<(4*axis+2*a+b);
                if (entry.normal_dirty & bit) continue;
                int key[3] = {entry.key.x,entry.key.y,entry.key.z};
                ++key[axis]; key[u] -= a; key[v] -= b;
                dirty.insert(Index(key[0],key[1],key[2]));
                entry.normal_dirty |= bit;
            }
        }
    }
    Voxel* writable(const Index& p, bool create) {
        Index key = chunkOf(p);
        CachedChunk& entry = cached(key);
        if (context.pager) {
            if (!entry.paged_slot && !create) { current_pin.reset(); return nullptr; }
            if (!entry.writable) {
                current_pin.reset();
                if (changed.size() >= context.config.values[MaxUpdateChunks])
                    throw Limit(FrameLimit,changed.size()+1,size_t(context.config.values[MaxUpdateChunks]),work,changed.size(),size());
                if (!entry.paged_slot && size() >= context.pager->logical_limit)
                    throw Limit(VolumeLimit,size()+1,context.pager->logical_limit,work,changed.size(),size());
                if (entry.paged_slot) current_pin.set(**entry.paged_slot);
                auto copy = context.pager->create(current_pin.record ? current_pin.record->data : nullptr);
                // Release the old pin before replacing its owning map value.
                current_pin.reset();
                if (!entry.paged_slot) entry.paged_slot = &paged_next.emplace(key,copy).first->second;
                else *entry.paged_slot = copy;
                changed.insert(key);
                for (int z = -1; z <= 0; ++z) for (int y = -1; y <= 0; ++y) for (int x = -1; x <= 0; ++x)
                    dirty.insert(Index(key.x+x,key.y+y,key.z+z));
                entry.writable = true;
            }
            // Interior writes have no positive stencil owner. Unsigned low bits
            // are floor-modulo 16 even for negative world coordinates.
            if ((uint32_t(p.x)&15u) == 15 || (uint32_t(p.y)&15u) == 15 || (uint32_t(p.z)&15u) == 15)
                normalDirty(p,entry);
            if (current_pin.record != entry.paged_slot->get()) current_pin.set(**entry.paged_slot);
            context.pager->modified(*current_pin.record);
            return &current_pin.record->data->voxels[offset(p)];
        }
        if (!entry.slot && !create) return nullptr;
        if (!entry.writable) {
            if (changed.size() >= context.config.values[MaxUpdateChunks])
                throw Limit(FrameLimit,changed.size()+1,size_t(context.config.values[MaxUpdateChunks]),work,changed.size(),next.size());
            if (!entry.slot && next.size() >= context.config.values[MaxChunks])
                throw Limit(VolumeLimit,next.size()+1,size_t(context.config.values[MaxChunks]),work,changed.size(),next.size());
            std::shared_ptr<Chunk> copy = !entry.slot ?
                std::allocate_shared<Chunk>(Allocator<Chunk>()) :
                std::allocate_shared<Chunk>(Allocator<Chunk>(), **entry.slot);
            context.ram_peak = std::max(context.ram_peak,uint64_t(context.volume.size()+changed.size()+1));
            if (!entry.slot) entry.slot = &next.emplace(key, copy).first->second;
            else *entry.slot = copy;
            changed.insert(key);
            // A node can affect cells owned by preceding chunks. Conservative
            // whole-chunk halo also covers surface deletions at boundaries.
            for (int z = -1; z <= 0; ++z) for (int y = -1; y <= 0; ++y) for (int x = -1; x <= 0; ++x)
                dirty.insert(Index(key.x+x, key.y+y, key.z+z));
            entry.writable = true;
        }
        if ((uint32_t(p.x)&15u) == 15 || (uint32_t(p.y)&15u) == 15 || (uint32_t(p.z)&15u) == 15)
            normalDirty(p,entry);
        return &(*entry.slot)->voxels[offset(p)];
    }
};
bool hasChunkSupport(Transaction& tx, const Index& base) {
    const Index key = chunkOf(base);
    // Only the final lattice interval in an axis reads the next chunk.
    const int nx = base.x-key.x*16 == 15 ? 1 : 0;
    const int ny = base.y-key.y*16 == 15 ? 1 : 0;
    const int nz = base.z-key.z*16 == 15 ? 1 : 0;
    for (int z = 0; z <= nz; ++z) for (int y = 0; y <= ny; ++y) for (int x = 0; x <= nx; ++x)
        if (tx.hasChunk(Index(key.x+x,key.y+y,key.z+z))) return true;
    return false;
}
double nextChunkSupportBoundary(const glm::dvec3& q, const Index& base,
                                const glm::dvec3& ray, double h) {
    Index key = chunkOf(base);
    int coords[3] = {key.x,key.y,key.z};
    double distance = std::numeric_limits<double>::infinity();
    for (int j = 0; j < 3; ++j) {
        if (std::abs(ray[j]) < 1e-12) continue;
        // A positive ray encounters the neighbor's first node one interval
        // before its chunk boundary. Negative rays encounter the preceding
        // chunk immediately after crossing the current chunk's lower bound.
        double edge = coords[j]*16;
        if (ray[j] > 0) {
            edge += 15;
            if (q[j] >= edge) edge += 1;
        }
        distance = std::min(distance,std::max(0.0,(edge-q[j])*h/ray[j]));
    }
    return distance;
}
void integrateDirect(Transaction& tx, const float* point, const Pose& camera,
               const Tango3DR_ImageBuffer* image, const Pose& color_pose) {
    const double* cfg = tx.context.config.values;
    double h = cfg[Resolution], mu = 3*h;
    glm::dvec3 p(point[0], point[1], point[2]);
    glm::dvec3 surface = camera.world(p);
    double range = glm::length(p);
    glm::dvec3 ray = camera.q * (p / range);
    // Truncation is in ray metres. Axial signed distances avoid spherical bias
    // on fronto-parallel planes. Splat support is one lattice interval.
    // Use one metric normalizer for all rays; ray-dependent normalization
    // biases zero-crossing interpolation when adjacent nodes see different rays.
    double axial_mu = mu;
    glm::dvec3 rgb;
    bool has_color = image && cfg[GenerateColor] && sample(*image, tx.context.color,
        color_pose, surface, cfg[Rectify] != 0, rgb);
    double start = cfg[Clearing] ? 0 : std::max(0.0, range-mu);
    double end = range+mu;
    const double step = h*.5;
    for (double t = start; t <= end; t += step) {
        tx.tick();
        glm::dvec3 q = (camera.t + ray*t)/h;
        Index base(int(std::floor(q.x)), int(std::floor(q.y)), int(std::floor(q.z)));
        bool in_band = t >= range-mu;
        if (!in_band && !hasChunkSupport(tx,base)) {
            // Sparse free-space traversal: skip empty chunks, but stop before
            // a chunk's interpolation halo or the observed truncation band.
            // Keep the half-voxel sampling phase of the unskipped traversal.
            double distance = std::min(range-mu-t,nextChunkSupportBoundary(q,base,ray,h));
            double steps = std::max(1.0,std::floor(distance/step));
            t += (steps-1)*step;
            continue;
        }
        for (int z = 0; z <= 1; ++z) for (int y = 0; y <= 1; ++y) for (int x = 0; x <= 1; ++x) {
            tx.tick();
            Index n(base.x+x, base.y+y, base.z+z);
            double weight = point[3] * (x ? q.x-base.x : 1-(q.x-base.x)) *
                (y ? q.y-base.y : 1-(q.y-base.y)) * (z ? q.z-base.z : 1-(q.z-base.z));
            if (weight < 1e-6) continue;
            if (!in_band) {
                const Voxel* old = tx.read(n);
                if (!old || !old->weight) continue; // never allocate free space
            }
            double sdf = (p.z - camera.local(position(n)*h).z)/axial_mu;
            if (sdf < -1) continue;
            Voxel* v = tx.writable(n, in_band);
            if (!v) continue;
            sdf = std::min(1.0, sdf);
            double total = v->weight + weight;
            v->sdf = float((v->sdf*v->weight + sdf*weight)/total);
            v->weight = float(std::min(cfg[MaxWeight], total));
            if (in_band && has_color) {
                total = v->color_weight + weight;
                for (int j = 0; j < 3; ++j) v->color[j] = float((v->color[j]*v->color_weight + rgb[j]*weight)/total);
                v->color_weight = float(std::min(cfg[MaxWeight], total));
            }
        }
    }
}
} // namespace recon
#include "ray_fusion.h"
extern "C" Tango3DR_Status Tango3DR_updateFromPointCloud(Tango3DR_ReconstructionContext c,
    const Tango3DR_PointCloud* cloud, const Tango3DR_Pose* cloud_pose,
    const Tango3DR_ImageBuffer* image, const Tango3DR_Pose* image_pose,
    Tango3DR_GridIndexArray* indices) {
    if (!indices) return TANGO_3DR_INVALID;
    *indices = Tango3DR_GridIndexArray();
    if (c) c->last_failure = SCANNER_RECONSTRUCTION_FAILURE_NONE;
    if (c && c->pager && c->pager->broken()) {
        c->last_failure = SCANNER_RECONSTRUCTION_FAILURE_CACHE_READ;
        return TANGO_3DR_ERROR;
    }
    recon::Pose camera, color_camera;
    if (!c || !cloud || (cloud->num_points && !cloud->points) || !std::isfinite(cloud->timestamp) ||
        !recon::pose(cloud_pose, camera)) return TANGO_3DR_INVALID;
    if (cloud->num_points > recon::kPointLimit) {
        size_t committed = c->pager ? c->paged_volume.size() : c->volume.size();
        c->last_failure = SCANNER_RECONSTRUCTION_FAILURE_POINT_LIMIT;
        recon::reportLimit(recon::Limit(recon::PointLimit,cloud->num_points,recon::kPointLimit,0,0,committed),
                           committed,cloud->num_points);
        return TANGO_3DR_INSUFFICIENT_SPACE;
    }
    if (image) {
        if (!recon::pose(image_pose, color_camera)) return TANGO_3DR_INVALID;
        if (!c->has_color) return TANGO_3DR_ERROR;
        if (!recon::imageValid(image, c->color)) return TANGO_3DR_INVALID;
    }
    const double* cfg = c->config.values;
    double h = cfg[recon::Resolution];
    // Validate the whole frame before staging. Invalid confidences/NaNs reject
    // the frame; finite out-of-range depths are filtered rather than fused.
    for (int j = 0; j < 3; ++j)
        if (std::abs(camera.t[j])/h > recon::kCoordinateLimit-1024) return TANGO_3DR_INVALID;
    for (uint32_t i = 0; i < cloud->num_points; ++i) {
        const float* p = cloud->points[i];
        for (int j = 0; j < 4; ++j) if (!std::isfinite(p[j])) return TANGO_3DR_INVALID;
        if (p[3] < 0 || p[3] > 1) return TANGO_3DR_INVALID;
        if (p[2] <= 0 || p[2] < cfg[recon::MinDepth] || p[2] > cfg[recon::MaxDepth] ||
            p[3] <= cfg[recon::MinConfidence]) continue;
        glm::dvec3 w = camera.world(glm::dvec3(p[0],p[1],p[2]));
        for (int j = 0; j < 3; ++j)
            if (!std::isfinite(w[j]) || std::abs(w[j])/h > recon::kCoordinateLimit-1024) return TANGO_3DR_INVALID;
        if (glm::length(glm::dvec3(p[0],p[1],p[2])) > 200) return TANGO_3DR_INVALID;
    }
    try {
        recon::Transaction tx(*c);
        for (uint32_t i = 0; i < cloud->num_points; ++i) {
            tx.tick();
            const float* p = cloud->points[i];
            if (p[2] <= 0 || p[2] < cfg[recon::MinDepth] || p[2] > cfg[recon::MaxDepth] ||
                p[3] <= cfg[recon::MinConfidence]) continue;
            recon::integrate(tx, p, camera, image, color_camera);
        }
        Tango3DR_GridIndex* out = recon::buffer<Tango3DR_GridIndex>(tx.dirty.size());
        size_t n = 0;
        for (const recon::Index& k : tx.dirty) {
            out[n][0] = k.x; out[n][1] = k.y; out[n][2] = k.z; ++n;
        }
        if (c->pager) c->paged_volume.swap(tx.paged_next);
        else c->volume.swap(tx.next);
        c->timestamp = cloud->timestamp;
        indices->indices = out; indices->num_indices = uint32_t(n);
        return TANGO_3DR_SUCCESS;
    } catch (const recon::Limit& limit) {
        c->last_failure = limit.kind == recon::WorkLimit ? SCANNER_RECONSTRUCTION_FAILURE_WORK_LIMIT :
            limit.kind == recon::FrameLimit ? SCANNER_RECONSTRUCTION_FAILURE_FRAME_LIMIT : SCANNER_RECONSTRUCTION_FAILURE_VOLUME_LIMIT;
        recon::reportLimit(limit,c->pager ? c->paged_volume.size() : c->volume.size(),cloud->num_points);
        return TANGO_3DR_INSUFFICIENT_SPACE;
    } catch (const recon::PagingFailure& e) { c->last_failure = e.reason; return e.status; }
      catch (const std::bad_alloc&) { c->last_failure = SCANNER_RECONSTRUCTION_FAILURE_MEMORY_LIMIT; return TANGO_3DR_ERROR; }
      catch (...) { c->last_failure = SCANNER_RECONSTRUCTION_FAILURE_INTERNAL; return TANGO_3DR_ERROR; }
}
namespace recon {
#include "field_normals.h"
struct Vertex { glm::dvec3 p, normal, color; bool boundary, field_normal; uint32_t support; };
typedef std::array<uint32_t, 3> Face;
typedef std::pair<Index, Index> Edge;
struct Extractor {
    const _Tango3DR_ReconstructionContext& context;
    Index segment;
    Vector<Vertex> vertices;
    Vector<Face> faces;
    const FieldNormals* shading = nullptr; // only alive during chunk(), never retained by vertices
#ifdef RECONSTRUCTION_LEGACY_MESH
    Map<Edge, uint32_t> edges;
#else
    // Seven Freudenthal edge directions plus exact-zero nodes, addressed in the
    // 17^3 owned-cell halo. Lazy, extraction-local, and bounded independently of
    // surface complexity. Avoid tree nodes and repeated logarithmic searches.
    Vector<uint32_t> edge_ids;
#endif
    Extractor(const _Tango3DR_ReconstructionContext& c, const Index& key) : context(c), segment(key) {}
    uint32_t vertex(Index a, Index b, const Voxel* va, const Voxel* vb) {
        if (b < a) { std::swap(a,b); std::swap(va,vb); }
        double t = clamp(double(va->sdf)/(double(va->sdf)-vb->sdf),0,1);
        Edge key(a,b);
        // Weld exact-zero nodes shared by multiple incident edges.
        if (t == 0) key.second = a;
        if (t == 1) key.first = b;
#ifdef RECONSTRUCTION_LEGACY_MESH
        auto it = edges.find(key);
        if (it != edges.end()) return it->second;
#else
        if (edge_ids.empty()) edge_ids.resize(17*17*17*8,UINT32_MAX);
        int x = key.first.x-segment.x*16, y = key.first.y-segment.y*16, z = key.first.z-segment.z*16;
        int dx = key.second.x-key.first.x, dy = key.second.y-key.first.y, dz = key.second.z-key.first.z;
        assert(x >= 0 && x <= 16 && y >= 0 && y <= 16 && z >= 0 && z <= 16);
        assert(dx >= 0 && dx <= 1 && dy >= 0 && dy <= 1 && dz >= 0 && dz <= 1);
        uint32_t& cached_id = edge_ids[8*(x+17*y+289*z)+dx+2*dy+4*dz];
        if (cached_id != UINT32_MAX) return cached_id;
#endif
        Vertex v;
        v.p = ((1-t)*position(a)+t*position(b))*context.config.values[Resolution];
        // Test degeneracy and accumulate normals in the actual public float
        // geometry, not a higher-precision triangle that collapses on output.
        v.p = glm::dvec3(glm::vec3(v.p));
        v.normal = glm::dvec3(0);
        v.field_normal = shading && shading->edge(a,b,t,v.normal);
        v.support = 1;
        // Preserve topological seam membership before float rounding. A
        // decimal world boundary (e.g. 0.64 m) is not necessarily an exact float.
        v.boundary = false;
        const int low[3] = {segment.x*16,segment.y*16,segment.z*16};
        const int av[3] = {a.x,a.y,a.z}, bv[3] = {b.x,b.y,b.z};
        for (int j = 0; j < 3; ++j) {
            if ((av[j] == low[j] || av[j] == low[j]+16) && (av[j] == bv[j] || t == 0)) v.boundary = true;
            if ((bv[j] == low[j] || bv[j] == low[j]+16) && t == 1) v.boundary = true;
        }
        double wa = (1-t)*va->color_weight, wb = t*vb->color_weight;
        for (int j = 0; j < 3; ++j) v.color[j] = wa+wb > 0 ?
            (wa*va->color[j]+wb*vb->color[j])/(wa+wb) : 255;
        uint32_t id = uint32_t(vertices.size());
        vertices.push_back(v);
#ifdef RECONSTRUCTION_LEGACY_MESH
        edges.emplace(key,id);
#else
        cached_id = id;
#endif
        return id;
    }
    void triangle(uint32_t a, uint32_t b, uint32_t c, const glm::dvec3& outward) {
        if (a == b || a == c || b == c) return;
        glm::dvec3 normal = glm::cross(vertices[b].p-vertices[a].p,vertices[c].p-vertices[a].p);
        if (glm::dot(normal,normal) < 1e-24) return;
        if (glm::dot(normal,outward) < 0) { std::swap(b,c); normal = -normal; }
        if (!vertices[a].field_normal) vertices[a].normal += normal;
        if (!vertices[b].field_normal) vertices[b].normal += normal;
        if (!vertices[c].field_normal) vertices[c].normal += normal;
        if (context.config.values[Clockwise]) std::swap(b,c);
        faces.push_back(Face{{a,b,c}});
    }
    void tetra(const Index* p, const Voxel* const* v, const int* ids) {
        int in[4], out[4], ni = 0, no = 0;
        glm::dvec3 positive(0), negative(0);
        for (int j = 0; j < 4; ++j) {
            int k = ids[j];
            if (v[k]->sdf < 0) { in[ni++] = k; negative += position(p[k]); }
            else { out[no++] = k; positive += position(p[k]); }
        }
        if (!ni || !no) return;
        glm::dvec3 direction = positive/double(no)-negative/double(ni);
        auto edge = [&](int a, int b) { return vertex(p[a],p[b],v[a],v[b]); };
        if (ni == 1) triangle(edge(in[0],out[0]),edge(in[0],out[1]),edge(in[0],out[2]),direction);
        else if (no == 1) triangle(edge(out[0],in[0]),edge(out[0],in[1]),edge(out[0],in[2]),direction);
        else {
            uint32_t a = edge(in[0],out[0]), b = edge(in[0],out[1]);
            uint32_t c = edge(in[1],out[1]), d = edge(in[1],out[0]);
            triangle(a,b,c,direction); triangle(a,c,d,direction);
        }
    }
#include "meshing.h"
    void chunk(const Index& key) {
        if (context.pager ? context.paged_volume.find(key) == context.paged_volume.end() :
            context.volume.find(key) == context.volume.end()) return;
        FieldNormals field(context,key);
        shading = &field;
        // Consistent Freudenthal subdivision on every cell, including seams.
        static const int tets[6][4] = {{0,1,3,7},{0,3,2,7},{0,2,6,7},
                                      {0,6,4,7},{0,4,5,7},{0,5,1,7}};
        // Every owned cell's minimum corner is in this chunk. An absent chunk
        // cannot own an observed cell, even when its positive neighbors exist.
        PagePin pins[8]; // live until ALL halo pointers and tetrahedra are done
        const Chunk* neighbors[8] = {};
        if (context.pager) {
            auto origin = context.paged_volume.find(key);
            if (origin == context.paged_volume.end()) return;
            pins[0].set(*origin->second); neighbors[0] = pins[0].record->data;
            for (int j = 1; j < 8; ++j) {
                auto found = context.paged_volume.find(Index(key.x+(j&1),key.y+((j>>1)&1),key.z+(j>>2)));
                if (found != context.paged_volume.end()) {
                    pins[j].set(*found->second); neighbors[j] = pins[j].record->data;
                }
            }
        } else {
            auto origin = context.volume.find(key);
            if (origin == context.volume.end()) return;
            neighbors[0] = origin->second.get();
            for (int j = 1; j < 8; ++j) {
                auto found = context.volume.find(Index(key.x+(j&1),key.y+((j>>1)&1),key.z+(j>>2)));
                neighbors[j] = found == context.volume.end() ? nullptr : found->second.get();
            }
        }
        // Build the read-only positive halo once: eight map lookups per segment
        // instead of up to 32768. Fixed stack storage adds no allocation/failure
        // point, and pointer identity always refers to the unchanged volume.
        const Voxel* halo[17*17*17];
        for (int z = 0; z <= 16; ++z) for (int y = 0; y <= 16; ++y) for (int x = 0; x <= 16; ++x) {
            const Chunk* chunk = neighbors[(x>>4)+2*(y>>4)+4*(z>>4)];
            const Voxel* voxel = chunk ? &chunk->voxels[(x&15)+16*(y&15)+256*(z&15)] : nullptr;
            if (voxel && voxel->weight < context.config.values[MinSurfaceWeight]) voxel = nullptr;
            halo[x+17*y+289*z] = voxel;
        }
        static const int corners[8] = {0,1,17,18,289,290,306,307};
        for (int z = 0; z < 16; ++z) for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x) {
            const Voxel* v[8]; bool valid = true;
            unsigned negative = 0;
            const Voxel* const* cell = halo+x+17*y+289*z;
            // Every tetrahedron shares the cell's 0--7 diagonal. Without
            // either endpoint no full or partial surface can be emitted.
            if (!cell[corners[0]] || !cell[corners[7]]) continue;
            for (int j = 0; j < 8; ++j) {
                v[j] = cell[corners[j]];
                if (!v[j]) { valid = false; continue; }
                if (v[j]->sdf < 0) negative |= 1u<<j;
            }
            if (negative == 0 || negative == 255) continue; // no tetrahedron crosses zero
            Index p[8];
            for (int j = 0; j < 8; ++j)
                p[j] = Index(key.x*16+x+(j&1),key.y*16+y+((j>>1)&1),key.z*16+z+(j>>2));
            if (!valid) {
                // Only use a tetrahedron when all four of its nodes are observed.
                // Unknown corners elsewhere in the cube do not invalidate it.
                for (const auto& t : tets)
                    if (v[t[0]] && v[t[1]] && v[t[2]] && v[t[3]]) tetra(p,v,t);
                continue;
            }
#ifdef RECONSTRUCTION_LEGACY_MESH
            for (const auto& t : tets) tetra(p,v,t);
#else
            reducedCell(p,v);
#endif
        }
        shading = nullptr;
    }
    uint32_t root(Vector<uint32_t>& parents, uint32_t a) {
        while (parents[a] != a) { parents[a] = parents[parents[a]]; a = parents[a]; }
        return a;
    }
    void filter() {
        // Preserve seam-touching components whose full size is unknown here.
        Vector<uint32_t> parents(vertices.size()), counts(vertices.size(),0);
        Vector<uint8_t> boundary(vertices.size(),0);
        for (uint32_t i = 0; i < vertices.size(); ++i) parents[i] = i;
        for (const Face& f : faces) {
            parents[root(parents,f[1])] = root(parents,f[0]);
            parents[root(parents,f[2])] = root(parents,f[0]);
        }
        for (uint32_t i = 0; i < vertices.size(); ++i) {
            uint32_t r = root(parents,i); counts[r] += vertices[i].support;
            if (vertices[i].boundary) boundary[r] = 1;
        }
        size_t n = 0;
        for (const Face& f : faces) {
            uint32_t r = root(parents,f[0]);
            if (counts[r] >= context.config.values[MinVertices] || boundary[r]) faces[n++] = f;
        }
        faces.resize(n);
        Vector<uint32_t> remap(vertices.size(),UINT32_MAX);
        for (const Face& f : faces) for (uint32_t i : f) remap[i] = 0;
        n = 0;
        for (size_t i = 0; i < vertices.size(); ++i)
            if (remap[i] != UINT32_MAX) { remap[i] = uint32_t(n); vertices[n++] = vertices[i]; }
        vertices.resize(n);
        for (Face& f : faces) for (uint32_t& i : f) i = remap[i];
    }
};
} // namespace recon
extern "C" Tango3DR_Status Tango3DR_extractMeshSegment(const Tango3DR_ReconstructionContext c,
    const Tango3DR_GridIndex index, Tango3DR_Mesh* mesh) {
    if (!mesh) return TANGO_3DR_INVALID;
    *mesh = Tango3DR_Mesh();
    if (!c || !index) return TANGO_3DR_INVALID;
    c->last_failure = SCANNER_RECONSTRUCTION_FAILURE_NONE;
    if (c->pager && c->pager->broken()) {
        c->last_failure = SCANNER_RECONSTRUCTION_FAILURE_CACHE_READ;
        return TANGO_3DR_ERROR;
    }
    for (int j = 0; j < 3; ++j)
        if (index[j] < -recon::kCoordinateLimit/16 || index[j] > recon::kCoordinateLimit/16) return TANGO_3DR_INVALID;
    try {
        recon::Index key(index[0],index[1],index[2]);
        recon::Extractor ex(*c,key);
        ex.chunk(key); ex.filter();
        Tango3DR_Status status = Tango3DR_Mesh_init(uint32_t(ex.vertices.size()),uint32_t(ex.faces.size()),
            true,c->config.values[recon::GenerateColor] != 0,false,false,0,0,0,mesh);
        if (status != TANGO_3DR_SUCCESS) {
            c->last_failure = SCANNER_RECONSTRUCTION_FAILURE_MEMORY_LIMIT;
            return status;
        }
        for (size_t i = 0; i < ex.vertices.size(); ++i) {
            const recon::Vertex& v = ex.vertices[i];
            double len = glm::length(v.normal);
            for (int j = 0; j < 3; ++j) {
                mesh->vertices[i][j] = float(v.p[j]);
                mesh->normals[i][j] = len > 0 ? float(v.normal[j]/len) : 0;
                if (mesh->colors) mesh->colors[i][j] = uint8_t(recon::clamp(std::round(v.color[j]),0,255));
            }
            if (mesh->colors) mesh->colors[i][3] = 255;
        }
        for (size_t i = 0; i < ex.faces.size(); ++i)
            for (int j = 0; j < 3; ++j) mesh->faces[i][j] = ex.faces[i][j];
        mesh->num_vertices = uint32_t(ex.vertices.size()); mesh->num_faces = uint32_t(ex.faces.size());
        mesh->timestamp = c->timestamp;
        return TANGO_3DR_SUCCESS;
    } catch (const recon::PagingFailure& e) {
        Tango3DR_Mesh_destroy(mesh); c->last_failure = e.reason; return e.status;
    } catch (const std::bad_alloc&) {
        Tango3DR_Mesh_destroy(mesh); c->last_failure = SCANNER_RECONSTRUCTION_FAILURE_MEMORY_LIMIT; return TANGO_3DR_ERROR;
    } catch (...) {
        Tango3DR_Mesh_destroy(mesh); c->last_failure = SCANNER_RECONSTRUCTION_FAILURE_INTERNAL; return TANGO_3DR_ERROR;
    }
}
