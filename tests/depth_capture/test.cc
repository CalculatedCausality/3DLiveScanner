#include <depth/frame_capture.h>
#include <depth/record.h>
#include <glm/gtc/matrix_transform.hpp>
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sys/stat.h>

using namespace oc;
static std::vector<char> bytes(const std::string& path) {
    std::ifstream file(path,std::ios::binary);return {std::istreambuf_iterator<char>(file),{}};
}
int main(int argc,char** argv) {
    assert(argc==2);
    std::vector<uint8_t> raw(5*19+17,0),confidence(5*17+11,0),secondary(raw.size(),0);
    auto set=[&](std::vector<uint8_t>& a,int x,int y,unsigned v) {a[y*19+x*3]=uint8_t(v);a[y*19+x*3+1]=uint8_t(v>>8);};
    for(int y=0;y<6;++y)for(int x=0;x<6;++x){set(raw,x,y,2000);set(secondary,x,y,2010);confidence[y*17+x*2]=220;}
    set(raw,2,2,0);confidence[17+2]=50;set(secondary,3,3,3000);
    geometry::Plane d,c,s;
    d.data=raw.data();d.length=raw.size();d.width=d.height=6;d.rowStride=19;d.pixelStride=3;
    c.data=confidence.data();c.length=confidence.size();c.width=c.height=6;c.rowStride=17;c.pixelStride=2;
    s=d;s.data=secondary.data();
    auto frame=depth_test::Capture(d,c,&s,128,.03f,7);assert(frame&&frame->depth.size()==36);
    assert(std::abs(frame->depth[0]-2.01)<1e-6&&frame->depth[7]==2.f&&frame->depth[21]==2.f);
    assert(frame->depth[14]==0&&frame->confidence[14]==0);
    set(raw,0,0,60);set(secondary,0,0,50);
    auto boundary=depth_test::Capture(d,c,&s,128,.03f,7);
    assert(boundary&&boundary->depth[0]==float(50*.001f)); // same float-to-double cutoff as producer
    raw.assign(raw.size(),0);confidence.assign(confidence.size(),0);
    assert(frame->depth[0]>2&&frame->confidence[0]>.8f); // no borrowed storage
    auto invalid=d;invalid.length=1;assert(!depth_test::Capture(invalid,c,nullptr,128,.03f,7));

    glm::dmat4 projection=glm::perspective(glm::radians(70.),1.3,.001,100.);
    glm::dmat4 pose=glm::translate(glm::dmat4(1),glm::dvec3(3,-2,7));
    glm::dmat4 inverse=pose*glm::inverse(projection);glm::dvec2 uv(.2,-.3);double length=100-.001f;
    auto original=geometry::DepthPoint(inverse,uv,length,1.99);
    depth_test::AddLink(frame,0,0,original,2.f,.05f,inverse,uv,length);
    assert(frame->links.size()==1&&frame->links[0].original==original);
    glm::vec4 corrected=original;
    for(int k=0;k<3;++k)corrected[k]=float(double(original[k])+frame->links[0].worldPerMetre[k]*.01);
    auto expected=geometry::DepthPoint(inverse,uv,length,2.0);
    assert(glm::length(glm::vec3(corrected-expected))<2e-6f);
    frame->pointCount=1;frame->cameraTimestamp=100;frame->depthTimestamp=90;
    std::string path=std::string(argv[1])+"/frame.tpu";
    assert(depth_test::WriteRecord(path,*frame,{original},{corrected}));
    auto saved=bytes(path);assert(saved.size()==256+36*8+32+8);
    assert(std::string(saved.data(),8)=="DPTR0001");
    assert(!memcmp(saved.data()+256+36*8,&original,16));
    assert(!memcmp(saved.data()+256+36*8+16,&corrected,16));
    assert(!depth_test::WriteRecord(path,*frame,{original},{}));assert(bytes(path)==saved);
    assert(!depth_test::WriteRecord(std::string(argv[1])+"/missing/frame.tpu",*frame,{original},{corrected}));
    std::cout<<"PASS padded planes, owned native inputs, original filtering, world-ray correction and atomic original-point backup\n";
}
