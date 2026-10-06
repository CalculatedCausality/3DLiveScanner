// Private extraction tests: exact scalar cells and closed analytic volumes.
// Fusion-independent tests deliberately exercise all sign/zero configurations.
#ifndef MESHING_CORE_SOURCE
#define MESHING_CORE_SOURCE "../../reconstruction/core.cc"
#endif
#include MESHING_CORE_SOURCE
#include <random>
#include <functional>
#include <tuple>
#include "recorded_geometry.h"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); std::abort(); } } while (0)
using namespace recon;

struct EdgeUse { int count = 0, winding = 0; };
typedef std::array<float,3> PointKey;
struct Geometry {
    std::map<PointKey,uint32_t> weld;
    std::vector<glm::dvec3> positions;
    std::map<std::pair<uint32_t,uint32_t>,EdgeUse> edges;
    size_t faces = 0, vertices = 0;
    double area = 0, error2 = 0, maxError = 0;
    void append(const Extractor& ex, const std::function<double(glm::dvec3)>& sdf, bool regular = true) {
        std::vector<uint32_t> ids;
        for (const Vertex& v : ex.vertices) {
            CHECK(std::isfinite(glm::length(v.normal)));
            if (regular) CHECK(glm::length(v.normal) > 0);
            PointKey k{{float(v.p.x),float(v.p.y),float(v.p.z)}};
            auto entry = weld.emplace(k,uint32_t(positions.size()));
            if (entry.second) positions.push_back(v.p);
            ids.push_back(entry.first->second);
            for (int j = 0; j < 3; ++j) CHECK(std::isfinite(v.color[j]) && v.color[j] >= -1e-10 && v.color[j] <= 255+1e-10);
        }
        vertices += ex.vertices.size();
        for (const Face& f : ex.faces) {
            glm::dvec3 a = ex.vertices[f[0]].p, b = ex.vertices[f[1]].p, c = ex.vertices[f[2]].p;
            double ar = .5*glm::length(glm::cross(b-a,c-a)); CHECK(ar > 5e-13);
            area += ar;
            // Area-weighted quadrature includes interiors, not only vertices.
            const glm::dvec3 samples[4] = {(a+b+c)/3.0,(4.0*a+b+c)/6.0,(a+4.0*b+c)/6.0,(a+b+4.0*c)/6.0};
            for (glm::dvec3 sample : samples) {
                double error = std::abs(sdf(sample));
                error2 += error*error*ar/4; maxError = std::max(maxError,error);
            }
            ++faces;
            for (int j = 0; j < 3; ++j) {
                uint32_t aId = ids[f[j]], bId = ids[f[(j+1)%3]]; CHECK(aId != bId);
                EdgeUse& use = edges[std::minmax(aId,bId)];
                ++use.count; use.winding += aId < bId ? 1 : -1;
            }
        }
    }
    void topology(bool closed) const {
        for (const auto& e : edges) {
            CHECK(e.second.count <= 2);
            if (closed && e.second.count != 2) {
                glm::dvec3 a = positions[e.first.first], b = positions[e.first.second];
                std::fprintf(stderr,"open edge: %.9g %.9g %.9g -> %.9g %.9g %.9g count=%d\n",a.x,a.y,a.z,b.x,b.y,b.z,e.second.count);
            }
            if (closed) CHECK(e.second.count == 2);
            if (e.second.count == 2) CHECK(e.second.winding == 0);
        }
        if (closed) CHECK(int64_t(positions.size())-int64_t(edges.size())+int64_t(faces) == 2);
    }
};

typedef std::map<std::pair<PointKey,PointKey>,std::pair<int,int>> Boundary;
Boundary boundaryOf(const Geometry& g) {
    Boundary result;
    for (const auto& e : g.edges) if (e.second.count != 2 || e.second.winding) {
        auto a = g.positions[e.first.first], b = g.positions[e.first.second];
        PointKey ka{{float(a.x),float(a.y),float(a.z)}}, kb{{float(b.x),float(b.y),float(b.z)}};
        result[std::minmax(ka,kb)] = std::make_pair(e.second.count,ka < kb ? e.second.winding : -e.second.winding);
    }
    return result;
}
void originalCell(Extractor& ex, const Index* p, const Voxel* const* v) {
    static const int tets[6][4] = {{0,1,3,7},{0,3,2,7},{0,2,6,7},{0,6,4,7},{0,4,5,7},{0,5,1,7}};
    for (const auto& tet : tets) ex.tetra(p,v,tet);
    ex.filter();
}

void cells() {
#ifndef RECONSTRUCTION_LEGACY_MESH
    _Tango3DR_Config cfg(TANGO_3DR_CONFIG_RECONSTRUCTION); cfg.values[Resolution] = 1;
    _Tango3DR_ReconstructionContext context(cfg);
    Index p[8]; Voxel voxels[8]; const Voxel* v[8];
    for (int j = 0; j < 8; ++j) { p[j] = Index(j&1,(j>>1)&1,j>>2); v[j] = &voxels[j]; }
    std::mt19937 random(617);
    size_t total = 0;
    for (int sample = 0; sample < 40; ++sample) for (int signs = 1; signs < 255; ++signs) {
        for (int j = 0; j < 8; ++j)
            voxels[j].sdf = float((.1+(random()%1000)/100.0)*((signs&(1<<j)) ? -1 : 1));
        Extractor ex(context,Index()); ex.reducedCell(p,v); ex.filter();
        Geometry geom; geom.append(ex,[](glm::dvec3){ return 0.0; });
        Extractor legacy(context,Index()); originalCell(legacy,p,v);
        Geometry old; old.append(legacy,[](glm::dvec3){ return 0.0; });
        CHECK(boundaryOf(old) == boundaryOf(geom));
        for (const auto& edge : geom.edges) {
            if (edge.second.count > 2 || (edge.second.count == 2 && edge.second.winding)) {
                std::fprintf(stderr,"cell sample=%d signs=%d count=%d winding=%d\n",sample,signs,edge.second.count,edge.second.winding);
                for (int j = 0; j < 8; ++j) std::fprintf(stderr,"%.9g ",voxels[j].sdf);
                std::fprintf(stderr,"\n");
            }
            CHECK(edge.second.count <= 2);
            if (edge.second.count == 2) CHECK(edge.second.winding == 0);
            else {
                glm::dvec3 a = geom.positions[edge.first.first], b = geom.positions[edge.first.second];
                bool boundary = false;
                for (int j = 0; j < 3; ++j) boundary |= a[j] == b[j] && (a[j] == 0 || a[j] == 1);
                CHECK(boundary);
            }
        }
        ++total;
    }
    std::puts("random-sign cell winding/boundary checks passed"); std::fflush(stdout);
    for (int pattern = 0; pattern < 6561; ++pattern) {
        int code = pattern;
        for (int j = 0; j < 8; ++j) { voxels[j].sdf = float(code%3-1); code /= 3; }
        Extractor ex(context,Index()); ex.reducedCell(p,v); ex.filter();
        Geometry geom; geom.append(ex,[](glm::dvec3){ return 0.0; },false);
        // Some exact-zero fields are intrinsically singular (coincident sheets,
        // zero normal sums). Compare their boundary/incidence to the legacy
        // construction rather than pretending every scalar field is a manifold.
        Extractor legacy(context,Index());
        originalCell(legacy,p,v);
        Geometry old; old.append(legacy,[](glm::dvec3){ return 0.0; },false);
        CHECK(boundaryOf(old) == boundaryOf(geom));
        ++total;
    }
    std::printf("cells: %zu random-sign / exact-zero configurations passed\n",total);
#endif
}

void boundedReduction() {
#ifndef RECONSTRUCTION_LEGACY_MESH
    _Tango3DR_Config cfg(TANGO_3DR_CONFIG_RECONSTRUCTION); cfg.values[Resolution] = 1;
    cfg.values[GenerateColor] = 0; // geometry bound; color-feature gate is tested separately
    _Tango3DR_ReconstructionContext context(cfg);
    std::mt19937 random(4312); size_t reductions = 0; double maximum = 0;
    for (int sample = 0; sample < 2000; ++sample) {
        Index p[8]; Voxel voxels[8]; const Voxel* v[8];
        glm::dvec3 normal((random()%2001)/1000.-1,(random()%2001)/1000.-1,(random()%2001)/1000.-1);
        normal = glm::normalize(normal);
        double d = glm::dot(normal,glm::dvec3(.5))+(random()%201)/1000.-.1;
        for (int j = 0; j < 8; ++j) {
            p[j] = Index(j&1,(j>>1)&1,j>>2); v[j] = &voxels[j];
            voxels[j].sdf = float(glm::dot(position(p[j]),normal)-d+(random()%201)/10000.-.01);
            voxels[j].color_weight = float(random()%4);
            for (int k = 0; k < 3; ++k) voxels[j].color[k] = float(random()%256);
        }
        Extractor ex(context,Index()), old(context,Index());
        ex.reducedCell(p,v); ex.filter(); originalCell(old,p,v);
        Geometry before, after;
        before.append(old,[](glm::dvec3){ return 0.0; }); after.append(ex,[](glm::dvec3){ return 0.0; });
        CHECK(boundaryOf(before) == boundaryOf(after)); after.topology(false);
        CHECK(ex.faces.size() <= old.faces.size());
        if (ex.faces.size() == old.faces.size()) continue;
        ++reductions;
        for (const Vertex& vertex : ex.vertices) {
            bool found = false;
            for (const Vertex& original : old.vertices) if (vertex.p == original.p) {
                CHECK(vertex.color == original.color); found = true;
            }
            CHECK(found); // Reduction never moves or invents a boundary vertex.
        }
        for (int direction = 0; direction < 2; ++direction) {
            const Extractor& a = direction ? old : ex;
            const Extractor& b = direction ? ex : old;
            for (const Face& face : a.faces) for (int u = 0; u <= 5; ++u) for (int w = 0; w <= 5-u; ++w) {
                glm::dvec3 point = (double(u)*a.vertices[face[0]].p+double(w)*a.vertices[face[1]].p+
                    double(5-u-w)*a.vertices[face[2]].p)/5.0;
                double distance2 = std::numeric_limits<double>::infinity();
                for (const Face& tri : b.faces) distance2 = std::min(distance2,recorded::triangleDistance2(point,
                    recorded::Triangle{b.vertices[tri[0]].p,b.vertices[tri[1]].p,b.vertices[tri[2]].p}));
                maximum = std::max(maximum,std::sqrt(distance2)); CHECK(distance2 <= .01000001*.01000001);
            }
        }
    }
    CHECK(reductions > 200);
    std::printf("bounded reductions: %zu / 2000 jittered planes, sampled symmetric deviation %.9f voxel (limit .01)\n",reductions,maximum);
#endif
}

void observedCorners() {
    _Tango3DR_Config cfg(TANGO_3DR_CONFIG_RECONSTRUCTION); cfg.values[Resolution] = 1;
    _Tango3DR_ReconstructionContext context(cfg);
    for (int j = 0; j < 8; ++j) {
        Index p(-1+(j&1),-1+((j>>1)&1),-1+(j>>2));
        auto& chunk = context.volume[chunkOf(p)]; if (!chunk) chunk = std::make_shared<Chunk>();
        Voxel& v = chunk->voxels[offset(p)]; v.sdf = (j&4) ? .7f : -.3f; v.weight = 2;
    }
    for (int missing = 0; missing < 8; ++missing) {
        Index p(-1+(missing&1),-1+((missing>>1)&1),-1+(missing>>2));
        Voxel& v = context.volume[chunkOf(p)]->voxels[offset(p)]; v.weight = .5f;
        Extractor ex(context,Index(-1,-1,-1)); ex.chunk(Index(-1,-1,-1)); ex.filter();
        CHECK(ex.vertices.empty() && ex.faces.empty()); v.weight = 2;
    }
    Extractor ex(context,Index(-1,-1,-1)); ex.chunk(Index(-1,-1,-1)); ex.filter();
    CHECK(!ex.faces.empty());
    std::puts("all-eight observed corners across eight negative/positive chunks: pass");
}

void componentEvidence() {
#ifndef RECONSTRUCTION_LEGACY_MESH
    _Tango3DR_Config cfg(TANGO_3DR_CONFIG_RECONSTRUCTION); cfg.values[Resolution] = 1;
    _Tango3DR_ReconstructionContext context(cfg);
    Index p[8]; Voxel voxels[8]; const Voxel* v[8];
    for (int j = 0; j < 8; ++j) {
        p[j] = Index(2+(j&1),2+((j>>1)&1),2+(j>>2)); v[j] = &voxels[j];
        voxels[j].sdf = (j&4) ? .7f : -.3f;
    }
    for (int threshold = 0; threshold <= 12; ++threshold) {
        context.config.values[MinVertices] = threshold;
        Extractor ex(context,Index()), old(context,Index());
        ex.reducedCell(p,v); ex.filter(); originalCell(old,p,v);
        CHECK(ex.faces.empty() == old.faces.empty());
        if (!old.faces.empty()) CHECK(ex.faces.size()+2 == old.faces.size());
    }
    std::printf("component-filter evidence preserved; private Vertex size %zu bytes\n",sizeof(Vertex));
#endif
}

void roundedSliver() {
#ifndef RECONSTRUCTION_LEGACY_MESH
    // Real 2 cm recorded-cell regression: one candidate fan triangle was valid
    // in normalized coordinates but below the public-float area threshold.
    const Index bases[] = {Index(-124,-88,-173),Index(-124,-87,-174),Index(-124,-87,-173)};
    const float fields[3][8] = {
        {-.0361625664f,-.386357993f,.0000102722252f,-.201332211f,-.100569479f,-.421110123f,-.147738352f,-.232184589f},
        {.204600260f,-.226370439f,-.620308161f,-.531006992f,.0000102722252f,-.201332211f,-.147070080f,-.208641350f},
        {.0000102722252f,-.201332211f,-.147070080f,-.208641350f,-.147738352f,-.232184589f,-.145402044f,-.142506033f}
    };
    _Tango3DR_Config cfg(TANGO_3DR_CONFIG_RECONSTRUCTION); cfg.values[Resolution] = .02;
    _Tango3DR_ReconstructionContext context(cfg);
    for (int sample = 0; sample < 3; ++sample) {
        Index p[8]; Voxel voxels[8]; const Voxel* v[8];
        for (int j = 0; j < 8; ++j) {
            p[j] = Index(bases[sample].x+(j&1),bases[sample].y+((j>>1)&1),bases[sample].z+(j>>2));
            v[j] = &voxels[j]; voxels[j].sdf = fields[sample][j];
        }
        Extractor ex(context,chunkOf(bases[sample])), old(context,chunkOf(bases[sample]));
        ex.reducedCell(p,v); ex.filter(); originalCell(old,p,v);
        Geometry before, after;
        before.append(old,[](glm::dvec3){ return 0.0; }); after.append(ex,[](glm::dvec3){ return 0.0; });
        CHECK(boundaryOf(before) == boundaryOf(after));
    }
    std::puts("recorded negative-coordinate float-sliver boundary regression: pass");
#endif
}

void colorEvidence() {
#ifndef RECONSTRUCTION_LEGACY_MESH
    _Tango3DR_Config cfg(TANGO_3DR_CONFIG_RECONSTRUCTION); cfg.values[Resolution] = 1;
    _Tango3DR_ReconstructionContext context(cfg);
    Index p[8]; Voxel voxels[8]; const Voxel* v[8];
    for (int mode = 0; mode < 3; ++mode) {
        for (int j = 0; j < 8; ++j) {
            p[j] = Index(j&1,(j>>1)&1,j>>2); v[j] = &voxels[j];
            voxels[j].sdf = (j&4) ? .75f : -.25f;
            voxels[j].color_weight = 1;
            for (int channel = 0; channel < 3; ++channel)
                voxels[j].color[channel] = mode == 0 ? 80 : mode == 1 ? float(32+64*((j>>channel)&1)) : (j == 0 ? 255.f : 0.f);
        }
        Extractor ex(context,Index()), old(context,Index());
        ex.reducedCell(p,v); ex.filter(); originalCell(old,p,v);
        if (mode < 2) CHECK(ex.faces.size()+2 == old.faces.size());
        else CHECK(ex.faces.size() == old.faces.size()); // retain a color-only feature
    }
    std::puts("constant/affine color reduction and high-contrast color-feature retention: pass");
#endif
}

void analytic(const char* name, double h, const std::function<double(glm::dvec3)>& sdf,
              bool closed, double limit) {
    _Tango3DR_Config cfg(TANGO_3DR_CONFIG_RECONSTRUCTION);
    cfg.values[Resolution] = h; cfg.values[MinVertices] = 0;
    _Tango3DR_ReconstructionContext context(cfg);
    int radius = int(std::ceil(.65/h));
    for (int z = -radius; z <= radius; ++z) for (int y = -radius; y <= radius; ++y) for (int x = -radius; x <= radius; ++x) {
        Index p(x,y,z), key = chunkOf(p);
        auto& chunk = context.volume[key]; if (!chunk) chunk = std::make_shared<Chunk>();
        Voxel& v = chunk->voxels[offset(p)];
        v.sdf = float(sdf(position(p)*h)); v.weight = 2;
        v.color_weight = 2; v.color[0] = 20; v.color[1] = 80; v.color[2] = 180;
    }
    Geometry geom;
    double milliseconds = 0;
    for (int repeat = 0; repeat < 4; ++repeat) {
        auto start = std::chrono::steady_clock::now();
        for (const auto& entry : context.volume) {
            Extractor ex(context,entry.first); ex.chunk(entry.first); ex.filter();
            auto finish = std::chrono::steady_clock::now();
            milliseconds += std::chrono::duration<double,std::milli>(finish-start).count();
            if (!repeat) geom.append(ex,sdf);
            start = std::chrono::steady_clock::now();
        }
    }
    std::printf("%s h=%.2f extract_ms=%.3f vertices=%zu faces=%zu bytes=%zu rms_mm=%.6f max_mm=%.6f\n",
        name,h,milliseconds/4,geom.vertices,geom.faces,geom.vertices*28+geom.faces*12,
        1000*std::sqrt(geom.error2/geom.area),1000*geom.maxError);
    std::fflush(stdout);
    geom.topology(closed); CHECK(geom.faces && geom.maxError < limit);
}

int main(int argc, char**) {
    if (argc == 1) { cells(); boundedReduction(); observedCorners(); componentEvidence(); roundedSliver(); colorEvidence(); }
    for (double h : {.02,.04}) {
        analytic("plane",h,[](glm::dvec3 p) { return p.z-.013; },false,2e-7);
        analytic("rotated-plane",h,[](glm::dvec3 p) { return glm::dot(p,glm::normalize(glm::dvec3(.31,-.23,1)))-.017; },false,2e-7);
        analytic("sphere",h,[](glm::dvec3 p) { return glm::length(p-glm::dvec3(.01))-.45; },true,.002);
        analytic("cube",h,[](glm::dvec3 p) { glm::dvec3 q = glm::abs(p-glm::dvec3(.013,.007,-.011))-glm::dvec3(.41); return std::max(q.x,std::max(q.y,q.z)); },true,.022);
        analytic("thin-box",h,[h](glm::dvec3 p) { glm::dvec3 q = glm::abs(p-glm::dvec3(.013,.007,-.011))-glm::dvec3(.41,.41,1.1*h); return std::max(q.x,std::max(q.y,q.z)); },true,.022);
    }
}
