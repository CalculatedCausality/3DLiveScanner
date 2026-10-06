// SPDX-License-Identifier: Apache-2.0
#include "reconstruction/dataset_texturing.h"
#include "data/dataset.h"
#include "data/image.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace oc;
// Integration contract lock: fail this owned suite before App/deviceprobe builds
// are wasted on accidental Settings/Options or callback/order/name drift.
typedef bool (*ExportContract)(const std::string&,const std::string&,Dataset*,
                               const DatasetTextureSettings&,const DatasetTextureProgress&,
                               DatasetTextureReport&);
static_assert(std::is_same<decltype(&ExportDatasetTexturedObj),ExportContract>::value,"Dataset exporter integration signature changed");
static_assert(std::is_same<DatasetTextureProgress,std::function<bool(const std::string&,double)>>::value,"Cancellable stage/fraction progress contract changed");
static_assert(std::is_same<decltype(DatasetTextureSettings::scratchDirectory),std::string>::value,"Private scratch setting contract changed");
static_assert(std::is_same<decltype(DatasetTextureReport::observedFaces),uint64_t>::value,"Observed-face report contract changed");
static_assert(std::is_same<decltype(DatasetTextureReport::artifacts),std::vector<std::string>>::value,"Artifact cleanup contract changed");
static std::string root;
static void put(const std::string& path,const std::string& text) { std::ofstream f(path);f<<text;assert(f.good()); }
static std::string get(const std::string& path) { std::ifstream f(path);return std::string(std::istreambuf_iterator<char>(f),{}); }
static bool exists(const std::string& path) { struct stat s;return stat(path.c_str(),&s)==0; }
static void noResources(const std::string& dir) {
    DIR* d=opendir(dir.c_str());assert(d);struct dirent* e;
    while((e=readdir(d))) assert(strstr(e->d_name,"dataset-texture-")==nullptr);
    closedir(d);
}
static std::string onlyStage(const std::string& parent) {
    DIR* d=opendir(parent.c_str());assert(d);struct dirent* e;std::string found;
    while((e=readdir(d))) if(strncmp(e->d_name,".dataset-texture-",17)==0) {
        assert(found.empty());found=parent+"/"+e->d_name;
    }
    closedir(d);assert(!found.empty());return found;
}
static void fixture(Dataset& d,int frames,int w=64,int h=96) {
    assert(d.WriteState(frames,w,h,w/2.,h/2.,w*.75,h*.75));
    std::vector<glm::mat4> pose(MAX_CAMERA,glm::mat4(1));
    Image image(w,h);
    // Image::WriteJPG is BOTTOMUP. This deliberately produces red at JPEG top
    // and blue at JPEG bottom, with a green right-hand stripe.
    for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
        glm::ivec4 color=x>w*3/4?glm::ivec4(20,230,20,255):
                         y<h/2?glm::ivec4(20,20,230,255):glm::ivec4(230,20,20,255);
        image.DrawPixel(x,y,color);
    }
    for(int i=0;i<frames;++i) {
        // Vary camera positions rather than giving the scale fixture identical poses.
        pose[COLOR_CAMERA][3][0]=float((i%3)-1)*.015f;
        assert(d.WritePose(i,pose));assert(image.Write(d.GetFileName(i,".jpg")));
    }
}
struct Digest {
    uint64_t vertices=0,normals=0,faces=0,geometry=1469598103934665603ULL,indices=1469598103934665603ULL;
    void hash(uint64_t& h,const std::string& s) { for(unsigned char c:s) h=(h^c)*1099511628211ULL; }
};
static Digest digest(const std::string& path) {
    std::ifstream in(path);std::string s;Digest d;
    while(std::getline(in,s)) {
        if(s.compare(0,2,"v ")==0) { ++d.vertices;d.hash(d.geometry,s); }
        if(s.compare(0,3,"vn ")==0) { ++d.normals;d.hash(d.geometry,s); }
        if(s.compare(0,2,"f ")==0) {
            ++d.faces;std::istringstream row(s.substr(2));std::string t;
            uint64_t faceHash=1469598103934665603ULL;
            while(row>>t) {
                size_t slash=t.find('/'),normal=t.find('/',slash==std::string::npos?t.size():slash+1);
                d.hash(faceHash,t.substr(0,slash)+"/"+(normal==std::string::npos?"":t.substr(normal+1))+" ");
            }
            d.indices+=faceHash; // Material batching may reorder complete faces.
        }
    }
    return d;
}
static void sameGeometry(const std::string& a,const std::string& b) {
    Digest x=digest(a),y=digest(b);
    assert(x.vertices==y.vertices&&x.normals==y.normals&&x.faces==y.faces&&x.geometry==y.geometry&&x.indices==y.indices);
}
static void cleanup(const DatasetTextureReport& r) { for(const auto& s:r.artifacts) assert(unlink(s.c_str())==0); }
static const char* triangle="v -0.5 -0.5 2\nv 0.5 -0.5 2\nv 0 0.5 2\nvn 0 0 -1\nf 1//1 2//1 3//1\n";
static void correctness() {
    std::string dir=root+"/small";assert(mkdir(dir.c_str(),0700)==0);Dataset d(dir);fixture(d,3);
    const std::string in=dir+"/in.obj",out=dir+"/out.obj";
    // Rear parallel triangle and one partially covered triangle must be neutral.
    // The small front occluder covers the middle of the large rear triangle.
    std::string mesh=triangle;
    mesh+="v -0.5 -0.5 2.1\nv 0.5 -0.5 2.1\nv 0 0.5 2.1\nf 4//1 5//1 6//1\n";
    mesh+="v -0.8 -0.8 3\nv 0.8 -0.8 3\nv 0 0.8 3\nf 7//1 8//1 9//1\n";
    mesh+="v 5 5 2\nv 6 5 2\nv 5 6 2\nf 10//1 11//1 12//1\n";
    put(in,mesh);DatasetTextureSettings s;s.textureSize=128;s.textureCount=1;DatasetTextureReport r;
    double completed=0;
    assert(ExportDatasetTexturedObj(in,out,&d,s,[&](const std::string& stage,double fraction){assert(!stage.empty()&&fraction>=completed&&fraction<=1);completed=fraction;return true;},r));
    assert(completed==1&&r.faces==4&&r.observedFaces==1&&r.vertices==12&&r.normals==1);sameGeometry(in,out);assert(get(in)==mesh);
    assert(r.usedFrames==1&&r.sourceWidth==64&&r.sourceHeight==96&&r.photoScaleX==1&&r.photoScaleY==1);
    std::string exported=get(out);assert(exported.find("usemtl neutral")!=std::string::npos);
    // Sample actual exported PNG using exported UV for the first triangle.
    std::istringstream lines(exported);std::string row;std::vector<std::pair<double,double>> uv;
    while(std::getline(lines,row)) if(row.compare(0,3,"vt ")==0) { double u,v;assert(sscanf(row.c_str(),"vt %lf %lf",&u,&v)==2);uv.push_back({u,v}); }
    assert(uv.size()==3);Image png(r.artifacts[0]);assert(png.IsValid());
    auto color=[&](int corner) { int x=int(uv[corner].first*128),y=int((1-uv[corner].second)*128);return png.GetColorRGBA(x,y); };
    assert(color(0).r>180&&color(0).b<60); // top of source JPEG, not Image's bottom-up buffer
    assert(color(2).b>180&&color(2).r<60);
    for(int i=3;i<128*128*4;i+=4) assert(png.GetData()[i]==255);
    std::vector<unsigned char> yuv(64*96*3/2);assert(Image::JPG2YUV(d.GetFileName(0,".jpg"),yuv.data(),64,96));
    // Real ReadJPG is bottom-up; JPG2YUV is top-down. Verify this explicitly.
    Image jpeg(d.GetFileName(0,".jpg"));assert(jpeg.GetColorRGBA(10,10).b>180&&jpeg.GetColorRGBA(10,85).r>180);
    cleanup(r);
    // Pixel-location uncertainty must not become a blanket depth tolerance:
    // a parallel surface just 2 mm behind the foreground stays untextured.
    put(in,std::string(triangle)+"v -.5 -.5 2.002\nv .5 -.5 2.002\nv 0 .5 2.002\nf 4//1 5//1 6//1\n");
    assert(ExportDatasetTexturedObj(in,out,&d,s,{},r)&&r.faces==2&&r.observedFaces==1);
    sameGeometry(in,out);cleanup(r);
    // Failure must preserve an existing destination as well as the source.
    put(out,"previous complete export\n");
    auto fail=[&](const std::string& bad) {
        put(in,bad);std::string before=get(in);DatasetTextureReport e;
        assert(!ExportDatasetTexturedObj(in,out,&d,s,{},e));assert(!e.error.empty()&&e.artifacts.empty());
        assert(get(in)==before&&get(out)=="previous complete export\n");
    };
    fail("v 0 0 1\nv 1 0 1\nv 0 1 1\nf 1 2 9999999999999999999999999\n");
    fail("v nan 0 1\nv 1 0 1\nv 0 1 1\nf 1 2 3\n");
    fail("v 0 0 1\nv 1 0 1\nv 0 1 1\nf 1 2\n");
    fail("v 0 0 -1\nv 1 0 -1\nv 0 1 -1\nf 1 2 3\n");
    fail("v 0 0 1\nv 1 0 1\nv 0 1 1\nf 1/ 2 3\n");
    fail(std::string(triangle)+std::string("# embedded\0NUL",14));
    fail(std::string(5000,'#'));
    put(in,triangle);assert(!ExportDatasetTexturedObj(in,in,&d,s,{},r));assert(get(in)==triangle);
    assert(link(in.c_str(),(dir+"/alias.obj").c_str())==0);
    assert(!ExportDatasetTexturedObj(in,dir+"/alias.obj",&d,s,{},r));
    assert(!ExportDatasetTexturedObj(in,out,&d,s,[](const std::string&,double fraction){return fraction<.95;},r));
    assert(get(out)=="previous complete export\n"&&get(in)==triangle);
    noResources(dir);
    // Final rename failure, after complete PNG and MTL publication, rolls both back.
    assert(mkdir((dir+"/blocked.obj").c_str(),0700)==0);
    assert(!ExportDatasetTexturedObj(in,dir+"/blocked.obj",&d,s,{},r));noResources(dir);
    DatasetTextureSettings excessive=s;excessive.textureSize=4096;excessive.textureCount=4;
    assert(!ExportDatasetTexturedObj(in,out,&d,excessive,{},r));noResources(dir);
    std::string jpg=d.GetFileName(2,".jpg"),saved=get(jpg);put(jpg,saved.substr(0,saved.size()/2));
    assert(!ExportDatasetTexturedObj(in,out,&d,s,{},r));assert(get(out)=="previous complete export\n");put(jpg,saved);
    assert(unlink(jpg.c_str())==0);assert(!ExportDatasetTexturedObj(in,out,&d,s,{},r));put(jpg,saved);noResources(dir);
    std::string mat=d.GetFileName(1,".mat"),pose=get(mat);put(mat,"1 0");
    assert(!ExportDatasetTexturedObj(in,out,&d,s,{},r));put(mat,pose);
    // Indexed and negative references, absent normals, forward references.
    put(in,"f 1 2 3\nv -.5 -.5 2\nv .5 -.5 2\nv 0 .5 2\nf -3 -2 -1\n");
    assert(ExportDatasetTexturedObj(in,out,&d,s,{},r)&&r.observedFaces==2);cleanup(r);
    // Non-identity camera-to-world rotation + translation; use only COLOR_CAMERA.
    assert(d.WriteState(1,64,96,32,48,48,72));
    std::vector<glm::mat4> poseMatrices(MAX_CAMERA,glm::mat4(1));
    poseMatrices[COLOR_CAMERA][0]=glm::vec4(0,0,-1,0);poseMatrices[COLOR_CAMERA][2]=glm::vec4(1,0,0,0);
    poseMatrices[COLOR_CAMERA][3]=glm::vec4(3,1,4,1);assert(d.WritePose(0,poseMatrices));
    put(in,"v 5 .5 4.5\nv 5 .5 3.5\nv 5 1.5 4\nvn -1 0 0\nf 1//1 2//1 3//1\n");
    assert(ExportDatasetTexturedObj(in,out,&d,s,{},r)&&r.observedFaces==1);sameGeometry(in,out);cleanup(r);
    // Best-frame selection must choose the nearer higher-resolution blue image.
    assert(d.WriteState(2,64,96,32,48,48,72));
    poseMatrices.assign(MAX_CAMERA,glm::mat4(1));assert(d.WritePose(0,poseMatrices));
    poseMatrices[COLOR_CAMERA][3][2]=1;assert(d.WritePose(1,poseMatrices));
    Image blue(64,96);for(int y=0;y<96;++y) for(int x=0;x<64;++x) { glm::ivec4 c(10,20,230,255);blue.DrawPixel(x,y,c); }
    assert(blue.Write(d.GetFileName(1,".jpg")));put(in,triangle);
    assert(ExportDatasetTexturedObj(in,out,&d,s,{},r)&&r.observedFaces==1);
    std::string best=get(out);size_t at=best.find("vt ");double u,v;assert(at!=std::string::npos&&sscanf(best.c_str()+at,"vt %lf %lf",&u,&v)==2);
    Image bestPng(r.artifacts[0]);glm::ivec4 c=bestPng.GetColorRGBA(int(u*128),int((1-v)*128));assert(c.b>180&&c.r<60);cleanup(r);
    put(dir+"/source.mtl","newmtl original\nKd 1 0 0\n");
    put(in,"mtllib source.mtl\nv -.5 -.5 2 1 0 0\nv .5 -.5 2 0 1 0\nv 0 .5 2 0 0 1\nvn 0 0 -1\nusemtl original\nf 1//1 2//1 3//1\n");
    assert(ExportDatasetTexturedObj(in,out,&d,s,{},r)&&r.observedFaces==1);sameGeometry(in,out);cleanup(r);
    assert(get(dir+"/source.mtl")=="newmtl original\nKd 1 0 0\n");
    // Extreme aspect ratios must not produce a one-pixel tile/divide-by-zero.
    fixture(d,1,1024,2);s.textureSize=16;
    put(in,"v -.1 -.3 2\nv .1 -.3 2\nv 0 -.1 2\nf 1 2 3\n");
    assert(ExportDatasetTexturedObj(in,out,&d,s,{},r)&&r.observedFaces==1&&r.tileWidth>=2&&r.tileHeight>=2);cleanup(r);
    // All twenty views win disjoint faces, so a tiny explicit atlas budget really
    // does require resampling. Validate density against the actual pixel spans.
    fixture(d,20);s.textureSize=64;
    std::ostringstream disjoint;
    for(int i=0;i<20;++i) {
        poseMatrices.assign(MAX_CAMERA,glm::mat4(1));poseMatrices[COLOR_CAMERA][3][0]=i*4;assert(d.WritePose(i,poseMatrices));
        disjoint<<"v "<<i*4-.25<<" -.25 2\nv "<<i*4+.25<<" -.25 2\nv "<<i*4<<" .25 2\nf "<<i*3+1<<" "<<i*3+2<<" "<<i*3+3<<"\n";
    }
    put(in,disjoint.str());assert(ExportDatasetTexturedObj(in,out,&d,s,{},r));
    assert(r.frames==20&&r.usedFrames==20&&r.observedFaces==20&&r.atlasPages==1&&r.photoScale<1);
    assert(r.photoScaleX==double(r.tileWidth-1)/63&&r.photoScaleY==double(r.tileHeight-1)/95);sameGeometry(in,out);cleanup(r);
    noResources(dir);
    printf("PASS real JPEG/PNG orientation, full geometry, front/back/partial occlusion, atomic failure, malformed resources, aliases, cancellation, indices, rigid poses, best-view selection\n");
}
static void denseSlopingSurface() {
    const std::string dir=root+"/dense-sloping";
    assert(mkdir(dir.c_str(),0700)==0);Dataset d(dir);fixture(d,1);
    std::ostringstream mesh;mesh<<std::setprecision(10);
    const int grid=100;
    for(int y=0;y<=grid;++y) for(int x=0;x<=grid;++x) {
        double px=-.4+.8*x/grid,py=-.4+.8*y/grid;
        mesh<<"v "<<px<<" "<<py<<" "<<2+.4*px-.3*py<<"\n";
    }
    for(int y=0;y<grid;++y) for(int x=0;x<grid;++x) {
        int a=1+y*(grid+1)+x,b=a+1,c=a+grid+1,e=c+1;
        mesh<<"f "<<a<<" "<<b<<" "<<e<<"\nf "<<a<<" "<<e<<" "<<c<<"\n";
    }
    const std::string input=dir+"/input.obj",output=dir+"/output.obj";
    put(input,mesh.str());DatasetTextureSettings settings;settings.textureSize=128;settings.textureCount=1;
    DatasetTextureReport report;
    assert(ExportDatasetTexturedObj(input,output,&d,settings,{},report));
    printf("Dense sloping surface: %llu/%llu faces, %.6f area coverage\n",
        (unsigned long long)report.observedFaces,(unsigned long long)report.faces,
        report.observedArea/report.surfaceArea);fflush(stdout);
    // There is no occluder and every triangle is inside the photo. Fine
    // tessellation must not turn a visible continuous plane into grey holes.
    assert(report.observedFaces>=uint64_t(grid*grid*2)*995/1000);
    assert(report.observedArea/report.surfaceArea>=.995);
    sameGeometry(input,output);cleanup(report);noResources(dir);
}

static void scratchLocation() {
    const std::string dir=root+"/scratch-location",cache=root+"/private-cache";
    assert(mkdir(dir.c_str(),0700)==0&&mkdir(cache.c_str(),0700)==0);
    // Files owned by the caller in the scratch parent must never be touched.
    put(cache+"/vertices.bin","caller-owned sentinel\n");
    Dataset d(dir);fixture(d,3);const std::string in=dir+"/in.obj",out=dir+"/out.obj";put(in,triangle);
    DatasetTextureSettings settings;settings.textureSize=128;settings.textureCount=1;settings.scratchDirectory=cache;
    DatasetTextureReport r;bool inspected=false;
    assert(ExportDatasetTexturedObj(in,out,&d,settings,[&](const std::string& stage,double) {
        if(!inspected&&stage=="Visibility frame 1/3") {
            const std::string geometry=onlyStage(cache),publication=onlyStage(dir);
            assert(exists(geometry+"/vertices.bin")&&exists(geometry+"/faces.bin"));
            assert(!exists(geometry+"/output.obj")&&exists(publication+"/output.obj"));
            assert(!exists(publication+"/vertices.bin")&&!exists(publication+"/faces.bin"));
            inspected=true;
        }
        return true;
    },r));
    assert(inspected&&r.observedFaces==1);sameGeometry(in,out);
    for(const auto& artifact:r.artifacts) assert(artifact.compare(0,dir.size()+1,dir+"/")==0);
    noResources(cache);cleanup(r);noResources(dir);
    assert(get(cache+"/vertices.bin")=="caller-owned sentinel\n");
    put(out,"previous destination\n");
    // Cancel after resources have been published, before the OBJ commit point.
    assert(!ExportDatasetTexturedObj(in,out,&d,settings,[](const std::string&,double fraction){return fraction<1;},r));
    assert(get(out)=="previous destination\n"&&get(in)==triangle&&r.artifacts.empty());noResources(cache);noResources(dir);
    settings.scratchDirectory=cache+"/missing";
    assert(!ExportDatasetTexturedObj(in,out,&d,settings,{},r));
    assert(r.error.find(settings.scratchDirectory)!=std::string::npos&&!exists(settings.scratchDirectory));
    assert(get(out)=="previous destination\n");noResources(dir);
    settings.scratchDirectory=cache+"/vertices.bin";
    assert(!ExportDatasetTexturedObj(in,out,&d,settings,{},r));
    assert(get(cache+"/vertices.bin")=="caller-owned sentinel\n");noResources(cache);noResources(dir);
    // Empty is an explicit destination-side fallback, including on Android.
    settings.scratchDirectory.clear();inspected=false;
    assert(ExportDatasetTexturedObj(in,out,&d,settings,[&](const std::string& stage,double) {
        if(!inspected&&stage=="Visibility frame 1/3") {
            const std::string publication=onlyStage(dir);
            assert(exists(publication+"/vertices.bin")&&exists(publication+"/faces.bin")&&exists(publication+"/output.obj"));
            inspected=true;
        }
        return true;
    },r));
    assert(inspected);sameGeometry(in,out);cleanup(r);noResources(cache);noResources(dir);
    printf("PASS private geometry scratch, destination-side publication, fallback, failure/cancellation cleanup and caller-owned files retained\n");
}
static void materialSegments() {
    // Actual failing capture has 1,462 neutral material runs, flat v/vn triples,
    // and no vt records. This must bypass the old >=9-material preflight too.
    std::string dir=root+"/materials";assert(mkdir(dir.c_str(),0700)==0);Dataset d(dir);fixture(d,3);
    std::ostringstream obj,mtl;obj<<"mtllib source.mtl\n";
    for(int i=0;i<1462;++i) {
        double x=-.6+(i%43)*(1.2/43),y=-.6+(i/43)*(1.2/34);
        obj<<"v "<<x<<" "<<y<<" 2\nvn 0 0 -1\n"
           <<"v "<<x+.02<<" "<<y<<" 2\nvn 0 0 -1\n"
           <<"v "<<x<<" "<<y+.02<<" 2\nvn 0 0 -1\n"
           <<"usemtl raw_"<<i<<"\nf "<<i*3+1<<"//"<<i*3+1<<" "
           <<i*3+2<<"//"<<i*3+2<<" "<<i*3+3<<"//"<<i*3+3<<"\n";
        mtl<<"newmtl raw_"<<i<<"\nKd 0.5 0.5 0.5\nd 1\nillum 1\n\n";
    }
    const std::string in=dir+"/segmented.obj",out=dir+"/textured.obj",source=obj.str(),materials=mtl.str();
    put(in,source);put(dir+"/source.mtl",materials);
    assert(source.find("\nvt ")==std::string::npos);
    DatasetTextureSettings settings;settings.textureSize=128;settings.textureCount=1;DatasetTextureReport r;
    assert(ExportDatasetTexturedObj(in,out,&d,settings,{},r));
    assert(r.faces==1462&&r.observedFaces==1462&&r.vertices==4386&&r.normals==4386);
    sameGeometry(in,out);assert(get(in)==source&&get(dir+"/source.mtl")==materials);
    const std::string exported=get(out);
    assert(exported.find("usemtl raw_")==std::string::npos&&exported.find("mtllib source.mtl")==std::string::npos);
    assert(exported.find("usemtl photo0")!=std::string::npos&&exported.find("\nvt ")!=std::string::npos);
    cleanup(r);noResources(dir);
    printf("PASS 1462 neutral source materials, no source UVs, all 1462 faces/4386 positions/normals retained, source OBJ/MTL unchanged\n");
}
static void scale(uint64_t requested,int frames) {
    std::string dir=root+"/scale";assert(mkdir(dir.c_str(),0700)==0);Dataset d(dir);fixture(d,frames,360,640);
    const std::string in=dir+"/large.obj",out=dir+"/large-textured.obj";
    FILE* f=fopen(in.c_str(),"wb");assert(f);
    int side=int(std::ceil(std::sqrt(requested/2.)));uint64_t faces=0,vertices=0;
    for(int y=0;y<side;++y) for(int x=0;x<side;++x) {
        double x0=-.8+1.6*x/side,x1=-.8+1.6*(x+1)/side,y0=-.8+1.6*y/side,y1=-.8+1.6*(y+1)/side;
        double p[6][2]={{x0,y0},{x1,y0},{x1,y1},{x0,y0},{x1,y1},{x0,y1}};
        for(int j=0;j<6;++j) fprintf(f,"v %.9g %.9g 2\nvn 0 0 -1\n",p[j][0],p[j][1]);
        for(int j=0;j<2;++j) { uint64_t n=vertices+j*3+1;fprintf(f,"f %llu//%llu %llu//%llu %llu//%llu\n",(unsigned long long)n,(unsigned long long)n,(unsigned long long)n+1,(unsigned long long)n+1,(unsigned long long)n+2,(unsigned long long)n+2);++faces; }
        vertices+=6;
    }
    assert(fclose(f)==0);struct stat st;assert(stat(in.c_str(),&st)==0);
    assert(st.st_size>32*1024*1024&&faces>500000);
    DatasetTextureSettings s;DatasetTextureReport r;
    bool ok=ExportDatasetTexturedObj(in,out,&d,s,{},r);
    if(!ok) fprintf(stderr,"Export failed: %s\n",r.error.c_str());assert(ok);
    assert(r.faces==faces&&r.vertices==vertices&&r.normals==vertices&&r.observedFaces==faces);sameGeometry(in,out);
    struct rusage usage;assert(getrusage(RUSAGE_SELF,&usage)==0);
    printf("SCALE input_bytes=%lld faces=%llu vertices=%llu frames=%d observed=%llu seconds=%.3f faces_per_second=%.0f peak_rss_kib=%ld scratch_bytes=%llu tile=%dx%d photo_scale=%.6f used_frames=%d\n",
           (long long)st.st_size,(unsigned long long)faces,(unsigned long long)vertices,frames,(unsigned long long)r.observedFaces,
           r.seconds,faces/r.seconds,usage.ru_maxrss,(unsigned long long)r.scratchBytes,r.tileWidth,r.tileHeight,r.photoScale,r.usedFrames);
    cleanup(r);
}
int main(int argc,char** argv) {
    assert(argc==4);root=argv[1];correctness();denseSlopingSurface();scratchLocation();materialSegments();uint64_t faces=strtoull(argv[2],nullptr,10);int frames=atoi(argv[3]);
    if(faces) scale(faces,frames);return 0;
}
