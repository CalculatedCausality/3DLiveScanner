// SPDX-License-Identifier: Apache-2.0
#include "tango_3d_reconstruction_api.h"
#include "data/image.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <array>
#include <vector>

extern "C" void ReconstructionCore_testFailAfter(long n);

namespace {
using Pixel=std::array<unsigned char,4>;
struct Mesh {
    Tango3DR_Mesh value{};
    ~Mesh() { assert(Tango3DR_Mesh_destroy(&value)==TANGO_3DR_SUCCESS); }
};
struct Context {
    Tango3DR_TexturingContext value=nullptr;
    ~Context() { if(value) assert(Tango3DR_TexturingContext_destroy(value)==TANGO_3DR_SUCCESS); }
};
struct Config {
    Tango3DR_Config value=Tango3DR_Config_create(TANGO_3DR_CONFIG_TEXTURING);
    Config(int size=64,int count=1,double resolution=.05) {
        assert(value);
        integer("mesh_simplification_factor",1); integer("texture_size",size); integer("max_num_textures",count);
        real("bevel",1); real("min_resolution",resolution);
    }
    void integer(const char* key,int value_) { assert(Tango3DR_Config_setInt32(value,key,value_)==TANGO_3DR_SUCCESS); }
    void real(const char* key,double value_) { assert(Tango3DR_Config_setDouble(value,key,value_)==TANGO_3DR_SUCCESS); }
    ~Config() { Tango3DR_Config_destroy(value); }
};
Tango3DR_CameraCalibration calibration() {
    Tango3DR_CameraCalibration c{}; c.width=c.height=64; c.fx=c.fy=64; c.cx=c.cy=32;
    c.calibration_type=TANGO_3DR_CALIBRATION_POLYNOMIAL_3_PARAMETERS; return c;
}
Tango3DR_Pose pose() { Tango3DR_Pose p{}; p.orientation[3]=1; return p; }
void triangle(Mesh& mesh,int faces=1) {
    auto& m=mesh.value;
    assert(Tango3DR_Mesh_init(faces*3,faces,true,false,false,false,0,0,0,&m)==TANGO_3DR_SUCCESS);
    m.num_faces=faces; m.num_vertices=faces*3;
    const float positions[3][3]={{-.5f,-.5f,2},{.5f,-.5f,2},{-.5f,.5f,2}};
    for(int i=0;i<faces*3;++i) {
        std::copy(positions[i%3],positions[i%3]+3,m.vertices[i]);
        m.normals[i][2]=-1; m.faces[i/3][i%3]=i;
    }
}
struct Frame {
    std::vector<unsigned char> data;
    Tango3DR_ImageBuffer image{};
    Frame(int format=TANGO_3DR_HAL_PIXEL_FORMAT_RGBA_8888) {
        image.width=image.height=64; image.format=Tango3DR_ImageFormatType(format);
        image.stride=format==1?64*4+16:format==3?64*3+12:68;
        data.resize(size_t(image.stride)*64*(format==0x11?2:1),0);
        image.data=data.data();
        for(int y=0;y<64;++y) for(int x=0;x<64;++x) {
            int channels=format==1?4:format==3?3:1;
            auto* p=data.data()+y*image.stride+x*channels;
            if(channels>1) { p[0]=x*3; p[1]=y*3; p[2]=120; if(channels==4) p[3]=255; }
            else p[0]=100;
        }
        if(format==0x11) for(int y=0;y<32;++y) for(int x=0;x<64;x+=2) {
            data[64*image.stride+y*image.stride+x]=180;
            data[64*image.stride+y*image.stride+x+1]=90;
        }
    }
};
Pixel at(const Tango3DR_Mesh& m,int face,double b=0,double c=0) {
    int a=m.faces[face][0],bb=m.faces[face][1],cc=m.faces[face][2];
    double u=m.texture_coords[a][0]*(1-b-c)+m.texture_coords[bb][0]*b+m.texture_coords[cc][0]*c;
    double v=m.texture_coords[a][1]*(1-b-c)+m.texture_coords[bb][1]*b+m.texture_coords[cc][1]*c;
    const auto& image=m.textures[m.texture_ids[face]];
    int x=int(std::round(u*image.width-.5)),y=int(std::round((1-v)*image.height-.5));
    x=std::max(0,std::min(int(image.width)-1,x)); y=std::max(0,std::min(int(image.height)-1,y));
    const auto* p=image.data+y*image.stride+4*x; return {{p[0],p[1],p[2],p[3]}};
}
void close(Pixel p,Pixel expected,int tolerance=1) {
    for(int i=0;i<4;++i) if(std::abs(int(p[i])-int(expected[i]))>tolerance) {
        std::cerr<<"channel "<<i<<" got "<<int(p[i])<<" expected "<<int(expected[i])<<"\n"; assert(false);
    }
}
void setup(Context& context,Mesh& mesh,Config& config) {
    context.value=Tango3DR_TexturingContext_create(config.value,&mesh.value); assert(context.value);
    auto c=calibration(); assert(Tango3DR_TexturingContext_setColorCalibration(context.value,&c)==TANGO_3DR_SUCCESS);
}
void projectionAndLifetime() {
    Mesh source,output; triangle(source); Config cfg; Context context; setup(context,source,cfg);
    Frame f; auto p=pose();
    assert(Tango3DR_getTexturedMesh(context.value,&output.value)==TANGO_3DR_ERROR);
    assert(Tango3DR_Mesh_destroy(&source.value)==TANGO_3DR_SUCCESS);
    assert(Tango3DR_updateTexture(context.value,&f.image,&p)==TANGO_3DR_SUCCESS);
    for(long fail:{0,2,6}) {
        Tango3DR_Mesh sentinel{}; sentinel.timestamp=432;
        ReconstructionCore_testFailAfter(fail);
        assert(Tango3DR_getTexturedMesh(context.value,&sentinel)==TANGO_3DR_ERROR);
        ReconstructionCore_testFailAfter(-1);
        assert(sentinel.timestamp==432&&!sentinel.vertices);
    }
    std::fill(f.data.begin(),f.data.end(),0); // borrowed frame must not survive the call
    assert(Tango3DR_getTexturedMesh(context.value,&output.value)==TANGO_3DR_SUCCESS);
    assert(Tango3DR_TexturingContext_destroy(context.value)==TANGO_3DR_SUCCESS); context.value=nullptr;
    auto& m=output.value;
    assert(m.num_faces==1&&m.num_vertices==3&&m.num_textures==1&&m.texture_ids[0]==0);
    close(at(m,0),{{48,48,120,255}}); close(at(m,0,1),{{144,48,120,255}}); close(at(m,0,0,1),{{48,144,120,255}});
    assert(m.vertices[0][0]==-.5f&&m.vertices[0][2]==2&&m.normals[0][2]==-1);
    std::cout<<"projection/metric coordinates/winding/source and result lifetime OK\n";
}
void formatsAndPose() {
    for(int format:{1,3,0x11}) {
        Mesh source,output; triangle(source); Config cfg; Context context;
        // Rotate +90 degrees around Y and translate the entire camera/mesh.
        for(uint32_t i=0;i<source.value.num_vertices;++i) {
            auto& v=source.value.vertices[i]; float oldX=v[0]; v[0]=v[2]+3; v[1]-=2; v[2]=-oldX+4;
        }
        setup(context,source,cfg); Frame frame(format); auto p=pose();
        p.translation[0]=3;p.translation[1]=-2;p.translation[2]=4;
        p.orientation[1]=std::sqrt(.5);p.orientation[3]=std::sqrt(.5);
        assert(Tango3DR_updateTexture(context.value,&frame.image,&p)==TANGO_3DR_SUCCESS);
        assert(Tango3DR_getTexturedMesh(context.value,&output.value)==TANGO_3DR_SUCCESS);
        close(at(output.value,0),format==0x11?Pixel{{173,76,33,255}}:Pixel{{48,48,120,255}});
    }
    std::cout<<"RGB/RGBA/padded full-range NV21 and camera pose inversion OK\n";
}
void perspectiveDistortionAndBestView() {
    Mesh source,output; triangle(source); source.value.vertices[1][2]=3;
    Config cfg; Context context; setup(context,source,cfg); Frame frame;
    auto c=calibration(); c.calibration_type=TANGO_3DR_CALIBRATION_POLYNOMIAL_5_PARAMETERS;
    c.distortion[0]=.1;c.distortion[1]=-.02;c.distortion[2]=.002;c.distortion[3]=-.001;c.distortion[4]=.003;
    assert(Tango3DR_TexturingContext_setColorCalibration(context.value,&c)==TANGO_3DR_SUCCESS);
    auto p=pose(); p.translation[2]=-2;
    std::vector<unsigned char> gradient=frame.data;
    for(size_t i=0;i<frame.data.size();i+=4) { frame.data[i]=250;frame.data[i+1]=frame.data[i+2]=0;frame.data[i+3]=255; }
    assert(Tango3DR_updateTexture(context.value,&frame.image,&p)==TANGO_3DR_SUCCESS);
    frame.data=gradient;frame.image.data=frame.data.data();p=pose();
    assert(Tango3DR_updateTexture(context.value,&frame.image,&p)==TANGO_3DR_SUCCESS);
    assert(Tango3DR_getTexturedMesh(context.value,&output.value)==TANGO_3DR_SUCCESS);
    auto& m=output.value; auto& image=m.textures[0];
    int x0=int(std::round(m.texture_coords[0][0]*image.width-.5));
    int y0=int(std::round((1-m.texture_coords[0][1])*image.height-.5));
    int span=int(std::round((m.texture_coords[1][0]-m.texture_coords[0][0])*image.width));
    int delta=span/3; double b=double(delta)/span;
    // Independent analytic projection of world barycentric point, not screen interpolation.
    double x=(-.5+b)/(2+b),y=(-.5+b)/(2+b),r=x*x+y*y;
    double radial=1+.1*r-.02*r*r+.003*r*r*r;
    double u=64*(x*radial+2*.002*x*y-.001*(r+2*x*x))+32;
    double v=64*(y*radial+.002*(r+2*y*y)-2*.001*x*y)+32;
    const auto* q=image.data+(y0+delta)*image.stride+4*(x0+delta);
    close({{q[0],q[1],q[2],q[3]}},{{(unsigned char)std::round(3*u),(unsigned char)std::round(3*v),120,255}});
    std::cout<<"perspective-correct texels/Brown distortion/better-view replacement OK\n";
}
void occlusion() {
    Mesh source,output; triangle(source,2); Config cfg; Context context;
    // Small foreground triangle occludes only the interior of the rear triangle.
    const float front[3][3]={{-.18f,-.18f,1},{.03f,-.18f,1},{-.18f,.03f,1}};
    for(int k=0;k<3;++k) std::copy(front[k],front[k]+3,source.value.vertices[3+k]);
    setup(context,source,cfg); Frame frame; auto p=pose();
    assert(Tango3DR_updateTexture(context.value,&frame.image,&p)==TANGO_3DR_SUCCESS);
    assert(Tango3DR_getTexturedMesh(context.value,&output.value)==TANGO_3DR_SUCCESS);
    assert(output.value.num_faces==2&&output.value.texture_ids[0]==-1&&output.value.texture_ids[1]>=0);
    // A foreground object behind the camera must not occlude a valid surface.
    for(int k=0;k<3;++k) source.value.vertices[3+k][2]=-1;
    Context next; setup(next,source,cfg); Mesh result;
    assert(Tango3DR_updateTexture(next.value,&frame.image,&p)==TANGO_3DR_SUCCESS);
    assert(Tango3DR_getTexturedMesh(next.value,&result.value)==TANGO_3DR_SUCCESS);
    assert(result.value.texture_ids[0]>=0&&result.value.texture_ids[1]==-1);
    std::cout<<"interior occlusion/behind-camera handling/unseen face markers OK\n";
}
void atlasBoundsAndDownsample() {
    Config cfg(16,2,.001); Mesh source,output; triangle(source,17); Context context; setup(context,source,cfg);
    Frame frame; auto p=pose();
    for(int i=0;i<40;++i) assert(Tango3DR_updateTexture(context.value,&frame.image,&p)==TANGO_3DR_SUCCESS);
    assert(Tango3DR_getTexturedMesh(context.value,&output.value)==TANGO_3DR_SUCCESS);
    assert(output.value.num_faces==17&&output.value.num_textures==2);
    for(unsigned i=0;i<output.value.num_vertices;++i) for(float uv:output.value.texture_coords[i]) assert(uv>0&&uv<1);
    for(unsigned f=0;f<output.value.num_faces;++f) {
        assert(output.value.texture_ids[f]>=0&&output.value.texture_ids[f]<2);
        close(at(output.value,f),{{48,48,120,255}});
        for(unsigned g=f+1;g<output.value.num_faces;++g) if(output.value.texture_ids[f]==output.value.texture_ids[g]) {
            const auto* a=output.value.texture_coords[f*3]; const auto* b=output.value.texture_coords[g*3];
            assert(std::abs(a[0]-b[0])>=5./16-1e-6||std::abs(a[1]-b[1])>=5./16-1e-6);
        }
    }
    Mesh tooMany;triangle(tooMany,19);assert(!Tango3DR_TexturingContext_create(cfg.value,&tooMany.value));
    cfg.integer("texture_size",4096);cfg.integer("max_num_textures",2);
    assert(!Tango3DR_TexturingContext_create(cfg.value,&source.value));
    Config skip;skip.integer("downsample",2);Mesh one,result;triangle(one);Context sampled;setup(sampled,one,skip);
    auto far=pose();far.translation[2]=-1;
    assert(Tango3DR_updateTexture(sampled.value,&frame.image,&far)==TANGO_3DR_SUCCESS);
    std::fill(frame.data.begin(),frame.data.end(),0);
    assert(Tango3DR_updateTexture(sampled.value,&frame.image,&p)==TANGO_3DR_SUCCESS); // skipped
    assert(Tango3DR_getTexturedMesh(sampled.value,&result.value)==TANGO_3DR_SUCCESS);
    close(at(result.value,0),{{64,64,120,255}},1);
    std::cout<<"bounded atlas pages/gutters/exhaustion/repeated frames/downsample OK\n";
}
void invalidInputs() {
    Config cfg;Mesh source,result;triangle(source);Context context;setup(context,source,cfg);Frame frame;auto p=pose();
    cfg.integer("mesh_simplification_factor",3);assert(!Tango3DR_TexturingContext_create(cfg.value,&source.value));
    cfg.integer("mesh_simplification_factor",1);cfg.real("bevel",0);assert(!Tango3DR_TexturingContext_create(cfg.value,&source.value));
    auto c=calibration();c.calibration_type=TANGO_3DR_CALIBRATION_EQUIDISTANT;
    assert(Tango3DR_TexturingContext_setColorCalibration(context.value,&c)==TANGO_3DR_INVALID);
    c=calibration();c.fx=std::numeric_limits<double>::quiet_NaN();
    assert(Tango3DR_TexturingContext_setColorCalibration(context.value,&c)==TANGO_3DR_INVALID);
    frame.image.stride=1;assert(Tango3DR_updateTexture(context.value,&frame.image,&p)==TANGO_3DR_INVALID);
    frame.image.stride=272;p.orientation[3]=0;assert(Tango3DR_updateTexture(context.value,&frame.image,&p)==TANGO_3DR_INVALID);
    p=pose();p.translation[0]=std::numeric_limits<double>::infinity();assert(Tango3DR_updateTexture(context.value,&frame.image,&p)==TANGO_3DR_INVALID);
    assert(Tango3DR_getTexturedMesh(nullptr,&result.value)==TANGO_3DR_INVALID);
    assert(Tango3DR_TexturingContext_destroy(nullptr)==TANGO_3DR_INVALID);
    std::cout<<"invalid inputs and unsupported options rejected OK\n";
}
void write(const std::string& path,const std::string& data) { std::ofstream f(path);f<<data;f.close();assert(f); }
std::string contents(const std::string& path) { std::ifstream f(path); return std::string(std::istreambuf_iterator<char>(f),{}); }
void obj(const std::string& directory) {
    Mesh source,output,loaded;triangle(source);Config cfg;Context context;setup(context,source,cfg);Frame frame;auto p=pose();
    assert(Tango3DR_updateTexture(context.value,&frame.image,&p)==TANGO_3DR_SUCCESS);
    assert(Tango3DR_getTexturedMesh(context.value,&output.value)==TANGO_3DR_SUCCESS);
    std::string path=directory+"/projection.obj";
    assert(Tango3DR_Mesh_saveToObj(&output.value,path.c_str())==TANGO_3DR_SUCCESS);
    assert(contents(path).find("vt ")!=std::string::npos&&contents(path).find("vn ")!=std::string::npos);
    assert(contents(directory+"/projection.mtl").find("Kd 1 1 1")!=std::string::npos);
    assert(Tango3DR_Mesh_loadFromObj(path.c_str(),&loaded.value)==TANGO_3DR_SUCCESS);
    for(int i=0;i<3;++i) for(int k=0;k<3;++k) {
        assert(loaded.value.vertices[i][k]==output.value.vertices[i][k]);
        assert(loaded.value.normals[i][k]==output.value.normals[i][k]);
    }
    close(at(loaded.value,0),{{48,48,120,255}});close(at(loaded.value,0,0,1),{{48,144,120,255}});
    // Compare every decoded exported PNG byte, not only the material reference.
    auto& a=loaded.value.textures[0];auto& b=output.value.textures[0];
    assert(a.width==b.width&&a.height==b.height&&std::memcmp(a.data,b.data,b.stride*b.height)==0);
    std::string geometry="v 0 0 2\nv 1 0 2\nv 0 1 2\nv 1 1 2\nvt 0 0\nvt 1 0\nvt 0 1\nvn 0 0 -1\n";
    write(directory+"/multi.mtl","newmtl red\nKd 1 0 0\nnewmtl green\nKd 0 1 0\n");
    write(directory+"/multi.obj","mtllib multi.mtl\n"+geometry+"usemtl red\nf 1/1/1 2/2/1 3/3/1\nusemtl green\nf 2/1/1 4/2/1 3/3/1\nusemtl red\nf -4/1/1 -3/2/1 -2/3/1\n");
    Mesh multi,roundtrip;
    assert(Tango3DR_Mesh_loadFromObj((directory+"/multi.obj").c_str(),&multi.value)==TANGO_3DR_SUCCESS);
    assert(multi.value.num_faces==3&&multi.value.num_textures==2);
    close(at(multi.value,0),{{255,0,0,255}});close(at(multi.value,1),{{0,255,0,255}});close(at(multi.value,2),{{255,0,0,255}});
    assert(Tango3DR_Mesh_saveToObj(&multi.value,(directory+"/multi_saved.obj").c_str())==TANGO_3DR_SUCCESS);
    assert(Tango3DR_Mesh_loadFromObj((directory+"/multi_saved.obj").c_str(),&roundtrip.value)==TANGO_3DR_SUCCESS);
    assert(roundtrip.value.num_faces==3);close(at(roundtrip.value,1),{{255,0,0,255}});close(at(roundtrip.value,2),{{0,255,0,255}});
    // Malformed faces cannot become silent geometry loss, missing textures cannot
    // become a fake magenta placeholder, and failure leaves output unchanged.
    Tango3DR_Mesh sentinel{};sentinel.timestamp=123;
    write(directory+"/bad.obj",geometry+"f 1 2 99\n");
    assert(Tango3DR_Mesh_loadFromObj((directory+"/bad.obj").c_str(),&sentinel)==TANGO_3DR_ERROR&&sentinel.timestamp==123);
    write(directory+"/bad.obj",geometry+"f 1 2 4 3\n");
    assert(Tango3DR_Mesh_loadFromObj((directory+"/bad.obj").c_str(),&sentinel)==TANGO_3DR_ERROR);
    write(directory+"/bad.mtl","newmtl bad\nmap_Kd missing.png\n");
    write(directory+"/bad.obj","mtllib bad.mtl\n"+geometry+"usemtl bad\nf 1 2 3\n");
    assert(Tango3DR_Mesh_loadFromObj((directory+"/bad.obj").c_str(),&sentinel)==TANGO_3DR_ERROR);
    write(directory+"/missing.png","not a PNG");
    assert(Tango3DR_Mesh_loadFromObj((directory+"/bad.obj").c_str(),&sentinel)==TANGO_3DR_ERROR);
    assert(Tango3DR_Mesh_saveToObj(&output.value,(directory+"/absent/out.obj").c_str())==TANGO_3DR_ERROR);
    std::cout<<"real OBJ/MTL/PNG roundtrip/diffuse material runs/negative indices/error paths OK\n";
}
void jpegDataset(const std::string& directory) {
    std::string jpeg=directory+"/recorded.jpg";
    {
        oc::Image source(64,64);
        for(int y=0;y<64;++y) for(int x=0;x<64;++x) {
            auto* pixel=source.GetData()+4*(y*64+x);
            Pixel color=y>=32?Pixel{{220,30,20,255}}:Pixel{{40,80,200,255}};
            std::copy(color.begin(),color.end(),pixel);
        }
        assert(source.Write(jpeg)); // real production bottom-up JPEG writer
    }
    std::vector<unsigned char> data(64*64*3);
    oc::Image::JPG2YUV(jpeg,data.data(),64,64); // real recorded-frame conversion
    Tango3DR_ImageBuffer frame{};
    frame.width=frame.height=frame.stride=64;frame.format=TANGO_3DR_HAL_PIXEL_FORMAT_YCrCb_420_SP;frame.data=data.data();
    Mesh source,output;triangle(source);Config cfg;Context context;setup(context,source,cfg);auto p=pose();
    assert(Tango3DR_updateTexture(context.value,&frame,&p)==TANGO_3DR_SUCCESS);
    assert(Tango3DR_getTexturedMesh(context.value,&output.value)==TANGO_3DR_SUCCESS);
    close(at(output.value,0),{{220,30,20,255}},5);close(at(output.value,0,0,1),{{40,80,200,255}},5);
    write(directory+"/jpeg.mtl","newmtl photo\nKd 0.5 1 0.25\nmap_Kd recorded.jpg\n");
    write(directory+"/jpeg.obj","mtllib jpeg.mtl\nv 0 0 2\nv 1 0 2\nv 0 1 2\nvt 0.25 0.75\nvt 0.75 0.75\nvt 0.25 0.25\nusemtl photo\nf 1/1 2/2 3/3\n");
    Mesh loaded;
    assert(Tango3DR_Mesh_loadFromObj((directory+"/jpeg.obj").c_str(),&loaded.value)==TANGO_3DR_SUCCESS);
    close(at(loaded.value,0),{{110,30,5,255}},5);close(at(loaded.value,0,0,1),{{20,80,50,255}},5);
    std::cout<<"real dataset JPEG-to-NV21 and JPEG OBJ orientation/diffuse modulation OK\n";
}
} // namespace
int main(int argc,char** argv) {
    std::cout<<std::unitbuf;
    assert(argc==2);
    projectionAndLifetime();formatsAndPose();perspectiveDistortionAndBestView();occlusion();atlasBoundsAndDownsample();invalidInputs();obj(argv[1]);jpegDataset(argv[1]);
    std::cout<<"All texturing host fixtures passed (not a device-quality claim).\n";
}
