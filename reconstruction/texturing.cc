// SPDX-License-Identifier: Apache-2.0
// Clean-room CPU compatibility implementation; no proprietary reconstruction code.
#include "tango_3d_reconstruction_api.h"
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include "texture_geometry.h"
#include "data/file3d.h"

namespace scanner_texture {
constexpr size_t kMaxPixels=16u*1024u*1024u; // 64 MiB persistent RGBA atlas.
constexpr uint32_t kMaxFaces=500000, kMaxVertices=1500000;
inline bool validImage(const Tango3DR_ImageBuffer& im) {
    if(!im.data||!im.width||!im.height||im.width>8192||im.height>8192||
       size_t(im.width)*im.height>kMaxPixels||!std::isfinite(im.timestamp)) return false;
    unsigned channels=im.format==TANGO_3DR_HAL_PIXEL_FORMAT_RGB_888?3:
                      im.format==TANGO_3DR_HAL_PIXEL_FORMAT_RGBA_8888?4:1;
    if(im.stride<im.width*channels||im.stride>32768) return false;
    if(channels==1&&(im.format!=TANGO_3DR_HAL_PIXEL_FORMAT_YCrCb_420_SP||
                     (im.width|im.height|im.stride)&1)) return false;
    return true;
}
inline uint8_t byte(double x) { return static_cast<uint8_t>(std::max(0.,std::min(255.,std::round(x)))); }
inline std::array<uint8_t,4> pixel(const Tango3DR_ImageBuffer& im,int x,int y) {
    const uint8_t* row=im.data+size_t(y)*im.stride;
    if(im.format==TANGO_3DR_HAL_PIXEL_FORMAT_RGBA_8888) return {{row[4*x],row[4*x+1],row[4*x+2],row[4*x+3]}};
    if(im.format==TANGO_3DR_HAL_PIXEL_FORMAT_RGB_888) return {{row[3*x],row[3*x+1],row[3*x+2],255}};
    const uint8_t* vu=im.data+size_t(im.stride)*im.height+size_t(y/2)*im.stride+(x&~1);
    // Full-range JPEG YCbCr, as written by Image::JPG2YUV / dataset capture.
    double yy=row[x],v=vu[0]-128.,u=vu[1]-128.;
    return {{byte(yy+1.402*v),byte(yy-.344136*u-.714136*v),byte(yy+1.772*u),255}};
}
inline std::array<uint8_t,4> sample(const Tango3DR_ImageBuffer& im,double u,double v) {
    int x=static_cast<int>(u),y=static_cast<int>(v);
    auto a=pixel(im,x,y),b=pixel(im,std::min(x+1,int(im.width)-1),y);
    auto c=pixel(im,x,std::min(y+1,int(im.height)-1)),d=pixel(im,std::min(x+1,int(im.width)-1),std::min(y+1,int(im.height)-1));
    std::array<uint8_t,4> out; double fx=u-x,fy=v-y;
    for(int i=0;i<4;++i) out[i]=byte((1-fy)*((1-fx)*a[i]+fx*b[i])+fy*((1-fx)*c[i]+fx*d[i]));
    return out;
}
inline bool validMesh(const Tango3DR_Mesh* m) {
    if(!m||!m->num_faces||!m->num_vertices||m->num_faces>kMaxFaces||m->num_vertices>kMaxVertices||
       !m->vertices||!m->faces||m->num_vertices>m->max_num_vertices||m->num_faces>m->max_num_faces||
       m->num_textures>m->max_num_textures||m->num_textures>8||!std::isfinite(m->timestamp)) return false;
    size_t pixels=0;
    if(m->num_textures&&(!m->textures||!m->texture_ids||!m->texture_coords)) return false;
    for(uint32_t i=0;i<m->num_textures;++i) {
        if(!validImage(m->textures[i])) return false;
        pixels+=size_t(m->textures[i].width)*m->textures[i].height; if(pixels>kMaxPixels) return false;
    }
    for(uint32_t i=0;i<m->num_vertices;++i) {
        if(!finite(V(m->vertices[i]))||(m->normals&&!finite(V(m->normals[i])))) return false;
        if(m->texture_coords&&(!std::isfinite(m->texture_coords[i][0])||!std::isfinite(m->texture_coords[i][1]))) return false;
    }
    for(uint32_t i=0;i<m->num_faces;++i) {
        for(int k=0;k<3;++k) if(m->faces[i][k]>=m->num_vertices) return false;
        V a(m->vertices[m->faces[i][0]]),b(m->vertices[m->faces[i][1]]),c(m->vertices[m->faces[i][2]]);
        if(!(length(cross(b-a,c-a))>0)) return false;
        if(m->texture_ids&&(m->texture_ids[i]<-1||m->texture_ids[i]>=int(m->num_textures))) return false;
    }
    return true;
}
struct OwnedMesh {
    Tango3DR_Mesh mesh{};
    ~OwnedMesh() { Tango3DR_Mesh_destroy(&mesh); }
    void release(Tango3DR_Mesh* out) { *out=mesh; mesh=Tango3DR_Mesh{}; }
};
} // namespace scanner_texture

struct _Tango3DR_TexturingContext {
    std::vector<scanner_texture::Triangle> faces;
    scanner_texture::Visibility visibility;
    std::vector<std::vector<uint8_t>> atlas;
    Tango3DR_CameraCalibration calibration{};
    bool calibrated=false, hasNormals=false, hasColors=false;
    int size=2048,limit=4,padding=3,downsample=1;
    uint64_t frames=0;
    double resolution=0,timestamp=0;
};

namespace scanner_texture {
bool pack(_Tango3DR_TexturingContext& c) {
    int page=0,x=0,y=0,row=0;
    for(Triangle& f:c.faces) {
        double need=std::ceil(f.edge/c.resolution);
        if(!std::isfinite(need)||need>c.size) return false;
        int side=std::max(2,int(need))+2*c.padding+1;
        if(side>c.size) return false;
        if(x+side>c.size) { x=0; y+=row; row=0; }
        if(y+side>c.size) { ++page; x=y=row=0; }
        if(page>=c.limit) return false;
        f.page=page; f.x=x; f.y=y; f.side=side;
        x+=side; row=std::max(row,side);
    }
    c.atlas.resize(page+1);
    for(auto& a:c.atlas) a.resize(size_t(c.size)*c.size*4,0);
    return true;
}
bool options(Tango3DR_Config config,_Tango3DR_TexturingContext& c) {
    int32_t backend=0,simplification=1,size=c.size,count=c.limit,downsample=1;
    double bevel=3,resolution=0;
    if(config) {
        // Required public getters deliberately reject a reconstruction config.
        if(Tango3DR_Config_getInt32(config,"texturing_backend",&backend)!=TANGO_3DR_SUCCESS||
           Tango3DR_Config_getInt32(config,"mesh_simplification_factor",&simplification)!=TANGO_3DR_SUCCESS||
           Tango3DR_Config_getInt32(config,"texture_size",&size)!=TANGO_3DR_SUCCESS||
           Tango3DR_Config_getInt32(config,"max_num_textures",&count)!=TANGO_3DR_SUCCESS||
           Tango3DR_Config_getInt32(config,"downsample",&downsample)!=TANGO_3DR_SUCCESS||
           Tango3DR_Config_getDouble(config,"bevel",&bevel)!=TANGO_3DR_SUCCESS||
           Tango3DR_Config_getDouble(config,"min_resolution",&resolution)!=TANGO_3DR_SUCCESS) return false;
    }
    if(backend!=0||simplification!=1||size<16||size>4096||count<0||count>8||downsample<1||
       !std::isfinite(bevel)||bevel<1||bevel>16||!std::isfinite(resolution)||resolution<0) return false;
    // Unlimited is bounded by our documented memory ceiling, never frame count.
    int memoryLimit=std::min(8,int(kMaxPixels/(size_t(size)*size)));
    c.size=size; c.limit=count?count:memoryLimit;
    if(c.limit>memoryLimit) return false;
    c.padding=int(std::ceil(bevel)); c.downsample=downsample; c.resolution=resolution;
    return true;
}
} // namespace scanner_texture

extern "C" Tango3DR_TexturingContext Tango3DR_TexturingContext_create(
        const Tango3DR_Config config,const Tango3DR_Mesh* mesh) {
    try {
        using namespace scanner_texture;
        if(!validMesh(mesh)) return nullptr;
        std::unique_ptr<_Tango3DR_TexturingContext> c(new _Tango3DR_TexturingContext);
        if(!options(config,*c)) return nullptr;
        c->hasNormals=mesh->normals!=nullptr; c->hasColors=mesh->colors!=nullptr; c->timestamp=mesh->timestamp;
        c->faces.resize(mesh->num_faces); double longest=0;
        for(size_t i=0;i<c->faces.size();++i) {
            Triangle& f=c->faces[i];
            for(int k=0;k<3;++k) {
                uint32_t index=mesh->faces[i][k]; f.p[k]=V(mesh->vertices[index]);
                if(c->hasNormals) f.n[k]=V(mesh->normals[index]);
                if(c->hasColors) std::copy(mesh->colors[index],mesh->colors[index]+4,f.color[k].begin());
            }
            // Canonical triangle (0,0),(1,0),(0,1); use longest edge for density bound.
            f.edge=std::max(length(f.p[1]-f.p[0]),std::max(length(f.p[2]-f.p[0]),length(f.p[2]-f.p[1])));
            longest=std::max(longest,f.edge);
        }
        if(c->resolution==0) c->resolution=longest/64.;
        // Skip impossible densities directly, including tiny positive input
        // resolutions, before the bounded shelf-packing search.
        int available=c->size-2*c->padding-1;
        if(available<2) return nullptr;
        c->resolution=std::max(c->resolution,longest/available);
        bool packed=false;
        for(int attempt=0;attempt<128;++attempt) {
            if(pack(*c)) { packed=true; break; }
            c->resolution*=1.25;
        }
        if(!packed) return nullptr;
        c->visibility.init(c->faces);
        return c.release();
    } catch(...) { return nullptr; }
}

extern "C" Tango3DR_Status Tango3DR_TexturingContext_destroy(Tango3DR_TexturingContext context) {
    if(!context) return TANGO_3DR_INVALID;
    try { delete context; return TANGO_3DR_SUCCESS; } catch(...) { return TANGO_3DR_ERROR; }
}

extern "C" Tango3DR_Status Tango3DR_TexturingContext_setColorCalibration(
        const Tango3DR_TexturingContext c,const Tango3DR_CameraCalibration* cal) {
    if(!c||!cal||!cal->width||!cal->height||cal->width>8192||cal->height>8192||
       size_t(cal->width)*cal->height>scanner_texture::kMaxPixels||
       !std::isfinite(cal->fx)||!std::isfinite(cal->fy)||cal->fx<=0||cal->fy<=0||
       !std::isfinite(cal->cx)||!std::isfinite(cal->cy)||cal->cx<0||cal->cy<0||
       cal->cx>=cal->width||cal->cy>=cal->height) return TANGO_3DR_INVALID;
    if(cal->calibration_type!=TANGO_3DR_CALIBRATION_POLYNOMIAL_2_PARAMETERS&&
       cal->calibration_type!=TANGO_3DR_CALIBRATION_POLYNOMIAL_3_PARAMETERS&&
       cal->calibration_type!=TANGO_3DR_CALIBRATION_POLYNOMIAL_5_PARAMETERS) return TANGO_3DR_INVALID;
    for(double d:cal->distortion) if(!std::isfinite(d)) return TANGO_3DR_INVALID;
    c->calibration=*cal; c->calibrated=true; return TANGO_3DR_SUCCESS;
}

extern "C" Tango3DR_Status Tango3DR_updateTexture(Tango3DR_TexturingContext c,
        const Tango3DR_ImageBuffer* image,const Tango3DR_Pose* pose) {
    try {
        using namespace scanner_texture;
        if(!c||!image||!pose||!validImage(*image)) return TANGO_3DR_INVALID;
        if(!c->calibrated) return TANGO_3DR_ERROR;
        if(image->width!=c->calibration.width||image->height!=c->calibration.height) return TANGO_3DR_INVALID;
        Camera camera; if(!camera.init(*pose)) return TANGO_3DR_INVALID;
        if((c->frames++%uint64_t(c->downsample))!=0) return TANGO_3DR_SUCCESS;
        // Allocate all scratch before changing any face; no source image is retained.
        int largest=0; for(const auto& f:c->faces) largest=std::max(largest,f.side);
        std::vector<uint8_t> tile(size_t(largest)*largest*4);
        for(size_t i=0;i<c->faces.size();++i) {
            Triangle& f=c->faces[i]; double u[3],v[3]; bool usable=true;
            for(int k=0;k<3;++k) if(!project(camera.local(f.p[k]),c->calibration,u[k],v[k])||
                !c->visibility.visible(camera.origin,f.p[k],i)) usable=false;
            if(!usable) continue;
            V center=(f.p[0]+f.p[1]+f.p[2])*(1./3.),normal=cross(f.p[1]-f.p[0],f.p[2]-f.p[0]);
            V ray=camera.origin-center;
            double cosine=std::abs(dot(normal,ray))/(length(normal)*length(ray));
            double score=std::abs((u[1]-u[0])*(v[2]-v[0])-(u[2]-u[0])*(v[1]-v[0]))*cosine;
            if(!std::isfinite(score)||score<=f.score||score<0.25||cosine<0.05) continue;
            int span=f.side-2*c->padding-1;
            // Every atlas texel is projected from its 3D barycentric position, not
            // affine screen interpolation. Edge extrusion provides the bevel gutter.
            for(int y=0;y<f.side&&usable;++y) for(int x=0;x<f.side;++x) {
                double b=std::max(0.,std::min(1.,double(x-c->padding)/span));
                double d=std::max(0.,std::min(1.,double(y-c->padding)/span));
                if(b+d>1) { double sum=b+d; b/=sum; d/=sum; }
                V p=f.p[0]*(1-b-d)+f.p[1]*b+f.p[2]*d; double sx,sy;
                if(!project(camera.local(p),c->calibration,sx,sy)||!c->visibility.visible(camera.origin,p,i)) {
                    usable=false; break;
                }
                auto rgba=sample(*image,sx,sy);
                std::copy(rgba.begin(),rgba.end(),tile.begin()+(size_t(y)*f.side+x)*4);
            }
            if(!usable) continue; // An occluded triangle keeps its previous best view.
            auto& atlas=c->atlas[f.page];
            for(int y=0;y<f.side;++y) std::memcpy(atlas.data()+(size_t(f.y+y)*c->size+f.x)*4,
                tile.data()+size_t(y)*f.side*4,size_t(f.side)*4);
            f.score=score;
        }
        c->timestamp=image->timestamp; return TANGO_3DR_SUCCESS;
    } catch(...) { return TANGO_3DR_ERROR; }
}

extern "C" Tango3DR_Status Tango3DR_getTexturedMesh(const Tango3DR_TexturingContext c,Tango3DR_Mesh* out) {
    try {
        using namespace scanner_texture;
        if(!c||!out) return TANGO_3DR_INVALID;
        bool observed=false; for(const auto& f:c->faces) observed|=f.score>0;
        if(!observed) return TANGO_3DR_ERROR;
        OwnedMesh owned; Tango3DR_Mesh& m=owned.mesh;
        auto status=Tango3DR_Mesh_init(uint32_t(c->faces.size()*3),uint32_t(c->faces.size()),c->hasNormals,
            c->hasColors,true,true,uint32_t(c->atlas.size()),c->size,c->size,&m);
        if(status!=TANGO_3DR_SUCCESS) return status;
        m.num_vertices=uint32_t(c->faces.size()*3); m.num_faces=uint32_t(c->faces.size());
        m.num_textures=uint32_t(c->atlas.size()); m.timestamp=c->timestamp;
        for(size_t i=0;i<c->faces.size();++i) {
            const Triangle& f=c->faces[i]; int span=f.side-2*c->padding-1;
            m.texture_ids[i]=f.score>0?f.page:-1;
            for(int k=0;k<3;++k) {
                size_t index=3*i+k; m.faces[i][k]=uint32_t(index);
                for(int a=0;a<3;++a) { m.vertices[index][a]=float(f.p[k][a]); if(m.normals) m.normals[index][a]=float(f.n[k][a]); }
                if(m.colors) std::copy(f.color[k].begin(),f.color[k].end(),m.colors[index]);
                m.texture_coords[index][0]=float((f.x+c->padding+(k==1?span:0)+.5)/c->size);
                m.texture_coords[index][1]=float(1-(f.y+c->padding+(k==2?span:0)+.5)/c->size);
            }
        }
        for(size_t i=0;i<c->atlas.size();++i) {
            // Core Mesh_init must allocate RGBA byte-stride image buffers.
            if(m.textures[i].format!=TANGO_3DR_HAL_PIXEL_FORMAT_RGBA_8888||m.textures[i].stride!=uint32_t(c->size*4)||!m.textures[i].data)
                return TANGO_3DR_ERROR;
            std::memcpy(m.textures[i].data,c->atlas[i].data(),c->atlas[i].size()); m.textures[i].timestamp=c->timestamp;
        }
        owned.release(out); return TANGO_3DR_SUCCESS;
    } catch(...) { return TANGO_3DR_ERROR; }
}

#include "texture_obj.h"
