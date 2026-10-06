// Independent analytic quality/performance fixtures through the public core ABI.
#include <tango_3d_reconstruction_api.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "recorded_geometry.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using V = glm::dvec3;
using Key = std::array<int32_t, 3>;
using Point = std::array<float, 4>;
using Clock = std::chrono::steady_clock;
static void need(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
static double elapsed(Clock::time_point start) { return std::chrono::duration<double,std::milli>(Clock::now()-start).count(); }
static double clamp(double x, double a, double b) { return std::max(a,std::min(b,x)); }
struct SolidBox { V low, high; };
struct Scene {
    std::string name;
    bool plane = false, sphere = false, noisy = false, gap = false;
    V center{0};
    V planeN{0,0,1};
    glm::dquat rotation{1,0,0,0};
    double radius = .35;
    std::vector<SolidBox> boxes;
    double distance(V p) const {
        p=glm::conjugate(rotation)*(p-center)+center;
        if (plane) return std::abs(glm::dot(p-center,planeN));
        if (sphere) return std::abs(glm::length(p-center)-radius);
        double best = 1e100;
        for (const auto& b : boxes) {
            V d = glm::abs(p-(b.low+b.high)*.5)-(b.high-b.low)*.5;
            double signedDistance = glm::length(glm::max(d,V(0))) + std::min(0.,std::max(d.x,std::max(d.y,d.z)));
            best = std::min(best,std::abs(signedDistance));
        }
        return best;
    }
    V normal(V p) const {
        p=glm::conjugate(rotation)*(p-center)+center;
        if (plane) return -planeN;
        if (sphere) return glm::normalize(p-center);
        double best = 1e100; V result(0);
        for (const auto& b : boxes) for (int axis = 0; axis < 3; ++axis) for (int high = 0; high < 2; ++high) {
            V q = p;
            for (int k = 0; k < 3; ++k) q[k] = clamp(q[k],b.low[k],b.high[k]);
            q[axis] = high ? b.high[axis] : b.low[axis];
            double d = glm::length(q-p);
            if (d < best) { best = d; result = V(0); result[axis] = high ? 1 : -1; }
        }
        return rotation*result;
    }
    bool hit(V origin, V ray, double& depth) const {
        origin=glm::conjugate(rotation)*(origin-center)+center;
        ray=glm::conjugate(rotation)*ray;
        depth = 1e100;
        if (plane) {
            double den = glm::dot(planeN,ray);
            if (std::abs(den) < 1e-12) return false;
            depth = glm::dot(planeN,center-origin)/den;
        } else if (sphere) {
            V delta = origin-center;
            double b = glm::dot(delta,ray), c = glm::dot(delta,delta)-radius*radius;
            double discriminant = b*b-c;
            if (discriminant < 0) return false;
            depth = -b-std::sqrt(discriminant);
        } else {
            for (const auto& box : boxes) {
                double near = 0, far = 1e100;
                for (int axis = 0; axis < 3; ++axis) {
                    if (std::abs(ray[axis]) < 1e-12) {
                        if (origin[axis] < box.low[axis] || origin[axis] > box.high[axis]) far = -1;
                    } else {
                        double a = (box.low[axis]-origin[axis])/ray[axis];
                        double b = (box.high[axis]-origin[axis])/ray[axis];
                        if (a>b) std::swap(a,b);
                        near = std::max(near,a); far = std::min(far,b);
                    }
                }
                if (near > 0 && near <= far) depth = std::min(depth,near);
            }
        }
        return depth > .01 && depth < 5;
    }
    std::vector<V> probes() const {
        std::vector<V> result;
        if (plane) {
            for (int y=0;y<25;++y) for (int x=0;x<25;++x) {
                V p(center.x+(x-12)*.025,center.y+(y-12)*.025,center.z);
                p.z -= (planeN.x*(p.x-center.x)+planeN.y*(p.y-center.y))/planeN.z;
                result.push_back(p);
            }
        } else if (sphere) {
            for (int i=0;i<768;++i) {
                double y=1-2*(i+.5)/768, phi=i*2.399963229728653;
                double r=std::sqrt(1-y*y);
                result.push_back(center+radius*V(r*std::cos(phi),y,r*std::sin(phi)));
            }
        } else {
            for (const auto& box : boxes) for (int axis=0;axis<3;++axis) for (int high=0;high<2;++high) {
                int a=(axis+1)%3,b=(axis+2)%3;
                for (int y=0;y<12;++y) for (int x=0;x<12;++x) {
                    V p;
                    p[axis]=high?box.high[axis]:box.low[axis];
                    p[a]=box.low[a]+(box.high[a]-box.low[a])*(x+.5)/12;
                    p[b]=box.low[b]+(box.high[b]-box.low[b])*(y+.5)/12;
                    result.push_back(p);
                }
            }
        }
        for(V& p:result) p=rotation*(p-center)+center;
        return result;
    }
};
struct Camera { V center; glm::dquat rotation; };
static Camera look(V position, V target) {
    V forward=glm::normalize(target-position), up(0,1,0);
    if (std::abs(glm::dot(forward,up))>.95) up=V(0,0,1);
    V right=glm::normalize(glm::cross(up,forward));
    return Camera{position,glm::quat_cast(glm::dmat3(right,glm::cross(forward,right),forward))};
}
static std::vector<Camera> cameras(const Scene& scene) {
    std::vector<Camera> result;
    if (scene.plane) {
        for (int i=0;i<8;++i) result.push_back(look(V(.025*std::sin(i),.025*std::cos(i),0),scene.center));
    } else {
        for (int i=0;i<12;++i) {
            double a=i*6.283185307179586/12;
            result.push_back(look(scene.center+V(1.15*std::sin(a),i%2?.32:-.32,1.15*std::cos(a)),scene.center));
        }
        result.push_back(look(scene.center+V(0,1.2,0),scene.center));
        result.push_back(look(scene.center+V(0,-1.2,0),scene.center));
    }
    return result;
}
static double noise(unsigned i) {
    i^=i>>16; i*=0x7feb352du; i^=i>>15; i*=0x846ca68bu; i^=i>>16;
    return (double(i&65535)/65535.-.5)*2;
}
struct MeshDelete { void operator()(Tango3DR_Mesh* m) const { Tango3DR_Mesh_destroy(m); delete m; } };
using Meshes=std::map<Key,std::unique_ptr<Tango3DR_Mesh,MeshDelete>>;
using PKey=std::array<uint32_t,3>;
static PKey positionKey(const float* p) {
    PKey key; for(int i=0;i<3;++i) { float x=p[i]==0?0:p[i]; std::memcpy(&key[i],&x,4); } return key;
}
static void run(const Scene& scene, double resolution) {
    auto config=Tango3DR_Config_create(TANGO_3DR_CONFIG_RECONSTRUCTION); need(config,"config");
    need(Tango3DR_Config_setDouble(config,"resolution",resolution)==0,"resolution");
    need(Tango3DR_Config_setDouble(config,"min_depth",0)==0,"min depth");
    need(Tango3DR_Config_setDouble(config,"max_depth",5)==0,"max depth");
    need(Tango3DR_Config_setBool(config,"generate_color",false)==0,"color");
    need(Tango3DR_Config_setBool(config,"use_space_clearing",true)==0,"clearing");
    need(Tango3DR_Config_setInt32(config,"min_num_vertices",0)==0,"components");
    auto context=Tango3DR_ReconstructionContext_create(config); Tango3DR_Config_destroy(config); need(context,"context");
    struct Owner { Tango3DR_ReconstructionContext c; ~Owner(){ Tango3DR_ReconstructionContext_destroy(c); } } owner{context};
    Meshes meshes; double updateMs=0,extractMs=0; size_t observations=0,view=0;
    for(const Camera& camera:cameras(scene)) {
        std::vector<Point> points;
        for(int y=0;y<72;++y) for(int x=0;x<96;++x) {
            V local=glm::normalize(V((x-47.5)/90,(y-35.5)/90,1));
            V ray=camera.rotation*local; double distance;
            if(!scene.hit(camera.center,ray,distance)) continue;
            float confidence=1;
            if(scene.noisy) {
                unsigned key=unsigned(x+y*96+view*7001);
                distance+=.005*noise(key);
                if(key%37==0) { distance+=.035*noise(key+17); confidence=.2f; }
            }
            V p=local*distance;
            points.push_back(Point{{float(p.x),float(p.y),float(p.z),confidence}});
        }
        observations+=points.size(); need(!points.empty(),"no observations");
        Tango3DR_PointCloud cloud{double(++view),uint32_t(points.size()),reinterpret_cast<Tango3DR_Vector4*>(points.data())};
        Tango3DR_Pose pose{{camera.center.x,camera.center.y,camera.center.z},
                          {camera.rotation.x,camera.rotation.y,camera.rotation.z,camera.rotation.w}};
        Tango3DR_GridIndexArray dirty{}; auto started=Clock::now();
        need(Tango3DR_updateFromPointCloud(context,&cloud,&pose,nullptr,nullptr,&dirty)==0,"analytic update rejected");
        updateMs+=elapsed(started); started=Clock::now();
        for(uint32_t i=0;i<dirty.num_indices;++i) {
            Key key{{dirty.indices[i][0],dirty.indices[i][1],dirty.indices[i][2]}};
            std::unique_ptr<Tango3DR_Mesh,MeshDelete> mesh(new Tango3DR_Mesh{});
            need(Tango3DR_extractMeshSegment(context,dirty.indices[i],mesh.get())==0,"extract failed");
            if(mesh->num_faces) meshes[key]=std::move(mesh); else meshes.erase(key);
        }
        Tango3DR_GridIndexArray_destroy(&dirty); extractMs+=elapsed(started);
    }
    std::vector<recorded::Triangle> triangles;
    std::map<PKey,uint32_t> ids;
    struct Edge { unsigned count=0; int winding=0; };
    std::map<std::pair<uint32_t,uint32_t>,Edge> edges;
    std::map<PKey,std::vector<V>> boundaryNormals;
    size_t vertices=0,faces=0,degenerate=0,zeroNormals=0,badNormals=0,bridgeFaces=0;
    double totalArea=0,error2=0,maxError=0,normalError=0,shadingError=0,triangleQuality=0,skinnyArea=0;
    for(const auto& item:meshes) {
        const auto& m=*item.second; vertices+=m.num_vertices; faces+=m.num_faces;
        std::vector<uint32_t> welded(m.num_vertices);
        for(uint32_t i=0;i<m.num_vertices;++i) {
            V p(m.vertices[i][0],m.vertices[i][1],m.vertices[i][2]);
            V n(m.normals[i][0],m.normals[i][1],m.normals[i][2]);
            for(int j=0;j<3;++j) need(std::isfinite(p[j])&&std::isfinite(n[j]),"nonfinite output");
            auto key=positionKey(m.vertices[i]);
            welded[i]=ids.emplace(key,uint32_t(ids.size())).first->second;
            double length=glm::length(n); if(length==0) ++zeroNormals;
            if(std::abs(length-1)>1e-4) ++badNormals;
            bool boundary=false;
            for(int j=0;j<3;++j) boundary|=p[j]==float(item.first[j]*16*resolution) || p[j]==float((item.first[j]+1)*16*resolution);
            if(boundary) boundaryNormals[key].push_back(n);
        }
        for(uint32_t i=0;i<m.num_faces;++i) {
            const uint32_t* f=m.faces[i];
            for(int j=0;j<3;++j) need(f[j]<m.num_vertices,"bad index");
            V a(m.vertices[f[0]][0],m.vertices[f[0]][1],m.vertices[f[0]][2]);
            V b(m.vertices[f[1]][0],m.vertices[f[1]][1],m.vertices[f[1]][2]);
            V c(m.vertices[f[2]][0],m.vertices[f[2]][1],m.vertices[f[2]][2]);
            double area=glm::length(glm::cross(b-a,c-a))*.5;
            if(area==0 || f[0]==f[1] || f[1]==f[2] || f[0]==f[2]) ++degenerate;
            totalArea+=area; triangles.push_back(recorded::Triangle{a,b,c});
            const double edgeSum=glm::dot(b-a,b-a)+glm::dot(c-b,c-b)+glm::dot(a-c,a-c);
            const double quality=edgeSum>0 ? 4*std::sqrt(3.)*area/edgeSum : 0;
            triangleQuality+=area*quality;
            if(quality<.05) skinnyArea+=area;
            for(V p:{(4.*a+b+c)/6.,(a+4.*b+c)/6.,(a+b+4.*c)/6.}) {
                double d=scene.distance(p); error2+=area*d*d/3; maxError=std::max(maxError,d);
            }
            if(area>0) {
                V n=glm::normalize(glm::cross(b-a,c-a));
                normalError+=area*std::acos(clamp(glm::dot(n,scene.normal((a+b+c)/3.)),-1,1))*180/3.141592653589793;
                V smooth(0);
                for(int j=0;j<3;++j) smooth+=V(m.normals[f[j]][0],m.normals[f[j]][1],m.normals[f[j]][2]);
                if(glm::length(smooth)>0) smooth=glm::normalize(smooth);
                shadingError+=area*std::acos(clamp(glm::dot(smooth,scene.normal((a+b+c)/3.)),-1,1))*180/3.141592653589793;
            }
            V center=(a+b+c)/3.;
            if(scene.gap && std::abs(center.x)<.035 && std::abs(center.y)<.2 && std::abs(center.z)<.08) ++bridgeFaces;
            for(int j=0;j<3;++j) {
                uint32_t aId=welded[f[j]],bId=welded[f[(j+1)%3]];
                auto& e=edges[std::minmax(aId,bId)]; ++e.count; e.winding+=aId<bId?1:-1;
            }
        }
    }
    need(faces>0 && totalArea>0,"empty analytic surface");
    size_t nonmanifold=0,inconsistent=0,boundary=0,seamPairs=0;
    for(const auto& item:edges) {
        if(item.second.count>2) ++nonmanifold;
        if(item.second.count==2 && item.second.winding) ++inconsistent;
        if(item.second.count==1) ++boundary;
    }
    double seamAngle=0;
    std::vector<double> seamAngles;
    for(const auto& item:boundaryNormals) for(size_t i=0;i<item.second.size();++i) for(size_t j=0;j<i;++j) {
        ++seamPairs;
        double angle=std::acos(clamp(glm::dot(item.second[i],item.second[j]),-1,1))*180/3.141592653589793;
        seamAngles.push_back(angle); seamAngle=std::max(seamAngle,angle);
    }
    std::sort(seamAngles.begin(),seamAngles.end());
    double seamMean=0; for(double angle:seamAngles) seamMean+=angle;
    if(!seamAngles.empty()) seamMean/=seamAngles.size();
    recorded::BVH bvh(triangles); std::vector<double> coverage;
    for(V p:scene.probes()) coverage.push_back(bvh.distance(p));
    std::sort(coverage.begin(),coverage.end());
    size_t covered=0; double coverage2=0;
    for(double d:coverage) { covered+=d<=resolution; coverage2+=d*d; }
    std::cout<<std::setprecision(12)<<"{\"scene\":\""<<scene.name<<"\",\"resolution\":"<<resolution
        <<",\"views\":"<<view<<",\"observations\":"<<observations<<",\"vertices\":"<<vertices<<",\"faces\":"<<faces
        <<",\"payload_bytes\":"<<(vertices*24+faces*12)<<",\"update_ms\":"<<updateMs<<",\"extract_ms\":"<<extractMs
        <<",\"surface_area\":"<<totalArea<<",\"surface_rms_m\":"<<std::sqrt(error2/totalArea)<<",\"surface_max_m\":"<<maxError
        <<",\"triangle_quality_area_mean\":"<<triangleQuality/totalArea<<",\"skinny_area_fraction\":"<<skinnyArea/totalArea
        <<",\"face_normal_mean_deg\":"<<normalError/totalArea<<",\"coverage_rms_m\":"<<std::sqrt(coverage2/coverage.size())
        <<",\"shading_normal_mean_deg\":"<<shadingError/totalArea
        <<",\"coverage_p95_m\":"<<coverage[size_t(.95*(coverage.size()-1))]<<",\"coverage_within_voxel_fraction\":"<<double(covered)/coverage.size()
        <<",\"degenerate_faces\":"<<degenerate<<",\"nonmanifold_edges\":"<<nonmanifold<<",\"winding_errors\":"<<inconsistent
        <<",\"boundary_edges\":"<<boundary<<",\"zero_normals\":"<<zeroNormals<<",\"nonunit_normals\":"<<badNormals
        <<",\"seam_normal_pairs\":"<<seamPairs<<",\"max_seam_normal_angle_deg\":"<<seamAngle
        <<",\"seam_normal_mean_deg\":"<<seamMean<<",\"seam_normal_p95_deg\":"
        <<(seamAngles.empty()?0:seamAngles[size_t(.95*(seamAngles.size()-1))])
        <<",\"gap_bridge_faces\":"<<bridgeFaces<<"}\n";
}
int main(int argc,char** argv) {
    try {
        double resolution=argc>1?std::stod(argv[1]):.02;
        need(resolution>=.01 && resolution<=.04,"resolution outside fixture range");
        recorded::geometrySelfTest();
        std::vector<Scene> scenes;
        Scene plane; plane.name="shifted_plane"; plane.plane=true; plane.center=V(-.13,.07,1.137); scenes.push_back(plane);
        plane.name="oblique_plane"; plane.planeN=glm::normalize(V(.35,-.2,1)); scenes.push_back(plane);
        plane.name="noisy_oblique_plane"; plane.noisy=true; scenes.push_back(plane);
        Scene sphere; sphere.name="sphere"; sphere.sphere=true; sphere.center=V(-.17,.09,.07); scenes.push_back(sphere);
        sphere.name="shifted_sphere"; sphere.center+=V(.005,.003,.007); scenes.push_back(sphere);
        Scene cube; cube.name="cube"; cube.center=V(-.11,.07,.03);
        cube.boxes.push_back(SolidBox{cube.center-V(.3),cube.center+V(.3)}); scenes.push_back(cube);
        cube.name="rotated_cube"; cube.rotation=glm::angleAxis(.41,glm::normalize(V(1,.7,.3))); scenes.push_back(cube);
        Scene plate; plate.name="thin_plate"; plate.boxes.push_back(SolidBox{V(-.35,-.3,-.035),V(.35,.3,.035)}); scenes.push_back(plate);
        Scene gap; gap.name="separated_thin_panels"; gap.gap=true;
        gap.boxes.push_back(SolidBox{V(-.4,-.3,-.04),V(-.075,.3,.04)});
        gap.boxes.push_back(SolidBox{V(.075,-.3,-.04),V(.4,.3,.04)}); scenes.push_back(gap);
        for(const auto& scene:scenes) run(scene,resolution);
    } catch(const std::exception& error) { std::cerr<<"FAIL: "<<error.what()<<"\n"; return 1; }
}
