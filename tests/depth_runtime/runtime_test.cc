// SPDX-License-Identifier: Apache-2.0
#define DEPTH_RUNTIME_TEST
#include "../../common/depth/experimental.cc"
#include <cassert>
#include <condition_variable>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <thread>

using namespace oc::depth_test;
namespace {
struct RecordedOperand { int type; float scale; int zero; std::vector<uint32_t> dims; std::vector<uint8_t> value; };
struct Fake {
    int loads = 0, builds = 0, computes = 0, models = 0, compilations = 0, executions = 0;
    int computeError = 0, compileError = 0, outputCode = 255;
    bool support = true, found = true;
    bool compilationTimeoutSymbol = true, executionTimeoutSymbol = true;
    int compilationTimeouts = 0, executionTimeouts = 0;
    int compilationTimeoutError = 0, executionTimeoutError = 0;
    std::vector<RecordedOperand> operands;
    std::vector<std::vector<uint32_t>> operations;
    std::vector<uint8_t> input;
    void* output = nullptr; size_t outputSize = 0;
    std::function<void()> duringCompute, duringBuild;
    void Reset() { assert(!models && !compilations && !executions); *this = Fake{}; }
} fake;
Model* model = reinterpret_cast<Model*>(uintptr_t(1));
Compilation* compilation = reinterpret_cast<Compilation*>(uintptr_t(2));
Execution* execution = reinterpret_cast<Execution*>(uintptr_t(3));
Device* device = reinterpret_cast<Device*>(uintptr_t(4));

void Install() {
    testApi = [](Api& a) {
        ++fake.loads;
        a.getDeviceCount = [](uint32_t* n) { *n = 1; return 0; };
        a.getDevice = [](uint32_t i, Device** d) { assert(i == 0); *d = device; return 0; };
        a.Device_getName = [](const Device*, const char** name) { *name = fake.found ? "google-edgetpu" : "nnapi-reference"; return 0; };
        a.Device_getType = [](const Device*, int32_t* t) { *t = fake.found ? 4 : 2; return 0; };
        a.Model_create = [](Model** m) { ++fake.models; *m = model; fake.operands.clear(); fake.operations.clear(); return 0; };
        a.Model_free = [](Model* m) { assert(m == model); --fake.models; };
        a.Model_addOperand = [](Model*, const Operand* o) {
            RecordedOperand r{o->type, o->scale, o->zeroPoint, {}, {}};
            if (o->dimensionCount) r.dims.assign(o->dimensions, o->dimensions+o->dimensionCount);
            fake.operands.push_back(r); return 0;
        };
        a.Model_setOperandValue = [](Model*, int32_t id, const void* p, size_t size) {
            const uint8_t* b = static_cast<const uint8_t*>(p); fake.operands.at(id).value.assign(b,b+size); return 0;
        };
        a.Model_addOperation = [](Model*, int32_t op, uint32_t n, const uint32_t* in, uint32_t m, const uint32_t*) {
            assert(op == 3 && n == 7 && m == 1); fake.operations.emplace_back(in,in+n); return 0;
        };
        a.Model_identifyInputsAndOutputs = [](Model*, uint32_t n, const uint32_t* in, uint32_t m, const uint32_t*) {
            assert(n == 1 && m == 1 && *in == 0); return 0;
        };
        a.Model_finish = [](Model*) { return 0; };
        a.Model_getSupportedOperationsForDevices = [](const Model*, const Device* const* d, uint32_t n, bool* support) {
            assert(n == 1 && *d == device); for (int i=0;i<3;++i) support[i] = fake.support; return 0;
        };
        a.Compilation_createForDevices = [](Model*, const Device* const* d, uint32_t n, Compilation** c) {
            assert(n == 1 && *d == device); ++fake.compilations; ++fake.builds; *c = compilation; return 0;
        };
        a.Compilation_setPreference = [](Compilation*, int32_t preference) { assert(preference == 1); return 0; };
        if (fake.compilationTimeoutSymbol) a.Compilation_setTimeout = [](Compilation* c, uint64_t ns) {
            assert(c == compilation && ns == 2000000000ULL);
            ++fake.compilationTimeouts; return fake.compilationTimeoutError;
        };
        a.Compilation_finish = [](Compilation*) {
            assert(fake.compilationTimeouts == (fake.compilationTimeoutSymbol ? fake.builds : 0));
            if (fake.duringBuild) fake.duringBuild();
            return fake.compileError;
        };
        a.Compilation_free = [](Compilation* c) { assert(c == compilation); --fake.compilations; };
        a.Execution_create = [](Compilation*, Execution** e) { ++fake.executions; *e = execution; return 0; };
        a.Execution_setInput = [](Execution*, int32_t i, const Operand* o, const void* p, size_t n) {
            assert(!i && !o); auto b = static_cast<const uint8_t*>(p); fake.input.assign(b,b+n); return 0;
        };
        a.Execution_setOutput = [](Execution*, int32_t i, const Operand* o, void* p, size_t n) {
            assert(!i && !o); fake.output = p; fake.outputSize = n; return 0;
        };
        if (fake.executionTimeoutSymbol) a.Execution_setTimeout = [](Execution* e, uint64_t ns) {
            assert(e == execution && ns == 100000000ULL);
            ++fake.executionTimeouts; return fake.executionTimeoutError;
        };
        a.Execution_compute = [](Execution*) {
            ++fake.computes; std::memset(fake.output, fake.outputCode, fake.outputSize);
            assert(fake.executionTimeouts == (fake.executionTimeoutSymbol ? fake.computes : 0));
            if (fake.duringCompute) fake.duringCompute();
            return fake.computeError;
        };
        a.Execution_free = [](Execution* e) { assert(e == execution); --fake.executions; };
    };
}
Frame MakeFrame(Runtime& runtime, int w=13, int h=11) {
    Frame f; f.width=w; f.height=h; f.generation=runtime.Generation();
    f.depth.assign(w*h, 1); f.confidence.assign(w*h, .5f);
    int p=(h/2)*w+w/2; f.depth[p]=1.02f;
    Link l; l.pixel=p; l.point=0; l.depth=f.depth[p];
    l.original=glm::vec4(10,20,30,.75f); l.worldPerMetre=glm::dvec3(2,-3,.5);
    f.links.push_back(l); f.pointCount=2;
    return f;
}
std::vector<glm::vec4> Points(const Frame& f) { return {f.links[0].original, glm::vec4(4,5,6,.25f)}; }
void Unchanged(const std::vector<glm::vec4>& a,const std::vector<glm::vec4>& b) {
    assert(a.size()==b.size()); for(size_t i=0;i<a.size();++i) assert(SamePoint(a[i],b[i]));
}
void GeometryAndGraph() {
    fake.Reset(); Install(); Runtime r; assert(r.Status()=="TPU test: off");
    r.SetEnabled(true); assert(fake.loads==0); auto f=MakeFrame(r); auto points=Points(f); Stats s;
    assert(r.Apply(f,points,s)); assert(s.inferred && s.eligible==1 && s.changed==1 && s.milliseconds>0);
    double delta=Clip((255-DecodeModel().activation[3].zero)*double(DecodeModel().activation[3].scale))*.02;
    assert(delta>0 && delta<=.02);
    for(int i=0;i<3;++i) assert(points[0][i]==float(double(f.links[0].original[i])+f.links[0].worldPerMetre[i]*delta));
    assert(points[0].w==.75f); assert(SamePoint(points[1],Points(f)[1]));
    assert(fake.input.size()==size_t(f.width*f.height*3));
    assert(fake.operands[0].dims==std::vector<uint32_t>({1,11,13,3}));
    assert(fake.operations.size()==3);
    auto params=DecodeModel();
    for(int i=0;i<3;++i) {
        const auto& args=fake.operations[i]; const auto& w=fake.operands[args[1]]; const auto& b=fake.operands[args[2]];
        assert(w.dims==std::vector<uint32_t>({kChannels[i+1],3,3,kChannels[i]}));
        assert(w.scale==params.layer[i].scale && w.zero==128 && w.value==params.layer[i].weights);
        assert(b.type==4 && b.scale==params.layer[i].scale*params.activation[i].scale && b.zero==0);
        assert(b.value.size()==params.layer[i].bias.size()*4);
        assert(!std::memcmp(b.value.data(),params.layer[i].bias.data(),b.value.size()));
        for(int j=3;j<7;++j) { int32_t v; std::memcpy(&v,fake.operands[args[j]].value.data(),4); assert(v==(j==6&&i==2?0:1)); }
    }
    points=Points(f); fake.outputCode=0; assert(r.Apply(f,points,s));
    delta=Clip((0-params.activation[3].zero)*double(params.activation[3].scale))*.02;
    assert(delta<0 && delta>=-.02);
    for(int i=0;i<3;++i) assert(points[0][i]==float(double(f.links[0].original[i])+f.links[0].worldPerMetre[i]*delta));
    assert(fake.builds==1); // Native-shape compilation reuse.
    f=MakeFrame(r,160,120); points=Points(f); assert(r.Apply(f,points,s));
    assert(fake.builds==2 && fake.operands[0].dims==std::vector<uint32_t>({1,120,160,3}));
    assert(r.Status().find("TPU test: active 160x120 1 points ")==0);
}
void RejectionsAndMasks() {
    fake.Reset(); Install(); Runtime r; r.SetEnabled(true); Stats s; auto original=MakeFrame(r);
    auto reject=[&](Frame f, std::vector<glm::vec4> p) { auto before=p; assert(!r.Apply(f,p,s)); Unchanged(before,p); assert(!s.changed); };
    Frame f=original; f.pointCount++; reject(f,Points(original));
    f=original; f.width=1025; reject(f,Points(original));
    f=original; f.width=1024; f.height=1024; reject(f,Points(original));
    f=original; f.depth.pop_back(); reject(f,Points(original));
    f=original; f.confidence.pop_back(); reject(f,Points(original));
    f=original; f.depth[1]=NAN; reject(f,Points(original));
    f=original; f.confidence[1]=1.1f; reject(f,Points(original));
    f=original; f.links[0].point=2; reject(f,Points(original));
    f=original; f.links[0].pixel=f.depth.size(); reject(f,Points(original));
    f=original; f.links[0].depth+=.01f; reject(f,Points(original));
    f=original; f.links[0].worldPerMetre.x=INFINITY; reject(f,Points(original));
    f=original; f.links[0].minimumDepth=NAN; reject(f,Points(original));
    f=original; f.minDepth=f.maxDepth; reject(f,Points(original));
    f=original; f.worldToCamera[0][1]=INFINITY; reject(f,Points(original));
    f=original; f.links.push_back(f.links[0]); reject(f,Points(original));
    auto p=Points(original); p[0].x=std::nextafter(p[0].x,INFINITY); reject(original,p);
    assert(fake.loads==0);
    auto retain=[&](Frame f) { auto p=Points(f), before=p; assert(r.Apply(f,p,s)); Unchanged(before,p); assert(!s.changed); };
    f=original; std::fill(f.depth.begin(),f.depth.end(),1); f.links[0].depth=1; retain(f); // flat
    f=original; f.depth[f.links[0].pixel-1]=0; retain(f); // incomplete 3x3
    f=original; f.depth[f.links[0].pixel-1]=1.2f; retain(f); // edge
    f=original; f.depth[f.links[0].pixel]=0; f.links[0].depth=0; retain(f); // hole
    f=original; f.links[0].pixel=3*f.width+4; f.depth[f.links[0].pixel]=f.links[0].depth; retain(f); // border
    f=original; f.links[0].original.w=0; retain(f); // invalid original
    f=original; f.links[0].original.x=2000000; retain(f); // out-of-range world
    f=original; std::fill(f.depth.begin(),f.depth.end(),.04f); f.depth[f.links[0].pixel]=.06f;
    f.links[0].depth=.06f; fake.outputCode=0; retain(f); // corrected depth <= .05
    f=original; f.links[0].original.x=1000000; fake.outputCode=255;
    f.links[0].worldPerMetre.x=100; retain(f); // corrected world out of range
    f=original; f.links[0].minimumDepth=1.03f; fake.outputCode=0; retain(f); // producer depth offset
    f=original; f.maxDepth=29.995; retain(f); // outside -> inside selected camera range
    f=original; f.minDepth=29.995; retain(f); // corrected point crosses selected camera range
    f=original; f.maxDepth=30.005; fake.outputCode=255; retain(f);
    f=original; f.links.clear(); p=Points(original); auto before=p; assert(r.Apply(f,p,s)); Unchanged(before,p);
}
void AdmissionAndOffsetGuards() {
    fake.Reset(); Install(); Runtime r; r.SetEnabled(true); Stats s;
    auto original=MakeFrame(r);
    auto check=[&](Frame f, bool changed) {
        auto p=Points(f), before=p;
        assert(r.Apply(f,p,s) && s.inferred && s.eligible==1);
        assert(s.changed==uint32_t(changed));
        assert(p.size()==before.size() && p[0].w==before[0].w && SamePoint(p[1],before[1]));
        assert(SamePoint(p[0],before[0])!=changed);
    };
    Frame f=original; f.minDepth=29; f.maxDepth=31; check(f,true); // admitted -> admitted
    f=original; f.maxDepth=29; check(f,true); // above range -> above range
    f=original; f.minDepth=31; f.maxDepth=32; check(f,true); // below range -> below range
    f=original; f.worldToCamera[2][2]=-1; check(f,true); // behind -> behind
    f=original; f.maxDepth=30.005; check(f,false); // admitted -> above
    f=original; f.minDepth=30.005; f.maxDepth=31; check(f,false); // below -> admitted
    f=original; f.worldToCamera[3][2]=-29; f.maxDepth=1.005; check(f,false); // w must be 1, not .75
    f=original; f.links[0].original.z=-.005f; check(f,false); // behind -> in front
    fake.outputCode=0;
    f=original; f.maxDepth=29.995; check(f,false); // above -> admitted
    f=original; f.minDepth=29.995; check(f,false); // admitted -> below
    f=original; f.links[0].minimumDepth=1.03f;
    auto p=Points(f), before=p;
    Link second=f.links[0]; second.point=1; second.pixel++;
    second.depth=f.depth[second.pixel]; second.original=p[1]; second.minimumDepth=.05f;
    f.links.push_back(second);
    assert(r.Apply(f,p,s) && s.eligible==2 && s.changed==1);
    assert(SamePoint(p[0],before[0]) && !SamePoint(p[1],before[1]));
    assert(p.size()==before.size() && p[1].w==before[1].w); // skip individually, no drops
}
void Fallbacks() {
    fake.Reset(); Install();
    auto fallback=[&](const std::string& reason) {
        Runtime r; r.SetEnabled(true); auto f=MakeFrame(r); auto p=Points(f), before=p; Stats s;
        assert(!r.Apply(f,p,s)); Unchanged(p,before); assert(r.Status().find(reason)!=std::string::npos);
        int loads=fake.loads; assert(!r.Apply(f,p,s)); assert(fake.loads==loads); // failed shape cached
    };
    fake.found=false; fallback("google-edgetpu unavailable"); fake.found=true;
    fake.support=false; fallback("unsupported graph"); fake.support=true;
    fake.compileError=5; fallback("compilation finish error 5"); fake.compileError=0;
    fake.computeError=5; fallback("TPU compute error 5"); fake.computeError=0;
    testApi=[](Api&) { throw std::runtime_error("NNAPI library unavailable"); }; fallback("NNAPI library unavailable");
    testApi={}; fallback("non-Android host");
    Runtime r; r.SetEnabled(true); r.Fallback("sidecar I/O failed");
    assert(r.Status()=="TPU test: original depth (sidecar I/O failed)");
    r.SetEnabled(false); r.Fallback("ignored"); assert(r.Status()=="TPU test: off");
}
void Deadlines() {
    for (int symbols=0; symbols<4; ++symbols) {
        fake.Reset(); Install();
        fake.compilationTimeoutSymbol=(symbols&1)!=0;
        fake.executionTimeoutSymbol=(symbols&2)!=0;
        Runtime r; r.SetEnabled(true); auto f=MakeFrame(r); auto p=Points(f); Stats s;
        assert(fake.loads==0 && !fake.compilationTimeouts && !fake.executionTimeouts);
        assert(r.Apply(f,p,s)); p=Points(f); assert(r.Apply(f,p,s));
        assert(fake.compilationTimeouts==(fake.compilationTimeoutSymbol?1:0));
        assert(fake.executionTimeouts==(fake.executionTimeoutSymbol?2:0));
    }
    for (int status : {10,11}) for (bool compiling : {false,true}) {
        fake.Reset(); Install();
        if(compiling) fake.compileError=status; else fake.computeError=status;
        Runtime r; r.SetEnabled(true); auto f=MakeFrame(r); auto p=Points(f), before=p; Stats s;
        assert(!r.Apply(f,p,s)); Unchanged(p,before); assert(!s.changed && !s.inferred);
        std::string expected=std::string("TPU test: original depth (")+
            (compiling?"TPU compilation finish":"TPU compute")+" timeout ("+
            (status==10?"transient":"persistent")+"))";
        assert(r.Status()==expected);
        if(compiling) {
            assert(!fake.models && !fake.compilations && !fake.computes);
            assert(!r.Apply(f,p,s)); assert(fake.builds==1); // failed compilation cached
        } else {
            assert(!fake.executions && fake.computes==1);
            fake.computeError=0; assert(r.Apply(f,p,s)); assert(fake.builds==1); // fresh execution can recover
        }
    }
    for (bool compiling : {false,true}) {
        fake.Reset(); Install();
        if(compiling) fake.compilationTimeoutError=4; else fake.executionTimeoutError=4;
        Runtime r; r.SetEnabled(true); auto f=MakeFrame(r); auto p=Points(f), before=p; Stats s;
        assert(!r.Apply(f,p,s)); Unchanged(p,before); assert(!fake.computes && !fake.executions);
        assert(r.Status().find(compiling?"TPU compilation deadline error 4":"TPU execution deadline error 4")!=std::string::npos);
    }
}
void Generations() {
    fake.Reset(); Install(); Runtime r; r.SetEnabled(true); auto f=MakeFrame(r); auto p=Points(f), before=p; Stats s;
    r.Invalidate(); assert(!r.Apply(f,p,s)); assert(fake.loads==0); Unchanged(p,before);
    f.generation=r.Generation(); fake.duringBuild=[&] { r.Invalidate(); };
    assert(!r.Apply(f,p,s)); assert(fake.computes==0); Unchanged(p,before); fake.duringBuild={};
    f.generation=r.Generation(); fake.duringCompute=[&] { r.Invalidate(); };
    assert(!r.Apply(f,p,s)); assert(s.inferred && !s.changed); Unchanged(p,before); fake.duringCompute={};
    f.generation=r.Generation(); testBeforeCommit=[&] { r.SetEnabled(false); r.SetEnabled(true); };
    assert(!r.Apply(f,p,s)); Unchanged(p,before); assert(!s.changed); testBeforeCommit={};
    f.generation=r.Generation(); assert(r.Apply(f,p,s)); assert(s.changed==1);
    p=Points(f); fake.duringCompute=[&] { p[0].x+=1; };
    auto replaced=p; replaced[0].x+=1;
    assert(!r.Apply(f,p,s)); Unchanged(p,replaced); assert(!s.changed);
    fake.duringCompute={};
}
void NonblockingControls() {
    fake.Reset(); Install(); Runtime r; r.SetEnabled(true); auto f=MakeFrame(r); auto p=Points(f), before=p; Stats s;
    std::mutex mutex; std::condition_variable cv; bool entered=false, release=false;
    fake.duringCompute=[&] {
        std::unique_lock<std::mutex> lock(mutex); entered=true; cv.notify_all(); cv.wait(lock,[&] { return release; });
    };
    auto worker=std::async(std::launch::async,[&] { return r.Apply(f,p,s); });
    { std::unique_lock<std::mutex> lock(mutex); assert(cv.wait_for(lock,std::chrono::seconds(5),[&] { return entered; })); }
    auto controls=std::async(std::launch::async,[&] {
        r.SetEnabled(false); r.Invalidate(); assert(!r.Enabled()); assert(r.Status()=="TPU test: off");
    });
    assert(controls.wait_for(std::chrono::milliseconds(200))==std::future_status::ready); controls.get();
    { std::lock_guard<std::mutex> lock(mutex); release=true; cv.notify_all(); }
    assert(!worker.get()); Unchanged(before,p); fake.duringCompute={};
}
void SimplePreprocessing() {
    assert(Quantize(0)==128 && Quantize(1)==255 && Quantize(-1)==0);
    assert(Quantize(.5f/128)==128 && Quantize(1.5f/128)==130);
    assert(Quantize(-.5f/128)==128 && Quantize(-1.5f/128)==126);
    Runtime r; auto f=MakeFrame(r,11,11); std::fill(f.depth.begin(),f.depth.end(),1);
    f.depth[60]=1.02f; auto a=Preprocess(f);
    assert(a.input[60*3]==191 && a.input[60*3+1]==138 && a.input[60*3+2]==192);
    assert(a.eligible[60]); assert(!a.eligible[0]);
    std::fill(f.depth.begin(),f.depth.end(),0); a=Preprocess(f);
    for(size_t i=0;i<f.depth.size();++i) assert(a.input[i*3]==128 && a.input[i*3+1]==128 && !a.eligible[i]);
}
} // namespace
int main(int argc,char** argv) {
    if(argc==4 && std::string(argv[1])=="features") {
        Frame f; std::ifstream input(argv[2],std::ios::binary);
        input.read(reinterpret_cast<char*>(&f.width),4); input.read(reinterpret_cast<char*>(&f.height),4);
        assert(f.width>0 && f.height>0 && size_t(f.width)*f.height<=262144);
        f.depth.resize(f.width*f.height); f.confidence.resize(f.depth.size());
        input.read(reinterpret_cast<char*>(f.depth.data()),f.depth.size()*4);
        input.read(reinterpret_cast<char*>(f.confidence.data()),f.confidence.size()*4); assert(input.good());
        auto a=Preprocess(f); std::ofstream output(argv[3],std::ios::binary);
        output.write(reinterpret_cast<const char*>(a.input.data()),a.input.size());
        output.write(reinterpret_cast<const char*>(a.eligible.data()),a.eligible.size()); return output.good()?0:1;
    }
    GeometryAndGraph(); RejectionsAndMasks(); AdmissionAndOffsetGuards(); Fallbacks(); Deadlines(); Generations(); NonblockingControls(); SimplePreprocessing();
    fake.Reset(); std::cout<<"depth_runtime: all contract tests passed\n";
}
