// SPDX-License-Identifier: Apache-2.0
#ifndef SCANNER_DEPTH_TEST_RECORD_H
#define SCANNER_DEPTH_TEST_RECORD_H
#include <depth/experimental.h>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace oc { namespace depth_test {
// Experimental sidecar: original and corrected world points plus the exact
// packed depth/confidence input and source-pixel links. Existing capture files
// keep their format. Publish this BEFORE integrating corrected measurements.
inline bool WriteRecord(const std::string& path,const Frame& frame,
        const std::vector<glm::vec4>& original,const std::vector<glm::vec4>& corrected) {
    static_assert(sizeof(glm::vec4)==16,"World point layout must be four floats");
    const uint16_t endian=1;
    if(*reinterpret_cast<const uint8_t*>(&endian)!=1||frame.width<=0||frame.height<=0||
       uint64_t(frame.width)*frame.height>262144||frame.pointCount!=original.size()||original.size()!=corrected.size()||
       original.size()>UINT32_MAX||frame.depth.size()!=frame.confidence.size()||
       frame.depth.size()!=size_t(frame.width)*frame.height||frame.links.size()>UINT32_MAX) return false;
    for(const Link& link:frame.links) if(link.point>=original.size()||link.pixel>=frame.depth.size()) return false;
    std::string temporary=path+".tmp-XXXXXX";
    int fd=mkstemp(&temporary[0]);if(fd<0)return false;
    FILE* file=fdopen(fd,"wb");if(!file){close(fd);unlink(temporary.c_str());return false;}
    bool okay=true;
    auto write=[&](const void* data,size_t bytes){if(bytes&&fwrite(data,1,bytes,file)!=bytes)okay=false;};
    const char magic[8]={'D','P','T','R','0','0','0','1'};write(magic,sizeof(magic));
    const char model[]="76ef7a8c8376a1a921e2f055419d27b4eaeca425ac2b430dd3f14a89eae84be0";
    write(model,64);
    uint32_t sizes[4]={uint32_t(frame.width),uint32_t(frame.height),uint32_t(original.size()),uint32_t(frame.links.size())};
    write(sizes,sizeof(sizes));write(&frame.cameraTimestamp,8);write(&frame.depthTimestamp,8);
    write(&frame.generation,8);write(&frame.worldToCamera[0][0],16*sizeof(double));
    write(&frame.minDepth,sizeof(double));write(&frame.maxDepth,sizeof(double));
    write(frame.depth.data(),frame.depth.size()*sizeof(float));
    write(frame.confidence.data(),frame.confidence.size()*sizeof(float));
    write(original.data(),original.size()*sizeof(glm::vec4));
    write(corrected.data(),corrected.size()*sizeof(glm::vec4));
    for(const Link& link:frame.links) { uint32_t pair[2]={link.point,link.pixel};write(pair,sizeof(pair)); }
    if(fflush(file)!=0||fsync(fileno(file))!=0)okay=false;
    if(fclose(file)!=0)okay=false;
    if(okay&&rename(temporary.c_str(),path.c_str())==0)return true;
    unlink(temporary.c_str());return false;
}
} }
#endif
