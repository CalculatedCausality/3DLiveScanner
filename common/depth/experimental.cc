// SPDX-License-Identifier: Apache-2.0
#include "experimental.h"
#include "model_data.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <stdexcept>
#include <utility>
#ifdef __ANDROID__
#include <android/log.h>
#endif
#ifdef DEPTH_RUNTIME_TEST
#include <functional>
#endif

namespace oc { namespace depth_test {
namespace detail {
constexpr size_t kMaxPixels = 262144;
constexpr double kWorldLimit = 1000000.;
using Clock = std::chrono::steady_clock;
double Elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
void Log(const std::string& message) {
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "DepthTPU", "EXPERIMENTAL %s", message.c_str());
#else
    std::fprintf(stderr, "EXPERIMENTAL %s\n", message.c_str());
#endif
}
void Require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }

struct Activation { float scale; int32_t zero; };
struct Layer { float scale; std::vector<uint8_t> weights; std::vector<int32_t> bias; };
struct Parameters { Activation activation[4]; Layer layer[3]; };
constexpr uint32_t kChannels[] = {3, 12, 12, 1};
Parameters DecodeModel() {
    static_assert(sizeof(kModelData) == 1880, "DCLN0001 size");
    Require(std::memcmp(kModelData, "DCLN0001", 8) == 0, "model magic");
    const uint8_t* p = kModelData + 8;
    auto bits = [&]() { uint32_t v = uint32_t(p[0]) | uint32_t(p[1]) << 8 |
        uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; p += 4; return v; };
    auto integer = [&]() { uint32_t v = bits(); int32_t i; std::memcpy(&i, &v, 4); return i; };
    auto scale = [&]() { uint32_t v = bits(); float f; std::memcpy(&f, &v, 4);
        Require(std::isfinite(f) && f > 0, "model scale"); return f; };
    Parameters result;
    for (auto& a : result.activation) {
        a.scale = scale(); a.zero = integer();
        Require(a.zero >= 0 && a.zero <= 255, "model zero point");
    }
    Require(result.activation[0].scale == 1.f/128 && result.activation[0].zero == 128,
            "model input contract");
    for (int i = 0; i < 3; ++i) {
        auto& l = result.layer[i]; l.scale = scale();
        float bs = l.scale * result.activation[i].scale;
        Require(std::isfinite(bs) && bs > 0, "model bias scale");
        size_t count = kChannels[i+1] * 9 * kChannels[i];
        l.weights.assign(p, p + count); p += count;
        for (uint32_t j = 0; j < kChannels[i+1]; ++j) l.bias.push_back(integer());
    }
    Require(p == kModelData + sizeof(kModelData), "model size");
    return result;
}

// Stable NNAPI C ABI subset. Deliberately no direct NNAPI symbol references:
// API24 builds must load on devices predating NNAPI (API27/device selection API29).
struct Model; struct Compilation; struct Execution; struct Device;
struct Operand { int32_t type; uint32_t dimensionCount; const uint32_t* dimensions;
    float scale; int32_t zeroPoint; };
#define NN_FUNCTIONS(X) \
    X(int, getDeviceCount, (uint32_t*)) \
    X(int, getDevice, (uint32_t, Device**)) \
    X(int, Device_getName, (const Device*, const char**)) \
    X(int, Device_getType, (const Device*, int32_t*)) \
    X(int, Model_create, (Model**)) \
    X(void, Model_free, (Model*)) \
    X(int, Model_addOperand, (Model*, const Operand*)) \
    X(int, Model_setOperandValue, (Model*, int32_t, const void*, size_t)) \
    X(int, Model_addOperation, (Model*, int32_t, uint32_t, const uint32_t*, uint32_t, const uint32_t*)) \
    X(int, Model_identifyInputsAndOutputs, (Model*, uint32_t, const uint32_t*, uint32_t, const uint32_t*)) \
    X(int, Model_finish, (Model*)) \
    X(int, Model_getSupportedOperationsForDevices, (const Model*, const Device* const*, uint32_t, bool*)) \
    X(int, Compilation_createForDevices, (Model*, const Device* const*, uint32_t, Compilation**)) \
    X(int, Compilation_setPreference, (Compilation*, int32_t)) \
    X(int, Compilation_finish, (Compilation*)) \
    X(void, Compilation_free, (Compilation*)) \
    X(int, Execution_create, (Compilation*, Execution**)) \
    X(int, Execution_setInput, (Execution*, int32_t, const Operand*, const void*, size_t)) \
    X(int, Execution_setOutput, (Execution*, int32_t, const Operand*, void*, size_t)) \
    X(int, Execution_compute, (Execution*)) \
    X(void, Execution_free, (Execution*))
struct Api {
#define DECLARE(ret, name, args) ret (*name) args = nullptr;
    NN_FUNCTIONS(DECLARE)
#undef DECLARE
    // Optional API30 hints; absence must not make older NNAPI unavailable.
    int (*Compilation_setTimeout)(Compilation*, uint64_t) = nullptr;
    int (*Execution_setTimeout)(Execution*, uint64_t) = nullptr;
    // Borrowed process-lifetime mapping. NNAPI/vendor service threads may
    // outlive a compilation; unloading their library during shape replacement
    // or runtime destruction can leave those threads executing unmapped code.
    void* library = nullptr;
    void Load();
};
#ifdef DEPTH_RUNTIME_TEST
// Translation-unit-private seams, never present in app builds or public header.
std::function<void(Api&)> testApi;
std::function<void()> testBeforeCommit;
#endif
void Api::Load() {
#ifdef DEPTH_RUNTIME_TEST
    if (testApi) { testApi(*this); return; }
#endif
#ifdef __ANDROID__
    static std::mutex libraryMutex;
    static void* processLibrary = nullptr;
    {
        std::lock_guard<std::mutex> lock(libraryMutex);
        if (!processLibrary) processLibrary = dlopen("libneuralnetworks.so", RTLD_NOW | RTLD_LOCAL);
        library = processLibrary;
    }
    Require(library != nullptr, "NNAPI library unavailable");
#define LOAD(ret, name, args) name = reinterpret_cast<decltype(name)>(dlsym(library, \
    (std::string("ANeuralNetworks") + (#name[0] == 'g' ? "_" : "") + #name).c_str())); \
    Require(name != nullptr, "NNAPI symbol unavailable: " #name);
    NN_FUNCTIONS(LOAD)
#undef LOAD
    Compilation_setTimeout = reinterpret_cast<decltype(Compilation_setTimeout)>(
        dlsym(library, "ANeuralNetworksCompilation_setTimeout"));
    Execution_setTimeout = reinterpret_cast<decltype(Execution_setTimeout)>(
        dlsym(library, "ANeuralNetworksExecution_setTimeout"));
#else
    throw std::runtime_error("NNAPI unavailable on non-Android host");
#endif
}
#undef NN_FUNCTIONS
void Check(int status, const char* stage) {
    // NNAPI MISSED_DEADLINE_TRANSIENT/PERSISTENT. Both retain original points.
    if (status == 10 || status == 11)
        throw std::runtime_error(std::string(stage) + " timeout (" +
                                 (status == 10 ? "transient" : "persistent") + ")");
    if (status) throw std::runtime_error(std::string(stage) + " error " + std::to_string(status));
}
struct Backend {
    Api api;
    Parameters parameters;
    Model* model = nullptr;
    Compilation* compilation = nullptr;
    int width, height;
    Backend(int w, int h) : width(w), height(h) {}
    ~Backend() {
        if (compilation) api.Compilation_free(compilation);
        if (model) api.Model_free(model);
    }
    void Build() {
        parameters = DecodeModel();
        api.Load();
        uint32_t count = 0;
        Check(api.getDeviceCount(&count), "device count");
        Require(count <= 128, "invalid device count");
        Device* selected = nullptr;
        for (uint32_t i = 0; i < count; ++i) {
            Device* device = nullptr; const char* name = nullptr; int32_t type = 0;
            Check(api.getDevice(i, &device), "device");
            Require(device != nullptr, "null device");
            Check(api.Device_getName(device, &name), "device name");
            Check(api.Device_getType(device, &type), "device type");
            if (name && std::strcmp(name, "google-edgetpu") == 0 && type == 4) selected = device;
        }
        Require(selected != nullptr, "google-edgetpu unavailable");
        Check(api.Model_create(&model), "model create");
        Require(model != nullptr, "null model");
        uint32_t next = 0;
        auto operand = [&](int32_t type, uint32_t rank, const uint32_t* dims, float scale, int32_t zero) {
            Operand t = {type, rank, dims, scale, zero}; uint32_t id = next++;
            Check(api.Model_addOperand(model, &t), "operand"); return id;
        };
        auto scalar = [&](int32_t value) {
            uint32_t id = operand(1, 0, nullptr, 0, 0); // INT32
            Check(api.Model_setOperandValue(model, id, &value, sizeof(value)), "scalar"); return id;
        };
        auto activation = [&](int layer) {
            uint32_t dims[] = {1, uint32_t(height), uint32_t(width), kChannels[layer]};
            auto a = parameters.activation[layer]; return operand(5, 4, dims, a.scale, a.zero);
        };
        uint32_t x = activation(0), input = x;
        for (int i = 0; i < 3; ++i) {
            auto& l = parameters.layer[i];
            uint32_t dims[] = {kChannels[i+1], 3, 3, kChannels[i]};
            uint32_t weights = operand(5, 4, dims, l.scale, 128); // QUANT8_ASYMM OHWI
            Check(api.Model_setOperandValue(model, weights, l.weights.data(), l.weights.size()), "weights");
            uint32_t bias = operand(4, 1, dims, parameters.activation[i].scale*l.scale, 0);
            Check(api.Model_setOperandValue(model, bias, l.bias.data(), l.bias.size()*4), "bias");
            uint32_t out = activation(i+1);
            uint32_t args[] = {x, weights, bias, scalar(1), scalar(1), scalar(1), scalar(i == 2 ? 0 : 1)};
            Check(api.Model_addOperation(model, 3, 7, args, 1, &out), "CONV_2D");
            x = out;
        }
        Check(api.Model_identifyInputsAndOutputs(model, 1, &input, 1, &x), "model IO");
        Check(api.Model_finish(model), "model finish");
        const Device* devices[] = {selected}; bool supported[3] = {};
        Check(api.Model_getSupportedOperationsForDevices(model, devices, 1, supported), "support query");
        Require(supported[0] && supported[1] && supported[2], "google-edgetpu unsupported graph");
        Check(api.Compilation_createForDevices(model, devices, 1, &compilation), "TPU compilation create");
        Require(compilation != nullptr, "null compilation");
        Check(api.Compilation_setPreference(compilation, 1), "compilation preference");
        if (api.Compilation_setTimeout)
            Check(api.Compilation_setTimeout(compilation, uint64_t(2000000000)), "TPU compilation deadline");
        Check(api.Compilation_finish(compilation), "TPU compilation finish");
        Log("google-edgetpu active native " + std::to_string(width) + "x" + std::to_string(height));
    }
    void Infer(const std::vector<uint8_t>& input, std::vector<uint8_t>& output) {
        Execution* execution = nullptr;
        struct Cleanup { Api& api; Execution*& e; ~Cleanup() { if (e) api.Execution_free(e); } } cleanup{api, execution};
        Check(api.Execution_create(compilation, &execution), "execution create");
        Require(execution != nullptr, "null execution");
        Check(api.Execution_setInput(execution, 0, nullptr, input.data(), input.size()), "input");
        Check(api.Execution_setOutput(execution, 0, nullptr, output.data(), output.size()), "output");
        if (api.Execution_setTimeout)
            Check(api.Execution_setTimeout(execution, uint64_t(100000000)), "TPU execution deadline");
        Check(api.Execution_compute(execution), "TPU compute");
    }
};

uint8_t Quantize(float feature) {
    // Explicit ties-to-even, independent of the caller's floating point rounding mode.
    double scaled = double(feature) * 128.;
    double low = std::floor(scaled), fraction = scaled - low;
    int value = int(low);
    if (fraction > .5 || (fraction == .5 && value % 2 != 0)) ++value;
    return uint8_t(std::max(0, std::min(255, value + 128)));
}
float Clip(double value) { return float(std::max(-1., std::min(1., value))); }
struct Features { std::vector<uint8_t> input, eligible; };
Features Preprocess(const Frame& f) {
    const size_t n = f.depth.size();
    Features result; result.input.resize(n*3); result.eligible.resize(n);
    std::vector<float> valid; valid.reserve(n);
    for (float d : f.depth) if (d > 0) valid.push_back(d);
    float anchor = 0;
    if (!valid.empty()) {
        size_t m = valid.size()/2;
        std::nth_element(valid.begin(), valid.begin()+m, valid.end()); anchor = valid[m];
        if (valid.size()%2 == 0) anchor = (anchor + *std::max_element(valid.begin(), valid.begin()+m)) * .5f;
    }
    // Cache horizontal statistics for the seven live source rows. Each sum is
    // still formed left-to-right from zero, never by subtracting a departing
    // sample: rolling sums would change float rounding and quantized features.
    struct Row { float sum3, sum7, lo, hi; int count3, count7; };
    std::vector<Row> rows(size_t(std::min(f.height, 7))*f.width);
    int cached[7] = {-1, -1, -1, -1, -1, -1, -1};
    for (int y = 0; y < f.height; ++y) {
        const Row* vertical[7];
        for (int dy = -3; dy <= 3; ++dy) {
            int yy = std::max(0, std::min(f.height-1, y+dy));
            int slot = yy % 7;
            Row* row = rows.data() + size_t(slot)*f.width;
            if (cached[slot] != yy) {
                for (int x = 0; x < f.width; ++x) {
                    Row r = {0, 0, INFINITY, 0, 0, 0};
                    for (int dx = -3; dx <= 3; ++dx) {
                        int xx = std::max(0, std::min(f.width-1, x+dx));
                        float v = f.depth[size_t(yy)*f.width+xx];
                        if (v > 0) {
                            r.sum7 += v; ++r.count7;
                            if (std::abs(dx) <= 1) {
                                r.sum3 += v; ++r.count3;
                                r.lo = std::min(r.lo, v); r.hi = std::max(r.hi, v);
                            }
                        }
                    }
                    row[x] = r;
                }
                cached[slot] = yy;
            }
            vertical[dy+3] = row;
        }
        for (int x = 0; x < f.width; ++x) {
            const size_t p = size_t(y)*f.width+x; float d = f.depth[p];
            float sum3 = 0, sum7 = 0, lo = INFINITY, hi = 0; int count3 = 0, count7 = 0;
            for (int dy = -3; dy <= 3; ++dy) {
                float row3 = 0, row7 = 0;
                const Row& r = vertical[dy+3][x];
                row7 = r.sum7; count7 += r.count7;
                if (std::abs(dy) <= 1) {
                    row3 = r.sum3; count3 += r.count3;
                    lo = std::min(lo, r.lo); hi = std::max(hi, r.hi);
                }
                sum3 += row3; sum7 += row7;
            }
            double residual = (d - double(sum3)/std::max(count3,1))/.04;
            result.input[p*3] = Quantize(d > 0 ? Clip((d-double(sum7)/std::max(count7,1))/.04) : 0);
            result.input[p*3+1] = Quantize(d > 0 ? Clip((d-anchor)/.25f) : 0);
            result.input[p*3+2] = Quantize(f.confidence[p]);
            result.eligible[p] = d > 0 && count3 == 9 && hi-lo < .08f && hi-lo > 1e-6f &&
                std::fabs(residual) >= 1./256 && x >= 4 && y >= 4 && x < f.width-4 && y < f.height-4;
        }
    }
    return result;
}
bool SamePoint(const glm::vec4& a, const glm::vec4& b) {
    for (int i = 0; i < 4; ++i) if (std::memcmp(&a[i], &b[i], sizeof(float))) return false;
    return true;
}
bool WorldPoint(const glm::vec4& p) {
    for (int i = 0; i < 3; ++i) if (!std::isfinite(p[i]) || std::fabs(p[i]) > kWorldLimit) return false;
    return std::isfinite(p.w) && p.w > 0 && p.w <= 1;
}
bool CameraAdmission(const Frame& f, const glm::vec4& p, bool& admitted) {
    // Caller supplies the reconstruction COLOR_CAMERA transform (+Z forward).
    // Range is axial depth, not ray length. Use homogeneous w=1, not confidence.
    glm::dvec4 camera = f.worldToCamera * glm::dvec4(p.x, p.y, p.z, 1.);
    for (int i = 0; i < 4; ++i) if (!std::isfinite(camera[i])) return false;
    if (std::fabs(camera.w) < 1e-12) return false;
    double depth = camera.z / camera.w;
    if (!std::isfinite(depth)) return false;
    admitted = depth > 0 && depth >= f.minDepth && depth <= f.maxDepth;
    return true;
}
void Validate(const Frame& f, const std::vector<glm::vec4>& points) {
    Require(f.width > 0 && f.height > 0 && f.width <= 1024 && f.height <= 1024 &&
            size_t(f.width)*f.height <= kMaxPixels, "invalid native dimensions");
    const size_t n = size_t(f.width)*f.height;
    Require(f.depth.size() == n && f.confidence.size() == n, "depth/confidence size mismatch");
    Require(f.pointCount == points.size() && points.size() <= kMaxPixels && f.links.size() <= points.size(),
            "point count mismatch");
    Require(std::isfinite(f.minDepth) && std::isfinite(f.maxDepth) && f.minDepth >= 0 &&
            f.maxDepth > f.minDepth, "invalid camera depth range");
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j)
        Require(std::isfinite(f.worldToCamera[i][j]), "invalid camera transform");
    for (size_t i = 0; i < n; ++i) {
        Require(std::isfinite(f.depth[i]) && f.depth[i] >= 0 && f.depth[i] <= 10000, "invalid depth");
        Require(std::isfinite(f.confidence[i]) && f.confidence[i] >= 0 && f.confidence[i] <= 1,
                "invalid confidence");
    }
    std::vector<uint8_t> seen(points.size());
    for (const auto& l : f.links) {
        Require(l.point < points.size() && l.pixel < n, "link index out of bounds");
        Require(!seen[l.point]++, "duplicate point link");
        Require(SamePoint(l.original, points[l.point]), "original point mismatch");
        Require(l.depth == f.depth[l.pixel] && std::isfinite(l.depth), "link depth mismatch");
        Require(std::isfinite(l.minimumDepth) && l.minimumDepth >= .05f, "invalid minimum depth");
        for (int j = 0; j < 3; ++j)
            Require(std::isfinite(l.worldPerMetre[j]) && std::fabs(l.worldPerMetre[j]) <= kWorldLimit,
                    "invalid world ray");
    }
}
} // namespace detail
using namespace detail;

struct Runtime::State {
    std::atomic<bool> enabled{false};
    std::atomic<uint64_t> generation{1};
    mutable std::mutex statusMutex;
    std::string status = "TPU test: off";
    std::mutex inferenceMutex;
    std::unique_ptr<Backend> backend;
    int failedWidth = 0, failedHeight = 0;
    uint64_t failedGeneration = 0;
    std::string failure;
    uint64_t frames = 0, changed = 0;
    double milliseconds = 0;
    bool Current(uint64_t g) const { return enabled.load() && generation.load() == g; }
    void Report(uint64_t g, const std::string& text) {
        std::lock_guard<std::mutex> lock(statusMutex);
        if (Current(g)) { if (status != text) Log(text); status = text; }
    }
};
Runtime::Runtime() : state_(std::make_shared<State>()) {}
Runtime::~Runtime() = default;
void Runtime::SetEnabled(bool enabled) {
    auto s = state_; std::lock_guard<std::mutex> lock(s->statusMutex);
    if (s->enabled.load() == enabled) return;
    s->generation.fetch_add(1); s->enabled.store(enabled);
    s->status = enabled ? "TPU test: initializing" : "TPU test: off";
    Log(s->status);
}
void Runtime::Invalidate() {
    auto s = state_; std::lock_guard<std::mutex> lock(s->statusMutex);
    s->generation.fetch_add(1);
    s->status = s->enabled.load() ? "TPU test: initializing" : "TPU test: off";
}
bool Runtime::Enabled() const { return state_->enabled.load(); }
uint64_t Runtime::Generation() const { return state_->generation.load(); }
std::string Runtime::Status() const {
    auto s = state_; std::lock_guard<std::mutex> lock(s->statusMutex); return s->status;
}
void Runtime::Fallback(const std::string& reason) {
    auto s = state_;
    s->Report(s->generation.load(), "TPU test: original depth (" + reason.substr(0,160) + ")");
}
bool Runtime::Apply(const Frame& frame, std::vector<glm::vec4>& points, Stats& stats) {
    auto s = state_; stats = Stats{};
    if (!s->Current(frame.generation)) return false;
    std::lock_guard<std::mutex> inference(s->inferenceMutex);
    if (!s->Current(frame.generation)) return false;
    auto start = Clock::now();
    struct Timing { Stats& stats; Clock::time_point start;
        ~Timing() { stats.milliseconds = Elapsed(start); } } timing{stats, start};
    try {
        Validate(frame, points);
        Features features = Preprocess(frame);
        for (const auto& l : frame.links) {
            bool admitted;
            if (features.eligible[l.pixel] && WorldPoint(l.original) &&
                CameraAdmission(frame, l.original, admitted)) ++stats.eligible;
        }
        if (!s->Current(frame.generation)) return false;
        if (!s->backend || s->backend->width != frame.width || s->backend->height != frame.height) {
            if (s->failedGeneration == frame.generation && s->failedWidth == frame.width && s->failedHeight == frame.height)
                throw std::runtime_error(s->failure);
            s->Report(frame.generation, "TPU test: initializing");
            // One-shape cache: no unbounded driver allocations when dimensions change.
            s->backend.reset();
            std::unique_ptr<Backend> backend(new Backend(frame.width, frame.height));
            try { backend->Build(); }
            catch (const std::exception& e) {
                s->failedGeneration = frame.generation; s->failedWidth = frame.width; s->failedHeight = frame.height;
                s->failure = e.what(); throw;
            }
            s->backend = std::move(backend);
        }
        if (!s->Current(frame.generation)) return false;
        std::vector<uint8_t> output(frame.depth.size());
        s->backend->Infer(features.input, output);
        stats.inferred = true;
        if (!s->Current(frame.generation)) return false;
        Require(points.size() == frame.pointCount, "point count changed during inference");
        for (const auto& l : frame.links)
            Require(SamePoint(l.original, points[l.point]), "original point changed during inference");
        std::vector<glm::vec4> candidate(points);
        uint32_t changed = 0;
        auto a = s->backend->parameters.activation[3];
        for (const auto& l : frame.links) {
            bool originalAdmitted;
            if (!features.eligible[l.pixel] || !WorldPoint(l.original) ||
                !CameraAdmission(frame, l.original, originalAdmitted)) continue;
            double delta = double(Clip((int(output[l.pixel])-a.zero)*double(a.scale))) * .02;
            double depth = double(l.depth) + delta;
            if (!std::isfinite(depth) || depth <= .05 || depth <= l.minimumDepth) continue;
            glm::vec4 p = l.original;
            for (int j = 0; j < 3; ++j) p[j] = float(double(l.original[j]) + l.worldPerMetre[j]*delta);
            bool candidateAdmitted;
            if (!WorldPoint(p) || !CameraAdmission(frame, p, candidateAdmitted) ||
                candidateAdmitted != originalAdmitted) continue;
            if (!SamePoint(p, l.original)) { candidate[l.point] = p; ++changed; }
        }
#ifdef DEPTH_RUNTIME_TEST
        if (testBeforeCommit) testBeforeCommit();
#endif
        stats.milliseconds = Elapsed(start);
        char status[160];
        std::snprintf(status, sizeof(status), "TPU test: active %dx%d %u points %.1fms",
                      frame.width, frame.height, changed, stats.milliseconds);
        {
            // Commit and generation changes share only this short lock, never an NNAPI call.
            std::lock_guard<std::mutex> lock(s->statusMutex);
            if (!s->Current(frame.generation)) return false;
            points.swap(candidate); stats.changed = changed; s->status = status;
        }
        ++s->frames; s->changed += changed; s->milliseconds += stats.milliseconds;
        if (s->frames % 30 == 0) {
            Log("30 frames: " + std::to_string(s->changed) + " changed points, mean " +
                std::to_string(s->milliseconds/30) + "ms");
            s->changed = 0; s->milliseconds = 0;
        }
        return true;
    } catch (const std::exception& e) {
        stats.milliseconds = Elapsed(start);
        s->Report(frame.generation, "TPU test: original depth (" + std::string(e.what()).substr(0,160) + ")");
        return false;
    }
}
} } // namespace oc::depth_test
