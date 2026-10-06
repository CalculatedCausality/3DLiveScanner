#define main original_core_test_main
#include "core_test.cc"
#undef main
#include "paging.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
extern "C" void ReconstructionCore_testPagingFault(Tango3DR_ReconstructionContext,int,int64_t);
extern "C" int ReconstructionCore_testPagingFd(Tango3DR_ReconstructionContext);
static const uint64_t S = 98304;
ScannerReconstruction_PagingStats stats(Context& c) {
    ScannerReconstruction_PagingStats s{};
    CHECK(ScannerReconstruction_getPagingStats(c.p,&s)==0); return s;
}
void enable(Context& c,const char* path,uint64_t resident=8*S,uint64_t disk=256*S,uint32_t logical=2048) {
    CHECK(ScannerReconstruction_enablePaging(c.p,path,resident,disk,logical)==0);
}
std::vector<uint8_t> exact(Context& c,const std::set<Key>& keys) {
    std::vector<uint8_t> bytes;
    auto append=[&](const void* p,size_t n) { if(n) { auto b=static_cast<const uint8_t*>(p); bytes.insert(bytes.end(),b,b+n); } };
    for(const auto& k:keys) {
        Mesh m; CHECK(Tango3DR_extractMeshSegment(c.p,k.data(),&m.p)==0);
        append(k.data(),12); append(&m.p.timestamp,8);
        append(&m.p.num_vertices,4); append(&m.p.num_faces,4);
        append(&m.p.max_num_vertices,4); append(&m.p.max_num_faces,4);
        uint32_t flags=(m.p.normals?1:0)|(m.p.colors?2:0); append(&flags,4);
        append(m.p.vertices,size_t(m.p.num_vertices)*12);
        if(m.p.normals) append(m.p.normals,size_t(m.p.num_vertices)*12);
        if(m.p.colors) append(m.p.colors,size_t(m.p.num_vertices)*4);
        append(m.p.faces,size_t(m.p.num_faces)*12);
    }
    return bytes;
}
std::set<Key> pairUpdate(Context& ram,Context& paged,Cloud& p,const Tango3DR_Pose& pose,
                        const Tango3DR_ImageBuffer* im=nullptr,double timestamp=1) {
    auto cloud=view(p,timestamp); Tango3DR_GridIndexArray a{},b{};
    auto sa=Tango3DR_updateFromPointCloud(ram.p,&cloud,&pose,im,im?&pose:nullptr,&a);
    auto sb=Tango3DR_updateFromPointCloud(paged.p,&cloud,&pose,im,im?&pose:nullptr,&b);
    CHECK(sa==sb && sa==0 && a.num_indices==b.num_indices);
    CHECK(!a.num_indices || std::memcmp(a.indices,b.indices,size_t(a.num_indices)*12)==0);
    std::set<Key> keys;
    for(uint32_t i=0;i<a.num_indices;++i) keys.insert(Key{{a.indices[i][0],a.indices[i][1],a.indices[i][2]}});
    Tango3DR_GridIndexArray_destroy(&a); Tango3DR_GridIndexArray_destroy(&b);
    return keys;
}
size_t directoryEntries(const char* path) {
    DIR* d=opendir(path); CHECK(d); size_t n=0;
    while(auto e=readdir(d)) if(std::strcmp(e->d_name,".") && std::strcmp(e->d_name,"..")) ++n;
    closedir(d); return n;
}
void contracts(const char* path) {
    size_t initial=directoryEntries(path);
    Config cfg; defaults(cfg); Context c(cfg);
    CHECK(!stats(c).logical_chunks && stats(c).chunk_bytes==S);
    CHECK(ScannerReconstruction_enablePaging(c.p,path,7*S,128*S,8192)==TANGO_3DR_INVALID);
    CHECK(stats(c).last_failure==SCANNER_RECONSTRUCTION_FAILURE_MEMORY_LIMIT);
    CHECK(ScannerReconstruction_enablePaging(c.p,path,8*S,S-1,8192)==TANGO_3DR_INVALID);
    CHECK(ScannerReconstruction_enablePaging(c.p,path,8*S,128*S,32769)==TANGO_3DR_INVALID);
    CHECK(ScannerReconstruction_enablePaging(c.p,"/nonexistent/scanner-cache-test",8*S,128*S,8192)==TANGO_3DR_ERROR);
    CHECK(stats(c).last_failure==SCANNER_RECONSTRUCTION_FAILURE_CACHE_OPEN);
    CHECK(ReconstructionCore_testPagingFd(c.p)==-1);
    for(long n=0;n<4;++n) {
        Context empty(cfg); ReconstructionCore_testFailAfter(n);
        auto status=ScannerReconstruction_enablePaging(empty.p,path,8*S,128*S,8192);
        ReconstructionCore_testFailAfter(-1);
        CHECK(status==TANGO_3DR_ERROR && !stats(empty).requires_replay);
    }
    auto p=plane(1.03f,.12,.02); auto k=update(c,p); auto before=exact(c,k);
    CHECK(ScannerReconstruction_enablePaging(c.p,path,8*S,128*S,8192)==TANGO_3DR_INVALID);
    CHECK(exact(c,k)==before);
    CHECK(Tango3DR_clear(c.p)==0); enable(c,path);
    CHECK(ScannerReconstruction_enablePaging(c.p,path,8*S,128*S,8192)==TANGO_3DR_INVALID);
    CHECK(directoryEntries(path)==initial);
    int fd=ReconstructionCore_testPagingFd(c.p); CHECK(fd>=0);
    struct stat st{}; CHECK(fstat(fd,&st)==0 && st.st_nlink==0 && (st.st_mode&0777)==0600);
    CHECK(fcntl(fd,F_GETFD)&FD_CLOEXEC);
    ReconstructionCore_testFailAfter(0); auto s=stats(c); ReconstructionCore_testFailAfter(-1);
    CHECK(s.resident_budget_bytes==8*S && s.backing_bytes==0);
    // Scope-specific descriptor cleanup, without asking the test helper to double-destroy.
    auto raw=Tango3DR_ReconstructionContext_create(cfg.p); CHECK(raw);
    CHECK(ScannerReconstruction_enablePaging(raw,path,8*S,128*S,8192)==0);
    int own=ReconstructionCore_testPagingFd(raw); CHECK(own>=0);
    CHECK(Tango3DR_ReconstructionContext_destroy(raw)==0);
    CHECK(fcntl(own,F_GETFD)==-1 && errno==EBADF);
    CHECK(fcntl(fd,F_GETFD)>=0 && directoryEntries(path)==initial);
    std::puts("paging enable / budgets / anonymous 0600 file / descriptor ownership / allocation-free stats: pass");
}
void evictionAndPins(const char* path) {
    Config cfg; defaults(cfg); cfg.boolean("use_space_clearing",true); cfg.integer("max_voxel_weight",4);
    Context ram(cfg),paged(cfg); enable(paged,path,8*S,512*S);
    auto cal=calibration(); CHECK(Tango3DR_ReconstructionContext_setColorCalibration(ram.p,&cal)==0);
    CHECK(Tango3DR_ReconstructionContext_setColorCalibration(paged.p,&cal)==0);
    std::vector<uint8_t> pixels(80*60*3);
    for(size_t i=0;i<pixels.size();++i) pixels[i]=uint8_t(i%251);
    Tango3DR_ImageBuffer image{80,60,240,1,TANGO_3DR_HAL_PIXEL_FORMAT_RGB_888,pixels.data()};
    std::set<Key> keys;
    for(int frame=0;frame<6;++frame) {
        auto p=plane(frame<4?1.27f:1.6f,.75,.03);
        auto camera=identity(); camera.translation[0]=(frame==1?.32:frame==2?-.48:0);
        auto dirty=pairUpdate(ram,paged,p,camera,&image,frame+1); keys.insert(dirty.begin(),dirty.end());
        CHECK(exact(ram,keys)==exact(paged,keys));
        auto s=stats(paged);
        CHECK(s.logical_chunks>8 && s.resident_chunks<=8 && s.peak_resident_chunks<=8);
        CHECK(s.resident_bytes<=8*S && s.peak_resident_bytes<=8*S && s.backing_bytes<=512*S);
    }
    auto s=stats(paged); CHECK(s.reads && s.writes && s.evictions && !s.requires_replay);
    std::printf("paging/revisit/clearing/color + 8-neighbor pins: byte-identical; logical=%llu reads=%llu writes=%llu evictions=%llu\n",
        (unsigned long long)s.logical_chunks,(unsigned long long)s.reads,(unsigned long long)s.writes,(unsigned long long)s.evictions);
}
void failuresAndReuse(const char* path) {
    Config cfg; defaults(cfg); Context ram(cfg),paged(cfg); enable(paged,path,8*S,128*S);
    auto baseline=plane(1.27f,.65,.04); auto camera=identity();
    auto keys=pairUpdate(ram,paged,baseline,camera,nullptr,1);
    auto expected=exact(ram,keys); CHECK(exact(paged,keys)==expected);
    Cloud changed=baseline; for(auto& p:changed) p[2]+=.02f;
    auto cloud=view(changed,2);
    for(int repeat=0;repeat<5;++repeat) {
        auto before=stats(paged);
        ReconstructionCore_testPagingFault(paged.p,1,0);
        Tango3DR_GridIndexArray dirty{};
        CHECK(Tango3DR_updateFromPointCloud(paged.p,&cloud,&camera,nullptr,nullptr,&dirty)==TANGO_3DR_ERROR);
        auto s=stats(paged);
        CHECK(s.last_failure==SCANNER_RECONSTRUCTION_FAILURE_CACHE_WRITE && !s.requires_replay);
        CHECK(!dirty.indices && !dirty.num_indices && s.logical_chunks==before.logical_chunks);
        CHECK(exact(paged,keys)==expected);
        CHECK(stats(paged).backing_bytes<=128*S);
    }
    auto highwater=stats(paged).backing_bytes;
    for(int repeat=0;repeat<8;++repeat) {
        ReconstructionCore_testPagingFault(paged.p,1,0);
        Tango3DR_GridIndexArray dirty{};
        CHECK(Tango3DR_updateFromPointCloud(paged.p,&cloud,&camera,nullptr,nullptr,&dirty)==TANGO_3DR_ERROR);
        CHECK(!stats(paged).requires_replay && exact(paged,keys)==expected);
    }
    CHECK(stats(paged).backing_bytes==highwater); // aborted COW slots are reused
    auto dirty=pairUpdate(ram,paged,changed,camera,nullptr,2); keys.insert(dirty.begin(),dirty.end());
    CHECK(exact(ram,keys)==exact(paged,keys));
    // Fail reads and checksum verification after earlier halo pins were acquired.
    for(int fault:{2,3}) {
        CHECK(exact(paged,keys)==exact(ram,keys)); // ends at positive indices, leaving negative chunks cold
        ReconstructionCore_testPagingFault(paged.p,fault,1);
        bool failed=false;
        for(const auto& k:keys) {
            Mesh mesh;
            auto status=Tango3DR_extractMeshSegment(paged.p,k.data(),&mesh.p);
            if(status!=0) {
                CHECK(status==TANGO_3DR_ERROR && !mesh.p.vertices && !mesh.p.faces);
                auto s=stats(paged); CHECK(s.requires_replay && s.last_failure==SCANNER_RECONSTRUCTION_FAILURE_CACHE_READ);
                failed=true; break;
            }
        }
        CHECK(failed);
        Tango3DR_GridIndexArray indices{};
        CHECK(Tango3DR_updateFromPointCloud(paged.p,&cloud,&camera,nullptr,nullptr,&indices)==TANGO_3DR_ERROR);
        CHECK(!indices.indices && !indices.num_indices && stats(paged).requires_replay);
        // Explicit clear releases every pin/record and permits dataset replay.
        CHECK(Tango3DR_clear(paged.p)==0 && Tango3DR_clear(ram.p)==0);
        CHECK(!stats(paged).requires_replay && !stats(paged).logical_chunks && !stats(paged).resident_chunks);
        CHECK(stats(paged).backing_live_bytes==0);
        keys=pairUpdate(ram,paged,changed,camera,nullptr,2);
        CHECK(exact(ram,keys)==exact(paged,keys));
    }
    std::puts("partial-write strong rollback / bounded slot reuse / read+checksum replay flags / extraction pin unwind: pass");
}
void allocationRollback(const char* path) {
    Config cfg; defaults(cfg); Context c(cfg); enable(c,path,8*S,128*S);
    auto points=plane(1.03f,.25,.03); auto keys=update(c,points); auto before=exact(c,keys);
    for(auto& p:points) p[2]+=.01f;
    auto cloud=view(points,2); auto camera=identity();
    int failures=0;
    for(long n=0;n<256;++n) {
        Tango3DR_GridIndexArray dirty{};
        ReconstructionCore_testFailAfter(n);
        auto status=Tango3DR_updateFromPointCloud(c.p,&cloud,&camera,nullptr,nullptr,&dirty);
        ReconstructionCore_testFailAfter(-1);
        if(status==0) { Tango3DR_GridIndexArray_destroy(&dirty); break; }
        CHECK(status==TANGO_3DR_ERROR && !dirty.indices && !dirty.num_indices);
        CHECK(stats(c).last_failure==SCANNER_RECONSTRUCTION_FAILURE_MEMORY_LIMIT && !stats(c).requires_replay);
        CHECK(exact(c,keys)==before); ++failures;
    }
    CHECK(failures>20 && failures<256 && exact(c,keys)!=before);
    CHECK(stats(c).peak_resident_chunks<=8);
    std::printf("paged allocation sweep: %d transactional failure positions; rollback/retry pass\n",failures);
}
void diskCorruptionAndLoadFailures(const char* path) {
    Config cfg; defaults(cfg);
    for(int mode=0;mode<3;++mode) {
        Context ram(cfg),c(cfg); enable(c,path,8*S,128*S);
        auto p=plane(1.27f,.65,.04); auto camera=identity();
        auto keys=pairUpdate(ram,c,p,camera,nullptr,1);
        auto expected=exact(ram,keys); CHECK(exact(c,keys)==expected);
        auto before=stats(c);
        int fd=ReconstructionCore_testPagingFd(c.p);
        if(mode==0) {
            uint8_t byte=0; CHECK(pread(fd,&byte,1,19)==1); byte^=1;
            CHECK(pwrite(fd,&byte,1,19)==1); // actual backing-byte corruption
        } else if(mode==1) CHECK(ftruncate(fd,0)==0); // actual EOF/truncation
        else {
            // A mid-update load failure must abort COW staging, not publish it.
            ReconstructionCore_testPagingFault(c.p,2,1);
            for(auto& point:p) point[2]+=.02f;
            auto cloud=view(p,2); Tango3DR_GridIndexArray dirty{};
            CHECK(Tango3DR_updateFromPointCloud(c.p,&cloud,&camera,nullptr,nullptr,&dirty)==TANGO_3DR_ERROR);
            CHECK(!dirty.indices && !dirty.num_indices);
        }
        if(mode<2) {
            bool detected=false;
            for(const auto& k:keys) {
                Mesh m;
                if(Tango3DR_extractMeshSegment(c.p,k.data(),&m.p)!=0) {
                    CHECK(!m.p.vertices && !m.p.faces); detected=true; break;
                }
            }
            CHECK(detected);
        }
        auto failed=stats(c);
        CHECK(failed.requires_replay && failed.last_failure==SCANNER_RECONSTRUCTION_FAILURE_CACHE_READ);
        CHECK(failed.logical_chunks==before.logical_chunks && failed.peak_resident_bytes<=8*S);
    }
    // Allocation failure while a cold record is being loaded is recoverable.
    Context c(cfg); enable(c,path,8*S,128*S);
    auto p=plane(1.27f,.65,.04); auto keys=update(c,p); auto expected=exact(c,keys);
    for(long fault:{0L,1L,2L}) {
        exact(c,keys);
        ReconstructionCore_testFailAfter(fault);
        bool failed=false;
        for(const auto& key:keys) {
            Mesh m; auto status=Tango3DR_extractMeshSegment(c.p,key.data(),&m.p);
            if(status!=0) { CHECK(status==TANGO_3DR_ERROR && !m.p.vertices); failed=true; break; }
        }
        ReconstructionCore_testFailAfter(-1);
        CHECK(failed && !stats(c).requires_replay && exact(c,keys)==expected);
    }
    std::puts("actual disk corruption/EOF + mid-update read failure flag replay; cold-load OOM recovers: pass");
}
void backingBudget(const char* path) {
    Config cfg; defaults(cfg); Context c(cfg); enable(c,path,8*S,S,2048);
    Cloud p{{{.25f,.25f,1.03f,1}}}; auto camera=identity();
    for(int i=0;i<9;++i) { camera.translation[0]=i*.64; update(c,p,camera); }
    CHECK(stats(c).logical_chunks==9 && stats(c).backing_bytes==S);
    camera.translation[0]=9*.64; auto cloud=view(p);
    Tango3DR_GridIndexArray dirty{};
    CHECK(Tango3DR_updateFromPointCloud(c.p,&cloud,&camera,nullptr,nullptr,&dirty)==TANGO_3DR_INSUFFICIENT_SPACE);
    auto s=stats(c); CHECK(s.last_failure==SCANNER_RECONSTRUCTION_FAILURE_BACKING_LIMIT && !s.requires_replay);
    CHECK(s.logical_chunks==9 && s.resident_chunks==8 && s.backing_bytes==S && !dirty.indices);
    std::puts("strict backing budget rejects before geometry loss: pass");
}
void growBeyond1024(const char* path) {
    Config cfg; defaults(cfg); cfg.integer("max_chunks",2048);
    Context ram(cfg);
    cfg.integer("max_chunks",1024); // paging extension, not a raised RAM config
    Context paged(cfg); enable(paged,path,8*S,1600*S,4096);
    auto p=plane(1.03f,.05,.01);
    for(auto& point:p) { point[0]+=.25f; point[1]+=.25f; }
    std::set<Key> all;
    for(int i=0;i<1100;++i) {
        auto pose=identity(); pose.translation[0]=(i%33)*.64; pose.translation[1]=(i/33)*.64;
        auto keys=pairUpdate(ram,paged,p,pose,nullptr,i+1); all.insert(keys.begin(),keys.end());
        CHECK(stats(paged).resident_chunks<=8 && stats(paged).peak_resident_chunks<=8);
    }
    CHECK(stats(paged).logical_chunks==1100);
    CHECK(exact(ram,all)==exact(paged,all));
    // Revisit the beginning after >1000 later logical chunks.
    auto dirty=pairUpdate(ram,paged,p,identity(),nullptr,1101); all.insert(dirty.begin(),dirty.end());
    CHECK(exact(ram,all)==exact(paged,all));
    auto s=stats(paged);
    CHECK(s.backing_bytes<=1600*S && s.reads && s.writes && s.evictions);
    std::printf("1100 logical chunks / 8 resident: exact RAM match after revisit; peak=%llu bytes, backing=%llu bytes\n",
        (unsigned long long)s.peak_resident_bytes,(unsigned long long)s.backing_bytes);
}
void pagedLimits(const char* path) {
    Cloud points=plane(1.03f,.25,.03); auto cloud=view(points); auto camera=identity();
    for(int mode=0;mode<3;++mode) {
        Config cfg; defaults(cfg);
        if(mode==1) cfg.integer("max_update_chunks",1);
        if(mode==2) cfg.integer("max_update_work",100);
        Context c(cfg); enable(c,path,8*S,64*S,mode==0?1:8192);
        Tango3DR_GridIndexArray dirty{};
        CHECK(Tango3DR_updateFromPointCloud(c.p,&cloud,&camera,nullptr,nullptr,&dirty)==TANGO_3DR_INSUFFICIENT_SPACE);
        uint32_t expected=mode==0?SCANNER_RECONSTRUCTION_FAILURE_VOLUME_LIMIT:
            mode==1?SCANNER_RECONSTRUCTION_FAILURE_FRAME_LIMIT:SCANNER_RECONSTRUCTION_FAILURE_WORK_LIMIT;
        auto s=stats(c); CHECK(s.last_failure==expected && !s.requires_replay && s.logical_chunks==0);
        CHECK(!dirty.indices && !dirty.num_indices);
    }
    std::puts("paged logical/frame/work limits: named, atomic, non-poisoning failures");
}
int main(int argc,char** argv) {
    CHECK(argc==2); std::setvbuf(stdout,nullptr,_IONBF,0);
    contracts(argv[1]); evictionAndPins(argv[1]); failuresAndReuse(argv[1]);
    allocationRollback(argv[1]); diskCorruptionAndLoadFailures(argv[1]); backingBudget(argv[1]); pagedLimits(argv[1]); growBeyond1024(argv[1]);
    std::puts("ALL PAGING TESTS PASSED"); return 0;
}
