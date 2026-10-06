// SPDX-License-Identifier: Apache-2.0
// Streaming, disk-backed projective photo atlas. Deliberately independent of
// File3d and the small-mesh Tango compatibility texturer.
#include "dataset_texturing.h"
#include "data/dataset.h"
#include "data/image.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace oc { namespace {
const size_t Batch = 8192;
const double Near = 1e-5;
void require(bool b, const std::string& message) { if (!b) throw std::runtime_error(message); }
struct File {
    FILE* p;
    File(const std::string& path, const char* mode) : p(fopen(path.c_str(), mode)) {
        require(p != nullptr, "Cannot open " + path + ": " + strerror(errno));
    }
    ~File() { if (p) fclose(p); }
    void finish() {
        bool ok = !ferror(p) && fflush(p) == 0 && fsync(fileno(p)) == 0;
        int r = fclose(p); p = nullptr;
        require(ok && r == 0, "Cannot finish staged file (storage full or I/O error)");
    }
    void seek(uint64_t offset) {
        require(offset <= uint64_t(std::numeric_limits<off_t>::max()) &&
                fseeko(p, off_t(offset), SEEK_SET) == 0, "Scratch seek failed");
    }
    void write(const void* data, size_t size) { require(fwrite(data, 1, size, p) == size, "Scratch/output write failed"); }
    void read(void* data, size_t size) { require(fread(data, 1, size, p) == size, "Truncated scratch/input read"); }
};
struct Stage {
    std::string parent, directory, stem;
    std::vector<std::string> files, published;
    bool committed = false;
    explicit Stage(const std::string& out,const std::string& parentOverride=std::string()) {
        size_t slash = out.find_last_of('/'); parent = slash == std::string::npos ? "." : out.substr(0, slash);
        if (parent.empty()) parent = "/";
        if (!parentOverride.empty()) parent = parentOverride;
        std::string pattern = parent + "/.dataset-texture-XXXXXX";
        std::vector<char> s(pattern.begin(), pattern.end()); s.push_back(0);
        char* created=mkdtemp(s.data());
        require(created != nullptr, "Cannot create staging directory in " + parent + ": " + strerror(errno));
        directory = s.data(); stem = "dataset-texture-" + directory.substr(directory.size() - 6);
    }
    std::string path(const std::string& name) {
        std::string p = directory + "/" + name; files.push_back(p); return p;
    }
    void publish(const std::string& from, const std::string& name) {
        std::string to = parent + "/" + name;
        // O_EXCL is portable to Android emulated storage, which lacks hard links.
        int fd = open(to.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        require(fd >= 0, "Cannot reserve unique output resource");
        close(fd); published.push_back(to);
        require(rename(from.c_str(), to.c_str()) == 0, "Cannot publish texture resource");
    }
    ~Stage() {
        for (const auto& p : files) unlink(p.c_str());
        if (!committed) for (const auto& p : published) unlink(p.c_str());
        rmdir(directory.c_str());
    }
};
struct V { double x,y,z; V(double a=0,double b=0,double c=0):x(a),y(b),z(c){} };
V operator+(V a,V b){return V(a.x+b.x,a.y+b.y,a.z+b.z);}
V operator-(V a,V b){return V(a.x-b.x,a.y-b.y,a.z-b.z);}
V operator*(V a,double b){return V(a.x*b,a.y*b,a.z*b);}
double dot(V a,V b){return a.x*b.x+a.y*b.y+a.z*b.z;}
V cross(V a,V b){return V(a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x);}
struct Point { float p[3]; };
struct Face {
    Point p[3]; uint32_t v[3], n[3]; float score; int32_t frame;
};
struct Bounds {
    V lo,hi; Bounds():lo(1e100,1e100,1e100),hi(-1e100,-1e100,-1e100){}
    void add(V p) { lo=V(std::min(lo.x,p.x),std::min(lo.y,p.y),std::min(lo.z,p.z));
                   hi=V(std::max(hi.x,p.x),std::max(hi.y,p.y),std::max(hi.z,p.z)); }
};
V point(const Point& p) { return V(p.p[0],p.p[1],p.p[2]); }
// 3 MiB direct-mapped vertex cache. Arbitrarily indexed meshes do not require
// all vertices resident; the common flat scanner stream is a sequential hit.
struct VertexCache {
    enum : size_t { Page = 1024, Slots = 256 };
    File& file; uint64_t count;
    std::vector<Point> data; std::array<uint64_t, Slots> tags;
    VertexCache(File& f,uint64_t n):file(f),count(n),data(Page*Slots) { tags.fill(UINT64_MAX); }
    Point get(uint32_t index) {
        uint64_t page = uint64_t(index-1)/Page; size_t slot = page%Slots;
        if (tags[slot]!=page) {
            size_t n=size_t(std::min<uint64_t>(Page,count-page*Page));
            file.seek(page*Page*sizeof(Point)); file.read(&data[slot*Page],n*sizeof(Point)); tags[slot]=page;
        }
        return data[slot*Page+(index-1)%Page];
    }
};
struct Lines {
    FILE* file; std::array<char,65536> buffer; size_t at=0,end=0;
    explicit Lines(FILE* f):file(f){}
    void reset() { at=end=0; }
    bool next(char* text,size_t capacity) {
        size_t length=0;
        for(;;) {
            if(at==end) {
                end=fread(buffer.data(),1,buffer.size(),file);at=0;
                require(!ferror(file),"Input read failed");
                if(!end) { text[length]=0;return length!=0; }
            }
            const char* newline=static_cast<const char*>(memchr(buffer.data()+at,'\n',end-at));
            size_t n=newline?size_t(newline-(buffer.data()+at))+1:end-at;
            require(length+n<capacity,"OBJ record exceeds 4095 bytes");
            require(!memchr(buffer.data()+at,0,n),"NUL byte in OBJ input");
            memcpy(text+length,buffer.data()+at,n);length+=n;at+=n;
            if(newline) { text[length]=0;return true; }
        }
    }
};
struct Tokens {
    char* values[9]; size_t count=0;
    bool empty() const { return !count; }
    size_t size() const { return count; }
    char* operator[](size_t i) const { return values[i]; }
};
Tokens tokens(char* text) {
    Tokens t; char* p=text;
    while (*p) {
        while (*p==' '||*p=='\t'||*p=='\r'||*p=='\n') ++p;
        if (!*p||*p=='#') break;
        require(t.count<9,"Too many fields in OBJ geometry record");
        t.values[t.count++]=p;
        while (*p&&*p!=' '&&*p!='\t'&&*p!='\r'&&*p!='\n'&&*p!='#') ++p;
        if (*p=='#') { *p=0; break; } if (*p) *p++=0;
        // Metadata never needs token storage proportional to its name count.
        if(t.count==1&&(strcmp(t[0],"o")==0||strcmp(t[0],"g")==0||strcmp(t[0],"s")==0||
                       strcmp(t[0],"usemtl")==0||strcmp(t[0],"mtllib")==0)) break;
    }
    return t;
}
double number(const char* p) {
    char* e; errno=0; double x=strtod(p,&e);
    require(e!=p&&!*e&&!errno&&std::isfinite(x)&&std::abs(x)<=std::numeric_limits<float>::max(),"Invalid/nonfinite OBJ number");
    return x;
}
uint32_t index(char* p, uint64_t total, uint64_t seen) {
    char* e; errno=0; long long n=strtoll(p,&e,10);
    require(e!=p&&!*e&&!errno&&n!=0,"Invalid OBJ index");
    if(n<0) { require(n>=-static_cast<long long>(seen),"Negative OBJ index out of range"); n=static_cast<long long>(seen)+n+1; }
    require(n>0&&uint64_t(n)<=total,"OBJ index out of range"); return uint32_t(n);
}
void parse(File& input,File& vertices,File& faces,File& output,DatasetTextureReport& r,
           std::vector<Bounds>& blocks,const DatasetTextureProgress& progress) {
    char text[4096],copy[4096]; uint64_t uv=0,records=0;Lines lines(input.p);
    while(lines.next(text,sizeof(text))) {
        memcpy(copy,text,strlen(text)+1); auto t=tokens(copy); if(t.empty()) continue;
        std::string k=t[0];
        if(k=="v"||k=="vn"||k=="vt") {
            require(k=="v" ? (t.size()==4||t.size()==7||t.size()==8) : k=="vn" ? t.size()==4 : (t.size()>=2&&t.size()<=4),"Unsupported OBJ vertex record");
            Point p={}; for(size_t i=1;i<t.size();++i) { double x=number(t[i]); if(i<4) p.p[i-1]=float(x); }
            if(k=="v") { require(++r.vertices<=UINT32_MAX,"OBJ exceeds 32-bit vertex index space"); vertices.write(&p,sizeof(p)); }
            if(k=="vn") require(++r.normals<=UINT32_MAX,"OBJ exceeds 32-bit normal index space");
            if(k=="vt") require(++uv<=UINT32_MAX,"OBJ exceeds 32-bit UV index space");
            else { output.write(text,strlen(text)); if(!strchr(text,'\n')) output.write("\n",1); }
        } else if(k=="f") {
            require(t.size()==4,"Only triangular OBJ faces supported; no triangulation or geometry changes performed");
            require(++r.faces<=uint64_t(std::numeric_limits<off_t>::max())/sizeof(Face),"Face scratch exceeds platform file-offset range");
        }
        else require(k=="o"||k=="g"||k=="s"||k=="usemtl"||k=="mtllib","Unsupported OBJ record: "+k);
        if((++records%262144)==0&&progress) require(progress("Reading geometry",.03),"Cancelled");
    }
    require(r.vertices&&r.faces,"OBJ contains no triangular surface");
    require(fflush(vertices.p)==0,"Vertex scratch write failed");
    VertexCache cache(vertices,r.vertices); input.seek(0);lines.reset();
    uint64_t seenV=0,seenN=0,seenUV=0,seenF=0; Bounds box;
    std::vector<Face> batch; batch.reserve(Batch);
    while(lines.next(text,sizeof(text))) {
        auto t=tokens(text); if(t.empty()) continue; std::string k=t[0];
        if(k=="v") ++seenV; if(k=="vn") ++seenN; if(k=="vt") ++seenUV;
        if(k!="f") continue;
        Face f={}; f.frame=-1;
        for(int j=0;j<3;++j) {
            char* a=t[j+1]; char* b=strchr(a,'/'); char* c=nullptr;
            if(b) { *b++=0; c=strchr(b,'/'); if(c) *c++=0; }
            f.v[j]=index(a,r.vertices,seenV);
            if(b&&*b) index(b,uv,seenUV);
            require(!b||*b||c,"Empty OBJ texture index");
            if(c) { require(*c!=0,"Empty OBJ normal index"); f.n[j]=index(c,r.normals,seenN); }
            f.p[j]=cache.get(f.v[j]); box.add(point(f.p[j]));
        }
        batch.push_back(f); ++seenF;
        if(batch.size()==Batch||seenF==r.faces) {
            faces.write(batch.data(),batch.size()*sizeof(Face)); blocks.push_back(box); box=Bounds(); batch.clear();
            if(progress) require(progress("Indexing geometry",.08),"Cancelled");
        }
    }
    require(seenF==r.faces&&fflush(faces.p)==0,"Input changed or scratch write failed");
    r.scratchBytes=r.vertices*sizeof(Point)+r.faces*sizeof(Face);
}
struct Camera {
    V origin,row[3]; double fx,fy,cx,cy; int w,h;
    V local(V p) const { V d=p-origin; return V(dot(row[0],d),dot(row[1],d),dot(row[2],d)); }
    V project(V p) const { return V(fx*p.x/p.z+cx,fy*p.y/p.z+cy,1/p.z); }
    bool relevant(const Bounds& b) const {
        int outside[5]={};
        for(int i=0;i<8;++i) {
            V p=local(V(i&1?b.hi.x:b.lo.x,i&2?b.hi.y:b.lo.y,i&4?b.hi.z:b.lo.z));
            outside[0]+=p.z<Near; outside[1]+=fx*p.x+cx*p.z<0;
            outside[2]+=fx*p.x+(cx-w+1)*p.z>0;
            outside[3]+=fy*p.y+cy*p.z<0; outside[4]+=fy*p.y+(cy-h+1)*p.z>0;
        }
        for(int n:outside) if(n==8) return false; return true;
    }
};
Camera camera(Dataset* dataset,int i,int w,int h,double fx,double fy,double cx,double cy) {
    std::vector<glm::mat4> matrices;
    require(dataset->ReadPose(i,matrices)&&matrices.size()==MAX_CAMERA,"Missing/truncated pose for frame "+std::to_string(i));
    const glm::mat4& m=matrices[COLOR_CAMERA];
    for(int c=0;c<4;++c) for(int r=0;r<4;++r) require(std::isfinite(m[c][r]),"Nonfinite camera pose");
    require(std::abs(m[3][3]-1)<.001,"Non-affine camera pose");
    for(int c=0;c<3;++c) {
        require(std::abs(m[c][3])<.001,"Non-affine camera pose");
        for(int r=0;r<3;++r) require(std::abs(glm::dot(glm::vec3(m[c]),glm::vec3(m[r]))-(c==r?1:0))<.002,"Non-rigid camera pose");
    }
    require(glm::determinant(glm::mat3(m))>.998,"Reflected camera pose");
    Camera cam; cam.origin=V(m[3][0],m[3][1],m[3][2]);
    for(int j=0;j<3;++j) cam.row[j]=V(m[j][0],m[j][1],m[j][2]);
    cam.w=w;cam.h=h;cam.fx=fx;cam.fy=fy;cam.cx=cx;cam.cy=cy;return cam;
}
double edge(V a,V b,double x,double y) { return (b.x-a.x)*(y-a.y)-(b.y-a.y)*(x-a.x); }
// Sample within the actual triangle, never its extrapolated plane. A subpixel
// triangle uses its projected centroid instead of disappearing from visibility.
// The caller supersamples the photo grid within a fixed depth-buffer budget.
template<class F> bool raster(V a,V b,V c,int w,int h,F visit) {
    double area=edge(a,b,c.x,c.y); if(std::abs(area)<1e-16) return true;
    if(area<0) { std::swap(b,c); area=-area; }
    double minx=std::min(a.x,std::min(b.x,c.x)),maxx=std::max(a.x,std::max(b.x,c.x));
    double miny=std::min(a.y,std::min(b.y,c.y)),maxy=std::max(a.y,std::max(b.y,c.y));
    if(maxx<-.5||maxy<-.5||minx>w-.5||miny>h-.5) return true;
    int x0=int(std::max(0.,std::ceil(minx-.5))),x1=int(std::min(double(w-1),std::floor(maxx+.5)));
    int y0=int(std::max(0.,std::ceil(miny-.5))),y1=int(std::min(double(h-1),std::floor(maxy+.5)));
    bool sampled=false;
    for(int y=y0;y<=y1;++y) for(int x=x0;x<=x1;++x) {
        double u=edge(b,c,x,y),v=edge(c,a,x,y),q=edge(a,b,x,y);
        if(u<0||v<0||q<0) continue;
        double iz=(u*a.z+v*b.z+q*c.z)/area;
        if(iz>0&&std::isfinite(iz)) {
            sampled=true;
            if(!visit(size_t(y)*w+x,iz,0.)) return false;
        }
    }
    if(!sampled) {
        double x=(a.x+b.x+c.x)/3,y=(a.y+b.y+c.y)/3,iz=(a.z+b.z+c.z)/3;
        if(x>=0&&y>=0&&x<w&&y<h&&iz>0&&std::isfinite(iz)) {
            int px=std::min(w-1,int(x+.5)),py=std::min(h-1,int(y+.5));
            // Two centroids stored in one subpixel need not lie at the same
            // location on a tilted surface. Bound reciprocal-depth variation
            // to the grid sample from the actual projected plane gradient.
            // Unlike a fixed metric tolerance, this preserves separation of
            // even closely spaced parallel layers (their gradient is zero).
            double dx=((b.z-a.z)*(c.y-a.y)-(c.z-a.z)*(b.y-a.y))/area;
            double dy=((b.x-a.x)*(c.z-a.z)-(c.x-a.x)*(b.z-a.z))/area;
            double error=std::abs(dx*(px-x))+std::abs(dy*(py-y));
            if(std::isfinite(error)) return visit(size_t(py)*w+px,iz,error);
        }
    }
    return true;
}
void depthFace(const Face& f,const Camera& cam,std::vector<float>& depth) {
    V in[4],out[4]; int count=0;
    for(int i=0;i<3;++i) in[i]=cam.local(point(f.p[i]));
    // Clip occluders at the near plane; triangles crossing camera or viewport
    // boundaries must still occlude candidates fully inside the image.
    for(int i=0;i<3;++i) {
        V a=in[i],b=in[(i+1)%3]; bool ai=a.z>=Near,bi=b.z>=Near;
        if(ai) out[count++]=a;
        if(ai!=bi) out[count++]=a+(b-a)*((Near-a.z)/(b.z-a.z));
    }
    for(int i=1;i+1<count;++i) raster(cam.project(out[0]),cam.project(out[i]),cam.project(out[i+1]),cam.w,cam.h,
        [&](size_t at,double z,double error) {
            depth[at]=std::max(depth[at],float(std::max(0.,z-error)));return true;
        });
}
bool candidate(const Face& f,const Camera& cam,V (&uv)[3],double& score) {
    V p[3];
    for(int j=0;j<3;++j) {
        p[j]=cam.local(point(f.p[j])); if(p[j].z<Near) return false;
        uv[j]=cam.project(p[j]);
        if(uv[j].x<0||uv[j].y<0||uv[j].x>cam.w-1||uv[j].y>cam.h-1) return false;
    }
    V n=cross(p[1]-p[0],p[2]-p[0]),center=(p[0]+p[1]+p[2])*(1./3);
    double denom=dot(n,n)*dot(center,center); if(!(denom>0)) return false;
    double cosine=std::abs(dot(n,center))/std::sqrt(denom);
    score=std::abs(edge(uv[0],uv[1],uv[2].x,uv[2].y))*cosine;
    return cosine>.05&&score>f.score&&std::isfinite(score)&&score<=std::numeric_limits<float>::max();
}
void update(File& scratch,uint64_t faceCount,const std::vector<Bounds>& blocks,const Camera& cam,
            int frame,const DatasetTextureProgress& progress,double fraction) {
    int scale=4;
    while(scale>1&&uint64_t(cam.w)*cam.h*scale*scale>4u*1024u*1024u) --scale;
    Camera view=cam;
    view.w*=scale;view.h*=scale;view.fx*=scale;view.fy*=scale;
    view.cx=cam.cx*scale+(scale-1)*.5;view.cy=cam.cy*scale+(scale-1)*.5;
    std::vector<float> depth(size_t(view.w)*view.h,0);
    std::vector<Face> batch(Batch);
    for(int pass=0;pass<2;++pass) for(size_t b=0;b<blocks.size();++b) {
        if(!cam.relevant(blocks[b])) continue;
        size_t n=size_t(std::min<uint64_t>(Batch,faceCount-uint64_t(b)*Batch));
        uint64_t offset=uint64_t(b)*Batch*sizeof(Face); scratch.seek(offset);scratch.read(batch.data(),n*sizeof(Face));
        bool changed=false;
        for(size_t j=0;j<n;++j) {
            Face& f=batch[j];
            if(!pass) { depthFace(f,view,depth); continue; }
            V uv[3]; double score;
            if(!candidate(f,cam,uv,score)) continue;
            for(V& p:uv) { p.x=p.x*scale+(scale-1)*.5;p.y=p.y*scale+(scale-1)*.5; }
            bool sampled=false;
            bool visible=raster(uv[0],uv[1],uv[2],view.w,view.h,[&](size_t at,double iz,double error) {
                sampled=true;
                // 1e-5 relative reciprocal-depth tolerance; two-sided geometry
                // contributes to depth, including faces with no assigned photo.
                return depth[at]<=(iz+error)*(1+1e-5)+1e-7;
            });
            if(visible&&sampled) { f.frame=frame;f.score=float(score);changed=true; }
        }
        if(changed) { scratch.seek(offset); scratch.write(batch.data(),n*sizeof(Face)); }
        if(progress&&(b%32==0)) require(progress("Visibility frame "+std::to_string(frame+1),fraction),"Cancelled");
    }
}
struct Layout { int cols,rows,tw,th,capacity,pages; double scale; };
Layout layout(int frames,int w,int h,int size,int pages) {
    Layout best={};
    int per=(frames+pages-1)/pages;
    for(int cols=1;cols<=per&&cols<=size/5;++cols) {
        int rows=(per+cols-1)/cols;
        int aw=size/cols-4,ah=size/rows-4;
        if(aw<2||ah<2) continue;
        double scale=std::min(double(aw-1)/(w-1),double(ah-1)/(h-1));
        // Never invent photo detail by upsampling.
        scale=std::min(1.,scale);
        if(scale>best.scale) best={cols,rows,std::max(2,int(std::floor((w-1)*scale))+1),
                                 std::max(2,int(std::floor((h-1)*scale))+1),cols*rows,pages,scale};
    }
    require(best.scale>0,"Texture budget cannot hold all source-photo tiles (minimum 2x2 plus gutters)"); return best;
}
void tileOrigin(const Layout& l,int frame,int& page,int& x,int& y) {
    page=frame/l.capacity; int slot=frame%l.capacity;
    x=(slot%l.cols)*(l.tw+4)+2; y=(slot/l.cols)*(l.th+4)+2;
}
void decode(Dataset* dataset,int frame,int w,int h,std::vector<unsigned char>& yuv) {
    std::string path=dataset->GetFileName(frame,".jpg"); struct stat st;
    // Image::JPG2YUV retains compressed bytes and planar decode simultaneously.
    // Bound its compressed allocation relative to validated image dimensions.
    require(stat(path.c_str(),&st)==0&&S_ISREG(st.st_mode)&&st.st_size>0&&uint64_t(st.st_size)<=uint64_t(w)*h*8+1048576,
            "Missing/oversized JPEG for frame "+std::to_string(frame));
    require(Image::JPG2YUV(path,yuv.data(),w,h),"Invalid/truncated/non-444 JPEG or dimensions for frame "+std::to_string(frame));
}
unsigned char byte(double x) { return static_cast<unsigned char>(std::max(0.,std::min(255.,std::round(x)))); }
void rgb(const std::vector<unsigned char>& yuv,int w,int h,int x,int y,double (&out)[3]) {
    size_t p=size_t(y)*w+x,uv=size_t(w)*h+size_t(y/2)*w+(x&~1);
    double yy=yuv[p],v=yuv[uv]-128.,u=yuv[uv+1]-128.;
    out[0]=yy+1.402*v;out[1]=yy-.344136*u-.714136*v;out[2]=yy+1.772*u;
}
void fillTile(Image& atlas,const std::vector<unsigned char>& yuv,int w,int h,const Layout& l,int frame) {
    int page,ox,oy;tileOrigin(l,frame,page,ox,oy); int size=atlas.GetWidth();
    for(int y=-2;y<l.th+2;++y) for(int x=-2;x<l.tw+2;++x) {
        double sx=std::max(0,std::min(l.tw-1,x))*double(w-1)/(l.tw-1);
        double sy=std::max(0,std::min(l.th-1,y))*double(h-1)/(l.th-1);
        int ix=int(sx),iy=int(sy); double a[3],b[3],c[3],d[3];
        rgb(yuv,w,h,ix,iy,a);rgb(yuv,w,h,std::min(ix+1,w-1),iy,b);
        rgb(yuv,w,h,ix,std::min(iy+1,h-1),c);rgb(yuv,w,h,std::min(ix+1,w-1),std::min(iy+1,h-1),d);
        size_t target=(size_t(oy+y)*size+ox+x)*4;
        for(int k=0;k<3;++k) atlas.GetData()[target+k]=byte((1-(sy-iy))*((1-(sx-ix))*a[k]+(sx-ix)*b[k])+(sy-iy)*((1-(sx-ix))*c[k]+(sx-ix)*d[k]));
        atlas.GetData()[target+3]=255;
    }
}
} // namespace

bool ExportDatasetTexturedObj(const std::string& inputObj,const std::string& outputObj,Dataset* dataset,
                             const DatasetTextureSettings& settings,const DatasetTextureProgress& progress,
                             DatasetTextureReport& report) {
    report=DatasetTextureReport(); auto start=std::chrono::steady_clock::now();
    try {
        require(dataset,"Dataset is null");
        struct stat source,target;
        require(stat(inputObj.c_str(),&source)==0&&S_ISREG(source.st_mode),"Input OBJ is not a regular file");
        require(inputObj!=outputObj&&!(stat(outputObj.c_str(),&target)==0&&source.st_dev==target.st_dev&&source.st_ino==target.st_ino),"Output aliases input OBJ");
        int size=settings.textureSize,count=settings.textureCount;
        require(size>=16&&size<=4096&&count>=0&&count<=8,"Unsupported texture settings: size 16..4096, count 0..8");
        int maximum=std::min(8,int((16u*1024u*1024u)/(uint64_t(size)*size)));
        if(!count) count=maximum;
        require(count<=maximum,"Requested atlas exceeds 64 MiB RGBA budget; settings were not changed");
        int frames,w,h; double cx,cy,fx,fy;dataset->ReadState(frames,w,h,cx,cy,fx,fy);
        require(frames>0&&frames<=100000&&w>=2&&h>=2&&w<=8192&&h<=8192&&!(w&1)&&!(h&1)&&
                uint64_t(w)*h<=16u*1024u*1024u&&std::isfinite(cx)&&std::isfinite(cy)&&std::isfinite(fx)&&std::isfinite(fy)&&
                fx>0&&fy>0&&fx<=1e9&&fy<=1e9&&cx>=0&&cx<w&&cy>=0&&cy<h,"Invalid/missing dataset calibration");
        report.frames=frames;report.atlasPages=count;report.sourceWidth=w;report.sourceHeight=h;
        std::vector<Camera> cameras; cameras.reserve(frames);
        for(int i=0;i<frames;++i) cameras.push_back(camera(dataset,i,w,h,fx,fy,cx,cy));
        Stage stage(outputObj);
        std::unique_ptr<Stage> geometryStage;
        if(!settings.scratchDirectory.empty()) geometryStage.reset(new Stage(outputObj,settings.scratchDirectory));
        Stage& scratch=geometryStage?*geometryStage:stage;
        std::string objPath=stage.path("output.obj"),mtlName=stage.stem+".mtl",mtlPath=stage.path(mtlName);
        File input(inputObj,"rb"),vertices(scratch.path("vertices.bin"),"w+b"),faces(scratch.path("faces.bin"),"w+b"),output(objPath,"wb");
        require(fprintf(output.p,"# Full geometry dataset photo atlas\nmtllib %s\n",mtlName.c_str())>0,"Output write failed");
        std::vector<Bounds> blocks;parse(input,vertices,faces,output,report,blocks,progress);
        std::vector<unsigned char> yuv(size_t(w)*h*3/2);
        for(int i=0;i<frames;++i) {
            decode(dataset,i,w,h,yuv); // Validate EVERY committed resource, even an unused view.
            double fraction=.1+.65*double(i)/frames;
            if(progress) require(progress("Visibility frame "+std::to_string(i+1)+"/"+std::to_string(frames),fraction),"Cancelled");
            update(faces,report.faces,blocks,cameras[i],i,progress,fraction);
        }
        // Pack only winning source photos. Unused views must not consume the
        // explicit atlas budget or reduce the density of the delivered texture.
        std::vector<Face> batch(Batch);std::vector<int> frameTile(frames,-1),photoFrames;
        for(uint64_t begin=0;begin<report.faces;begin+=Batch) {
            size_t n=size_t(std::min<uint64_t>(Batch,report.faces-begin));faces.seek(begin*sizeof(Face));faces.read(batch.data(),n*sizeof(Face));
            for(size_t j=0;j<n;++j) {
                V normal=cross(point(batch[j].p[1])-point(batch[j].p[0]),point(batch[j].p[2])-point(batch[j].p[0]));
                double area=.5*std::sqrt(dot(normal,normal));report.surfaceArea+=area;
                if(batch[j].frame>=0) {
                    ++report.observedFaces;report.observedArea+=area;frameTile[batch[j].frame]=0;
                }
            }
            if(progress) require(progress("Collecting selected photos",.75),"Cancelled");
        }
        require(report.observedFaces>0,"No faces were observed by any dataset camera");
        for(int frame=0;frame<frames;++frame) if(frameTile[frame]>=0) {
            frameTile[frame]=int(photoFrames.size());photoFrames.push_back(frame);
        }
        report.usedFrames=int(photoFrames.size());
        Layout l=layout(report.usedFrames,w,h,size,count);
        report.tileWidth=l.tw;report.tileHeight=l.th;report.photoScale=l.scale;
        report.photoScaleX=double(l.tw-1)/(w-1);report.photoScaleY=double(l.th-1)/(h-1);
        uint64_t uvIndex=0,writtenFaces=0;
        // The app's OBJ reader creates a render mesh at each material switch.
        // Group output by atlas page (plus neutral), rather than generating
        // millions of tiny meshes from per-triangle best-view assignments.
        // Additional scratch passes keep this grouping bounded in memory.
        for(int material=-1;material<count;++material) {
        bool materialWritten=false;
        for(uint64_t begin=0;begin<report.faces;begin+=Batch) {
            size_t n=size_t(std::min<uint64_t>(Batch,report.faces-begin));faces.seek(begin*sizeof(Face));faces.read(batch.data(),n*sizeof(Face));
            for(size_t j=0;j<n;++j) {
                const Face& f=batch[j];int page=-1,x=0,y=0;
                if(f.frame>=0) tileOrigin(l,frameTile[f.frame],page,x,y);
                if(page!=material) continue;
                if(f.frame>=0) {
                    for(int k=0;k<3;++k) {
                        V uv=cameras[f.frame].project(cameras[f.frame].local(point(f.p[k])));
                        // JPG2YUV is top-down; PNG writer preserves rows. OBJ v=1
                        // is PNG's top row, with texel-centre alignment.
                        fprintf(output.p,"vt %.10g %.10g\n",(x+.5+uv.x*(l.tw-1)/(w-1))/size,1-(y+.5+uv.y*(l.th-1)/(h-1))/size);
                    }
                }
                if(!materialWritten) { if(page<0) fprintf(output.p,"usemtl neutral\n");else fprintf(output.p,"usemtl photo%d\n",page);materialWritten=true; }
                fprintf(output.p,"f");
                for(int k=0;k<3;++k) {
                    fprintf(output.p," %u",f.v[k]);
                    if(page>=0) fprintf(output.p,"/%llu",static_cast<unsigned long long>(uvIndex+k+1));
                    else if(f.n[k]) fprintf(output.p,"/");
                    if(f.n[k]) fprintf(output.p,"/%u",f.n[k]);
                }
                fprintf(output.p,"\n");if(page>=0) uvIndex+=3;++writtenFaces;
            }
            require(!ferror(output.p),"OBJ output write failed");
            if(progress) require(progress("Writing full geometry",.76+.14*(material+1+double(begin)/report.faces)/(count+1)),"Cancelled");
        }
        }
        require(writtenFaces==report.faces,"Output face count mismatch");
        output.finish();File mtl(mtlPath,"wb");
        std::vector<std::pair<std::string,std::string>> resources;
        fprintf(mtl.p,"newmtl neutral\nKd 0.5 0.5 0.5\nd 1\nillum 1\n");
        for(int page=0;page<count;++page) {
            std::string name=stage.stem+"-"+std::to_string(page)+".png",path=stage.path(name);
            Image atlas(size,size);require(atlas.IsValid(),"Atlas allocation failed");
            memset(atlas.GetData(),128,size_t(size)*size*4);
            for(size_t k=3;k<size_t(size)*size*4;k+=4) atlas.GetData()[k]=255;
            for(int slot=page*l.capacity;slot<std::min(report.usedFrames,(page+1)*l.capacity);++slot) {
                decode(dataset,photoFrames[slot],w,h,yuv);fillTile(atlas,yuv,w,h,l,slot);
            }
            require(atlas.Write(path),"PNG write failed");
            { File sync(path,"r+b");sync.finish(); }
            fprintf(mtl.p,"\nnewmtl photo%d\nKd 1 1 1\nd 1\nillum 1\nmap_Kd %s\n",page,name.c_str());
            resources.push_back(std::make_pair(path,name));
            if(progress) require(progress("Writing atlas",.9+.09*double(page+1)/count),"Cancelled");
        }
        mtl.finish();
        for(const auto& resource:resources) stage.publish(resource.first,resource.second);
        stage.publish(mtlPath,mtlName);
        struct stat after;require(fstat(fileno(input.p),&after)==0&&after.st_size==source.st_size&&after.st_mtime==source.st_mtime,"Input changed during export");
        if(progress) require(progress("Publishing",1),"Cancelled");
        // Allocate result before the commit point; nothing may throw afterwards.
        report.artifacts=stage.published;report.artifacts.push_back(outputObj);
        require(rename(objPath.c_str(),outputObj.c_str())==0,"Cannot atomically publish OBJ");
        stage.committed=true;
        report.seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();return true;
    } catch(const std::exception& e) { report.error=e.what(); }
      catch(...) { report.error="Unexpected allocation/codec/export failure"; }
    report.artifacts.clear();report.seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();return false;
}
} // namespace oc
