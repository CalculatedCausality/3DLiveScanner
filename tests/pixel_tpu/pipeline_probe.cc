// SPDX-License-Identifier: Apache-2.0
// Generated-only full-path costs: no scanner process, camera or datasets.
// Use the real optional NNAPI loader/runtime; explicitly select google-edgetpu.
#include "../../common/depth/experimental.cc"
#include <array>
#include <numeric>

using namespace oc::depth_test;
using namespace oc::depth_test::detail;
static double median(std::vector<double> values) {
    std::sort(values.begin(),values.end());return values[values.size()/2];
}
static void metric(const char* name,const std::vector<double>& values) {
    auto sorted=values;std::sort(sorted.begin(),sorted.end());
    std::printf("\"%s\":{\"median_ms\":%.6f,\"p95_ms\":%.6f}",name,median(values),sorted[size_t(.95*(sorted.size()-1))]);
}

static void depthPipeline(int width,int height) {
    Runtime runtime;runtime.SetEnabled(true);
    Frame frame;frame.width=width;frame.height=height;frame.generation=runtime.Generation();
    frame.worldToCamera[1][1]=-1;frame.worldToCamera[2][2]=-1;
    frame.depth.resize(size_t(width)*height);frame.confidence.assign(frame.depth.size(),.85f);
    std::vector<glm::vec4> original;
    for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
        size_t p=size_t(y)*width+x;
        frame.depth[p]=2.f+.008f*std::sin(float(x*17+y*13));
        Link link;link.pixel=p;link.point=p;link.depth=frame.depth[p];
        link.worldPerMetre=glm::dvec3((x-width*.5)/(width*.9),(y-height*.5)/(width*.9),-1.);
        link.original=glm::vec4(glm::vec3(link.worldPerMetre*double(link.depth)),.85f);
        frame.links.push_back(link);original.push_back(link.original);
    }
    frame.pointCount=original.size();
    auto start=Clock::now();Backend backend(width,height);backend.Build();double compile=Elapsed(start);
    auto features=Preprocess(frame);std::vector<uint8_t> output(frame.depth.size());
    start=Clock::now();backend.Infer(features.input,output);double first=Elapsed(start);
    {auto points=original;Stats stats;Require(runtime.Apply(frame,points,stats),"runtime warm-up failed");}
    std::vector<double> prepare,infer,total;
    uint32_t changed=0;
    for(int i=0;i<23;++i) {
        start=Clock::now();auto current=Preprocess(frame);double a=Elapsed(start);
        Require(current.input==features.input&&current.eligible==features.eligible,"nondeterministic features");
        start=Clock::now();backend.Infer(current.input,output);double b=Elapsed(start);
        auto points=original;Stats stats;start=Clock::now();
        Require(runtime.Apply(frame,points,stats)&&stats.inferred,"runtime inference failed");
        double c=Elapsed(start);changed=stats.changed;
        for(size_t j=0;j<points.size();++j)
            Require(points[j].w==original[j].w&&std::isfinite(points[j].z)&&std::abs(points[j].z-original[j].z)<=.020001,"runtime output guard");
        if(i>=3) {prepare.push_back(a);infer.push_back(b);total.push_back(c);}
    }
    std::printf("{\"kind\":\"existing_depth_runtime\",\"width\":%d,\"height\":%d,\"compile_ms\":%.6f,\"first_execution_ms\":%.6f,\"changed\":%u,",width,height,compile,first,changed);
    metric("cpu_preprocess",prepare);std::printf(",");metric("tpu_execution_including_io_binding",infer);
    std::printf(",");metric("complete_runtime_apply",total);
    std::printf(",\"excludes_camera_backup_fusion_render\":true}\n");std::fflush(stdout);
}

struct Voxel {float sdf,weight;};
static const int corners[8]={0,1,17,18,289,290,306,307};
static const int tetrahedra[6][4]={{0,1,3,7},{0,3,2,7},{0,2,6,7},{0,6,4,7},{0,4,5,7},{0,5,1,7}};
static const unsigned masks[6]={139,141,197,209,177,163};
static void cpuMasks(const std::vector<Voxel>& voxels,int blocks,std::vector<uint8_t>& result) {
    size_t out=0;
    for(int b=0;b<blocks;++b)for(int z=0;z<16;++z)for(int y=0;y<16;++y)for(int x=0;x<16;++x) {
        const Voxel* cell=&voxels[size_t(b)*4913+x+17*y+289*z];
        unsigned observed=0,negative=0;
        for(int j=0;j<8;++j) {
            if(cell[corners[j]].weight>=1)observed|=1u<<j;
            if(cell[corners[j]].sdf<0)negative|=1u<<j;
        }
        for(unsigned mask:masks)result[out++]=((observed&mask)==mask&&(negative&mask)!=0&&(negative&mask)!=mask);
    }
}
static void packMasks(const std::vector<Voxel>& voxels,int blocks,std::vector<uint8_t>& input) {
    size_t out=0;
    for(int b=0;b<blocks;++b)for(int z=0;z<16;++z)for(int y=0;y<16;++y)for(int x=0;x<16;++x) {
        const Voxel* cell=&voxels[size_t(b)*4913+x+17*y+289*z];
        for(const auto& t:tetrahedra) {
            for(int j=0;j<4;++j)input[out+j]=cell[corners[t[j]]].weight>=1;
            for(int j=0;j<4;++j)input[out+4+j]=cell[corners[t[j]]].sdf<0;
            out+=8;
        }
    }
}
static void meshTensor(int blocks) {
    Api api;api.Load();Device* selected=nullptr;uint32_t count=0;
    Check(api.getDeviceCount(&count),"devices");Require(count<=128,"device count");
    for(uint32_t i=0;i<count;++i) {
        Device* d=nullptr;const char* name=nullptr;int32_t type=0;
        Check(api.getDevice(i,&d),"device");Check(api.Device_getName(d,&name),"name");Check(api.Device_getType(d,&type),"type");
        if(name&&std::strcmp(name,"google-edgetpu")==0&&type==4)selected=d;
    }
    Require(selected,"google-edgetpu missing");
    Model* model=nullptr;Compilation* compilation=nullptr;
    struct Cleanup {Api& api;Model*& m;Compilation*& c;~Cleanup(){if(c)api.Compilation_free(c);if(m)api.Model_free(m);}} cleanup{api,model,compilation};
    Check(api.Model_create(&model),"create");uint32_t next=0;
    auto operand=[&](int32_t type,uint32_t rank,const uint32_t* shape,float scale,int32_t zero) {
        uint32_t id=next++;Operand op={type,rank,shape,scale,zero};Check(api.Model_addOperand(model,&op),"operand");return id;
    };
    auto scalar=[&](int32_t value){auto id=operand(1,0,nullptr,0,0);Check(api.Model_setOperandValue(model,id,&value,4),"scalar");return id;};
    uint32_t inShape[]={1,uint32_t(blocks*16),1536,8},outShape[]={1,uint32_t(blocks*16),1536,2};
    auto inputId=operand(5,4,inShape,1,0),outputId=operand(5,4,outShape,1,0);
    uint32_t weightShape[]={2,1,1,8},biasShape[]={2};
    auto weightId=operand(5,4,weightShape,.25f,128),biasId=operand(4,1,biasShape,.25f,0);
    uint8_t weights[16];std::fill(weights,weights+16,128);
    for(int j=0;j<4;++j){weights[j]=132;weights[8+4+j]=132;}
    const int32_t biases[2]={0,0};
    Check(api.Model_setOperandValue(model,weightId,weights,sizeof(weights)),"weights");
    Check(api.Model_setOperandValue(model,biasId,biases,sizeof(biases)),"bias");
    uint32_t args[]={inputId,weightId,biasId,scalar(1),scalar(1),scalar(1),scalar(0)};
    Check(api.Model_addOperation(model,3,7,args,1,&outputId),"conv");
    Check(api.Model_identifyInputsAndOutputs(model,1,&inputId,1,&outputId),"IO");Check(api.Model_finish(model),"finish");
    const Device* devices[]={selected};bool supported=false;
    Check(api.Model_getSupportedOperationsForDevices(model,devices,1,&supported),"support");Require(supported,"mask graph unsupported");
    auto start=Clock::now();Check(api.Compilation_createForDevices(model,devices,1,&compilation),"compile");
    Check(api.Compilation_setPreference(compilation,1),"preference");Check(api.Compilation_finish(compilation),"compile finish");double compile=Elapsed(start);
    const size_t size=size_t(blocks)*4096*6;
    std::vector<Voxel> voxels(size_t(blocks)*4913);
    uint32_t seed=613853;
    for(auto& v:voxels){seed=seed*1664525u+1013904223u;v.sdf=(seed&1)?-.5f:.5f;v.weight=((seed>>8)%5)?2.f:0.f;}
    std::vector<uint8_t> input(size*8),output(size*2),expected(size),actual(size);
    std::vector<double> cpu,packing,execution,decoding,total;
    for(int iteration=0;iteration<23;++iteration) {
        start=Clock::now();cpuMasks(voxels,blocks,expected);double c=Elapsed(start);
        const auto begin=Clock::now();packMasks(voxels,blocks,input);double p=Elapsed(begin);
        start=Clock::now();Execution* e=nullptr;Check(api.Execution_create(compilation,&e),"execute");
        struct FreeExecution{Api& api;Execution* e;~FreeExecution(){api.Execution_free(e);}} free{api,e};
        Check(api.Execution_setInput(e,0,nullptr,input.data(),input.size()),"input");
        Check(api.Execution_setOutput(e,0,nullptr,output.data(),output.size()),"output");Check(api.Execution_compute(e),"compute");
        double invoke=Elapsed(start);start=Clock::now();
        for(size_t i=0;i<size;++i)actual[i]=output[2*i]==4&&output[2*i+1]>0&&output[2*i+1]<4;
        double decode=Elapsed(start),complete=Elapsed(begin);
        Require(actual==expected,"TPU/CPU mask mismatch");
        if(iteration>=3){cpu.push_back(c);packing.push_back(p);execution.push_back(invoke);decoding.push_back(decode);total.push_back(complete);}
    }
    std::printf("{\"kind\":\"mesh_tetrahedron_eligibility\",\"blocks\":%d,\"tetrahedra\":%zu,\"input_bytes\":%zu,\"compile_ms\":%.6f,\"all_masks_identical\":true,",blocks,size,input.size(),compile);
    metric("cpu_direct",cpu);std::printf(",");metric("cpu_pack",packing);std::printf(",");metric("tpu_execute",execution);
    std::printf(",");metric("cpu_decode",decoding);std::printf(",");metric("full_offload",total);
    std::printf(",\"excludes_paging_and_triangle_generation\":true}\n");std::fflush(stdout);
}
int main() {
    try {depthPipeline(160,90);depthPipeline(320,180);meshTensor(1);meshTensor(8);}
    catch(const std::exception& e){std::fprintf(stderr,"Probe failed: %s\n",e.what());return 2;}
}
