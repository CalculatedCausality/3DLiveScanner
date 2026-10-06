// SPDX-License-Identifier: Apache-2.0
// Private adapter around production OBJ/image code; included by texturing.cc only.
#ifndef SCANNER_TEXTURE_OBJ_H
#define SCANNER_TEXTURE_OBJ_H
#include <fstream>
#include <sstream>
#include <set>
#include <map>
#include <climits>
#include <cstdlib>
#include <png.h>
#include <turbojpeg.h>

namespace scanner_texture {
// The production codecs have process-global scratch. Callers must serialize
// other oc::Image codec operations with these backend OBJ operations as well.
static std::mutex objMutex;
struct Model {
    std::vector<oc::Mesh> meshes;
    ~Model() {
        // Mesh is shallow-copyable and has no destructor. Delete each image once.
        for(size_t i=0;i<meshes.size();++i) {
            oc::Image* image=meshes[i].image;
            if(!image) continue;
            for(size_t j=i+1;j<meshes.size();++j) if(meshes[j].image==image) meshes[j].image=nullptr;
            delete image;
        }
    }
};
inline bool objPath(const char* path) {
    if(!path) return false;
    std::string p(path);
    return p.size()>4&&p.size()<4096&&p.substr(p.size()-4)==".obj"&&p.find_first_of("\r\n\t")==std::string::npos;
}
inline std::string directory(const std::string& p) {
    size_t slash=p.find_last_of('/'); return slash==std::string::npos?"":p.substr(0,slash+1);
}
inline bool boundedFile(const std::string& path,size_t max=32u*1024u*1024u) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    return f&&f.tellg()>=0&&static_cast<uint64_t>(f.tellg())<=max;
}
// Validate with real codecs before invoking File3d's legacy image loader.
// Its PNG error path is unsafe, so only pre-decoded 8-bit RGB(A) PNGs reach it.
inline bool checkedTexture(const std::string& path,size_t& totalPixels) {
    if(!boundedFile(path)||path.size()<4) return false;
    size_t pixels=0;
    if(path.substr(path.size()-4)==".png") {
        std::ifstream f(path,std::ios::binary); unsigned char header[29]{};
        if(!f.read(reinterpret_cast<char*>(header),sizeof(header))||png_sig_cmp(header,0,8)!=0||
           header[24]!=8||(header[25]!=PNG_COLOR_TYPE_RGB&&header[25]!=PNG_COLOR_TYPE_RGBA)||header[28]!=0) return false;
        png_image image{}; image.version=PNG_IMAGE_VERSION;
        if(!png_image_begin_read_from_file(&image,path.c_str())) { png_image_free(&image); return false; }
        pixels=size_t(image.width)*image.height;
        if(!image.width||!image.height||image.width>4096||image.height>4096||pixels>kMaxPixels-totalPixels) {
            png_image_free(&image); return false;
        }
        image.format=PNG_FORMAT_RGBA;
        void* data=std::malloc(pixels*4);
        bool ok=data&&png_image_finish_read(&image,nullptr,data,0,nullptr);
        std::free(data); png_image_free(&image); if(!ok) return false;
    } else if(path.substr(path.size()-4)==".jpg") {
        std::ifstream f(path,std::ios::binary|std::ios::ate); size_t bytes=size_t(f.tellg());
        std::vector<unsigned char> data(bytes); f.seekg(0);
        if(!f.read(reinterpret_cast<char*>(data.data()),bytes)) return false;
        tjhandle codec=tjInitDecompress(); if(!codec) return false;
        int w=0,h=0,sub=0;
        bool ok=tjDecompressHeader2(codec,data.data(),bytes,&w,&h,&sub)==0;
        pixels=w>0&&h>0?size_t(w)*h:0;
        ok=ok&&pixels&&w<=4096&&h<=4096&&pixels<=kMaxPixels-totalPixels;
        void* decoded=ok?std::malloc(pixels*4):nullptr;
        ok=decoded&&tjDecompress2(codec,data.data(),bytes,static_cast<unsigned char*>(decoded),w,0,h,TJPF_RGBA,0)==0;
        std::free(decoded); tjDestroy(codec); if(!ok) return false;
    } else return false;
    totalPixels+=pixels; return true;
}
struct Material { bool texture=false; double kd[3]={1,1,1}; };
inline bool materialFile(const std::string& path,const std::string& base,std::map<std::string,Material>& materials) {
    if(!boundedFile(path,65536)) return false;
    std::ifstream file(path); std::string line,key; size_t pixels=0;
    while(std::getline(file,line)) {
        if(line.size()>1000) return false;
        std::istringstream in(line); std::string tag; in>>tag;
        if(tag.empty()||tag[0]=='#') continue;
        if(tag=="newmtl") {
            if(!(in>>key)||materials.count(key)||materials.size()>=9) return false;
            materials[key]=Material();
        } else {
            if(key.empty()) return false;
            Material& m=materials[key];
            if(tag=="map_Kd") {
                std::string name,extra; if(!(in>>name)||(in>>extra)||name[0]=='-'||m.texture) return false;
                if(!checkedTexture(name[0]=='/'?name:base+name,pixels)) return false;
                m.texture=true;
            } else if(tag=="Kd") {
                for(double& d:m.kd) if(!(in>>d)||!std::isfinite(d)||d<0||d>1) return false;
            } else if(tag=="Ka"||tag=="Ks"||tag=="Ke") {
                // Native ABI is diffuse-only. Accept neutral or production
                // boilerplate; refuse other unsupported material effects.
                double a,b,c; if(!(in>>a>>b>>c)||a!=b||b!=c) return false;
                if(tag=="Ka"&&a!=0&&a!=1) return false;
                if(tag=="Ks"&&a!=0&&a!=0.5) return false;
                if(tag=="Ke"&&a!=0) return false;
            } else if(tag=="d"||tag=="Tr"||tag=="Ni"||tag=="illum"||tag=="Ns") {
                double d; if(!(in>>d)||!std::isfinite(d)) return false;
                if((tag=="d"||tag=="Ni")&&d!=1) return false;
                if(tag=="Tr"&&d!=0) return false;
                if(tag=="illum"&&d!=0&&d!=1&&d!=2) return false;
                if(tag=="Ns"&&d!=0&&std::abs(d-96.078431)>1e-6) return false;
            } else return false;
        }
    }
    return !file.bad();
}
inline bool preflightObj(const std::string& path,std::vector<Material>& faceMaterials) {
    if(!boundedFile(path)) return false;
    std::ifstream file(path); std::string line,current; size_t vertices=0,uvs=0,normals=0;
    std::map<std::string,Material> materials; std::vector<std::string> names;
    std::vector<bool> faceUvs; size_t materialRuns=0;
    bool library=false;
    while(std::getline(file,line)) {
        if(line.size()>4096) return false;
        std::istringstream in(line); std::string tag; in>>tag;
        if(tag.empty()||tag[0]=='#') continue;
        if(tag=="v"||tag=="vn"||tag=="vt") {
            int n=tag=="vt"?2:3; double d;
            for(int i=0;i<n;++i) if(!(in>>d)||!std::isfinite(d)||std::abs(d)>std::numeric_limits<float>::max()) return false;
            std::string extra; if((in>>extra)&&extra[0]!='#') return false;
            size_t& count=tag=="v"?vertices:tag=="vn"?normals:uvs;
            if(++count>kMaxVertices) return false;
        } else if(tag=="f") {
            std::string token; int corners=0; bool hasUvs=true;
            while(in>>token) {
                if(token[0]=='#') break;
                ++corners; size_t slash=token.find('/');
                hasUvs&=slash!=std::string::npos&&slash+1<token.size()&&token[slash+1]!='/';
            }
            // Restrict to triangles rather than silently accepting File3d's
            // fixed diagonal for possibly concave/nonplanar polygons.
            if(corners!=3) return false;
            for(int i=0;i<corners-2;++i) { names.push_back(current); faceUvs.push_back(hasUvs); }
            if(names.size()>kMaxFaces) return false;
        } else if(tag=="mtllib") {
            std::string name,extra;
            if(library||!(in>>name)||(in>>extra)||name.find('/')!=std::string::npos) return false;
            library=true;
            if(!materialFile(directory(path)+name,directory(path),materials)) return false;
        } else if(tag=="usemtl") {
            std::string extra; if(!(in>>current)||(in>>extra)) return false;
            if(++materialRuns>kMaxFaces) return false;
        } else if(tag!="o"&&tag!="g"&&tag!="s") return false;
    }
    for(size_t i=0;i<names.size();++i) {
        const auto& name=names[i];
        if(!name.empty()&&!materials.count(name)) return false;
        Material m=name.empty()?Material():materials.at(name);
        if(m.texture&&!faceUvs[i]) return false;
        faceMaterials.push_back(m);
    }
    return !file.bad()&&!names.empty();
}
} // namespace scanner_texture

extern "C" Tango3DR_Status Tango3DR_Mesh_loadFromObj(const char* const path,Tango3DR_Mesh* out) {
    try {
        using namespace scanner_texture;
        if(!out||!objPath(path)) return TANGO_3DR_INVALID;
        std::lock_guard<std::mutex> lock(objMutex);
        std::vector<Material> materials; if(!preflightObj(path,materials)) return TANGO_3DR_ERROR;
        Model model; oc::File3d(path,false).ReadModel(INT_MAX,model.meshes);
        size_t vertices=0,pixels=0; std::vector<oc::Image*> images; std::vector<Material> imageMaterials;
        for(auto& m:model.meshes) {
            if(m.vertices.empty()) continue;
            if(m.vertices.size()%3||m.normals.size()!=m.vertices.size()||m.uv.size()!=m.vertices.size()||!m.indices.empty()||
               vertices/3>=materials.size()) return TANGO_3DR_ERROR;
            Material material=materials[vertices/3]; vertices+=m.vertices.size();
            if(!m.image||!m.image->IsValid()) return TANGO_3DR_ERROR;
            bool diffuseImage=material.texture||material.kd[0]!=1||material.kd[1]!=1||material.kd[2]!=1;
            if(diffuseImage&&std::find(images.begin(),images.end(),m.image)==images.end()) {
                images.push_back(m.image); imageMaterials.push_back(material);
                pixels+=size_t(m.image->GetWidth())*m.image->GetHeight();
            }
        }
        if(vertices!=materials.size()*3||vertices>kMaxVertices||images.size()>8||pixels>kMaxPixels) return TANGO_3DR_ERROR;
        OwnedMesh owned; auto& result=owned.mesh;
        auto status=Tango3DR_Mesh_init(uint32_t(vertices),uint32_t(materials.size()),true,false,true,true,
            uint32_t(images.size()),1,1,&result);
        if(status!=TANGO_3DR_SUCCESS) return status;
        result.num_vertices=uint32_t(vertices); result.num_faces=uint32_t(materials.size()); result.num_textures=uint32_t(images.size());
        for(size_t i=0;i<images.size();++i) {
            oc::Image& source=*images[i]; Tango3DR_ImageBuffer& dest=result.textures[i];
            // core.cc Mesh_destroy owns each texture via std::free. Keep that
            // allocator contract without introducing extra exported Image APIs.
            uint8_t* data=static_cast<uint8_t*>(std::malloc(size_t(source.GetWidth())*source.GetHeight()*4));
            if(!data) return TANGO_3DR_ERROR;
            std::free(dest.data); dest.data=data;
            dest.width=source.GetWidth(); dest.height=source.GetHeight(); dest.stride=dest.width*4;
            dest.format=TANGO_3DR_HAL_PIXEL_FORMAT_RGBA_8888;
            bool flip=source.GetExtension()=="jpg";
            for(uint32_t y=0;y<dest.height;++y) {
                uint8_t* row=dest.data+size_t(y)*dest.stride;
                std::memcpy(row,source.GetData()+size_t(flip?dest.height-1-y:y)*dest.stride,dest.stride);
                if(imageMaterials[i].texture) for(uint32_t x=0;x<dest.width;++x)
                    for(int k=0;k<3;++k) row[4*x+k]=byte(row[4*x+k]*imageMaterials[i].kd[k]);
            }
        }
        size_t offset=0;
        for(const auto& m:model.meshes) {
            if(m.vertices.empty()) continue;
            auto found=std::find(images.begin(),images.end(),m.image);
            int texture=found==images.end()?-1:int(found-images.begin());
            for(size_t i=0;i<m.vertices.size();++i) {
                size_t index=offset+i;
                for(int a=0;a<3;++a) { result.vertices[index][a]=m.vertices[i][a]; result.normals[index][a]=m.normals[i][a]; }
                for(int a=0;a<2;++a) result.texture_coords[index][a]=m.uv[i][a];
                result.faces[index/3][index%3]=uint32_t(index); result.texture_ids[index/3]=texture;
            }
            offset+=m.vertices.size();
        }
        if(!validMesh(&result)) return TANGO_3DR_ERROR;
        owned.release(out); return TANGO_3DR_SUCCESS;
    } catch(...) { return TANGO_3DR_ERROR; }
}

extern "C" Tango3DR_Status Tango3DR_Mesh_saveToObj(const Tango3DR_Mesh* mesh,const char* const path) {
    try {
        using namespace scanner_texture;
        if(!objPath(path)||!validMesh(mesh)) return TANGO_3DR_INVALID;
        std::lock_guard<std::mutex> lock(objMutex);
        Model model;
        std::vector<std::unique_ptr<oc::Image>> images(mesh->num_textures);
        std::string base=std::string(path).substr(0,std::strlen(path)-4);
        if(base.substr(directory(base).size()).find(' ')!=std::string::npos) return TANGO_3DR_INVALID;
        for(uint32_t i=0;i<mesh->num_textures;++i) {
            const auto& source=mesh->textures[i];
            images[i].reset(new oc::Image(int(source.width),int(source.height)));
            for(uint32_t y=0;y<source.height;++y) for(uint32_t x=0;x<source.width;++x) {
                auto rgba=pixel(source,x,y); std::copy(rgba.begin(),rgba.end(),images[i]->GetData()+(size_t(y)*source.width+x)*4);
            }
            images[i]->SetName(base+"_texture_"+std::to_string(i)+".png");
        }
        // One group per material bounds File3d's mesh and MTL allocations. Face
        // order may change on OBJ save; corner order/winding/attributes do not.
        model.meshes.resize(mesh->num_textures+1);
        for(uint32_t f=0;f<mesh->num_faces;++f) {
            int texture=mesh->texture_ids?mesh->texture_ids[f]:-1;
            auto& dest=model.meshes[texture+1];
            for(int k=0;k<3;++k) {
                uint32_t index=mesh->faces[f][k];
                if(texture<0&&mesh->colors&&(mesh->colors[index][0]!=255||mesh->colors[index][1]!=255||mesh->colors[index][2]!=255||mesh->colors[index][3]!=255))
                    return TANGO_3DR_INVALID; // Standard OBJ cannot carry arbitrary vertex colors.
                dest.vertices.emplace_back(mesh->vertices[index][0],mesh->vertices[index][1],mesh->vertices[index][2]);
                if(mesh->normals) dest.normals.emplace_back(mesh->normals[index][0],mesh->normals[index][1],mesh->normals[index][2]);
                if(texture>=0) dest.uv.emplace_back(mesh->texture_coords[index][0],mesh->texture_coords[index][1]);
            }
        }
        // Detach must outlive all attachments, including exception unwinding.
        struct Detach { Model& model; ~Detach() { for(auto& m:model.meshes) m.image=nullptr; } } detach{model};
        for(size_t i=0;i<images.size();++i) model.meshes[i+1].image=images[i].get();
        for(auto& image:images) if(!image->Write(image->GetName())) return TANGO_3DR_ERROR;
        if(!oc::File3d(path,true).WriteModel(model.meshes)) return TANGO_3DR_ERROR;
        // File3d emits a fixed .64 diffuse multiplier. Replace its boilerplate
        // so calibrated image values survive standard OBJ/MTL rendering.
        std::ofstream mtl(base+".mtl",std::ios::trunc);
        if(!mtl) return TANGO_3DR_ERROR;
        for(size_t i=0;i<model.meshes.size();++i) {
            if(model.meshes[i].vertices.empty()) continue;
            mtl<<"newmtl "<<i<<"\nKa 0 0 0\nKd 1 1 1\nKs 0 0 0\nd 1\nillum 1\n";
            if(model.meshes[i].image) {
                std::string name=model.meshes[i].image->GetName();
                mtl<<"map_Kd "<<name.substr(directory(name).size())<<"\n";
            }
            mtl<<"\n";
        }
        mtl.close(); return mtl?TANGO_3DR_SUCCESS:TANGO_3DR_ERROR;
    } catch(...) { return TANGO_3DR_ERROR; }
}
#endif
