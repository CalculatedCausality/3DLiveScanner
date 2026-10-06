// Fixed DCLN0001 depth-residual graph. See TRAINED_DEPTH.md for the wire contract.
#define _POSIX_C_SOURCE 200809L
#include <android/NeuralNetworks.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define H 120
#define W 160
#define INPUT_BYTES (H * W * 3)
#define OUTPUT_BYTES (H * W)
#define MAX_FRAMES 64
#define MAX_REPETITIONS 100
#define MODEL_BYTES 1880

typedef struct { float scale; int32_t zero; } Activation;
typedef struct {
    float scale;
    uint8_t weights[1296];
    int32_t bias[12];
} Layer;
typedef struct { Activation activation[4]; Layer layer[3]; } Parameters;
typedef struct { ANeuralNetworksModel *model; uint32_t count, input, output; } Graph;
typedef struct { double input_ms, nnapi_ms, output_ms, total_ms; } Timing;

static FILE *report;
static const uint32_t channels[] = {3, 12, 12, 1};

// Every record is flushed before the next driver call, retaining evidence on timeout.
static void emit(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    fflush(stdout);
    if (report) {
        va_start(args, format);
        vfprintf(report, format, args);
        va_end(args);
        if (fflush(report) || ferror(report)) {
            fprintf(stderr, "Cannot write native report\n");
            exit(2);
        }
    }
}

static void fail(const char *stage, int status, int code) {
    emit("{\"kind\":\"error\",\"stage\":\"%s\",\"status\":%d,\"exit_code\":%d}\n",
         stage, status, code);
    fprintf(stderr, "%s: status=%d (exit %d)\n", stage, status, code);
    exit(code);
}

#define CHECK(call) do { int s_ = (call); if (s_) fail(#call, s_, 4); } while (0)

static double now_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) fail("clock", -1, 2);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void *allocate(size_t bytes) {
    void *p = calloc(1, bytes);
    if (!p) fail("allocation", -1, 2);
    return p;
}

// Bounded regular files only. The extra read also detects growth after fstat.
static uint8_t *load(const char *path, size_t minimum, size_t maximum,
                     size_t multiple, size_t *bytes) {
    FILE *f = fopen(path, "rb");
    if (!f) fail("open_input_file", -1, 2);
    struct stat st;
    if (fstat(fileno(f), &st) || !S_ISREG(st.st_mode) || st.st_size < (off_t)minimum ||
        st.st_size > (off_t)maximum || st.st_size % (off_t)multiple) {
        fclose(f);
        fail("invalid_file_size_or_type", -1, 2);
    }
    *bytes = (size_t)st.st_size;
    uint8_t *data = allocate(*bytes);
    bool bad = fread(data, 1, *bytes, f) != *bytes;
    bad |= fgetc(f) != EOF || ferror(f);
    bad |= fclose(f) != 0;
    if (bad) fail("input_read_or_trailing_bytes", -1, 2);
    return data;
}

static uint32_t u32le(const uint8_t **p) {
    uint32_t value = (uint32_t)(*p)[0] | (uint32_t)(*p)[1] << 8 |
                     (uint32_t)(*p)[2] << 16 | (uint32_t)(*p)[3] << 24;
    *p += 4;
    return value;
}

static int32_t i32le(const uint8_t **p) {
    uint32_t bits = u32le(p);
    int32_t value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static float scale_le(const uint8_t **p) {
    uint32_t bits = u32le(p);
    float value;
    _Static_assert(sizeof(float) == 4, "Requires float32");
    memcpy(&value, &bits, sizeof(value));
    if (!isfinite(value) || value <= 0) fail("invalid_scale", -1, 2);
    return value;
}

static Parameters read_parameters(const char *path) {
    size_t bytes;
    uint8_t *data = load(path, MODEL_BYTES, MODEL_BYTES, 1, &bytes);
    if (memcmp(data, "DCLN0001", 8)) fail("invalid_magic", -1, 2);
    const uint8_t *p = data + 8;
    Parameters params = {0};
    for (int i = 0; i < 4; ++i) {
        params.activation[i].scale = scale_le(&p);
        params.activation[i].zero = i32le(&p);
        if (params.activation[i].zero < 0 || params.activation[i].zero > 255)
            fail("invalid_activation_zero", -1, 2);
    }
    for (int i = 0; i < 3; ++i) {
        Layer *l = &params.layer[i];
        l->scale = scale_le(&p);
        float bias_scale = params.activation[i].scale * l->scale;
        if (!isfinite(bias_scale) || bias_scale <= 0) fail("invalid_bias_scale_product", -1, 2);
        size_t n = (size_t)channels[i + 1] * 3 * 3 * channels[i];
        memcpy(l->weights, p, n);
        p += n;
        for (uint32_t j = 0; j < channels[i + 1]; ++j) l->bias[j] = i32le(&p);
    }
    if ((size_t)(p - data) != bytes) fail("model_contract_size", -1, 2);
    free(data);
    return params;
}

static uint32_t operand(Graph *g, int type, uint32_t rank, const uint32_t *dims,
                        float scale, int32_t zero) {
    uint32_t id = g->count++;
    ANeuralNetworksOperandType t = {type, rank, dims, scale, zero};
    CHECK(ANeuralNetworksModel_addOperand(g->model, &t));
    return id;
}

static uint32_t scalar(Graph *g, int32_t value) {
    uint32_t id = operand(g, ANEURALNETWORKS_INT32, 0, NULL, 0, 0);
    CHECK(ANeuralNetworksModel_setOperandValue(g->model, id, &value, sizeof(value)));
    return id;
}

static uint32_t activation(Graph *g, const Parameters *p, int layer) {
    uint32_t shape[] = {1, H, W, channels[layer]};
    return operand(g, ANEURALNETWORKS_TENSOR_QUANT8_ASYMM, 4, shape,
                   p->activation[layer].scale, p->activation[layer].zero);
}

static Graph build(const Parameters *p) {
    Graph g = {0};
    CHECK(ANeuralNetworksModel_create(&g.model));
    uint32_t x = activation(&g, p, 0);
    g.input = x;
    for (int i = 0; i < 3; ++i) {
        uint32_t shape[] = {channels[i + 1], 3, 3, channels[i]};
        uint32_t weights = operand(&g, ANEURALNETWORKS_TENSOR_QUANT8_ASYMM, 4,
                                   shape, p->layer[i].scale, 128);
        CHECK(ANeuralNetworksModel_setOperandValue(g.model, weights, p->layer[i].weights,
                                                  (size_t)shape[0] * 9 * shape[3]));
        uint32_t bias = operand(&g, ANEURALNETWORKS_TENSOR_INT32, 1, &shape[0],
                               p->activation[i].scale * p->layer[i].scale, 0);
        CHECK(ANeuralNetworksModel_setOperandValue(g.model, bias, p->layer[i].bias,
                                                  shape[0] * sizeof(int32_t)));
        uint32_t out = activation(&g, p, i + 1);
        uint32_t args[] = {x, weights, bias, scalar(&g, ANEURALNETWORKS_PADDING_SAME),
                          scalar(&g, 1), scalar(&g, 1),
                          scalar(&g, i == 2 ? ANEURALNETWORKS_FUSED_NONE : ANEURALNETWORKS_FUSED_RELU)};
        CHECK(ANeuralNetworksModel_addOperation(g.model, ANEURALNETWORKS_CONV_2D, 7, args, 1, &out));
        x = out;
    }
    g.output = x;
    CHECK(ANeuralNetworksModel_identifyInputsAndOutputs(g.model, 1, &g.input, 1, &g.output));
    CHECK(ANeuralNetworksModel_finish(g.model));
    return g;
}

// Escape driver-provided strings rather than assuming printable JSON-safe metadata.
static void json_string(const char *s) {
    emit("\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p == '"' || *p == '\\') emit("\\%c", *p);
        else if (*p < 32 || *p >= 127) emit("\\u%04x", *p);
        else emit("%c", *p);
    }
    emit("\"");
}

static bool supported(Graph *g, ANeuralNetworksDevice *device, const char *name) {
    bool ops[3] = {false};
    const ANeuralNetworksDevice *devices[] = {device};
    int status = ANeuralNetworksModel_getSupportedOperationsForDevices(g->model, devices, 1, ops);
    emit("{\"kind\":\"support\",\"device\":\"%s\",\"status\":%d,\"ops\":[%s,%s,%s]}\n",
         name, status, ops[0] ? "true" : "false", ops[1] ? "true" : "false", ops[2] ? "true" : "false");
    if (status) fail("support_query", status, 4);
    return ops[0] && ops[1] && ops[2];
}

static Timing execute(ANeuralNetworksCompilation *comp, const uint8_t *source,
                      uint8_t *input, uint8_t *output, uint8_t *destination,
                      const char *device, const char *phase, unsigned index) {
    Timing t = {0};
    ANeuralNetworksExecution *exec = NULL;
    double start = now_ms();
    memcpy(input, source, INPUT_BYTES);
    double copied = now_ms();
    t.input_ms = copied - start;
    const char *stage = "create";
    int status = ANeuralNetworksExecution_create(comp, &exec);
    if (!status) { stage = "set_input"; status = ANeuralNetworksExecution_setInput(exec, 0, NULL, input, INPUT_BYTES); }
    if (!status) { stage = "set_output"; status = ANeuralNetworksExecution_setOutput(exec, 0, NULL, output, OUTPUT_BYTES); }
    if (!status) { stage = "compute"; status = ANeuralNetworksExecution_compute(exec); }
    double computed = now_ms();
    t.nnapi_ms = computed - copied; // creation, bindings and synchronous compute
    if (!status) memcpy(destination, output, OUTPUT_BYTES);
    double out_copied = now_ms();
    t.output_ms = out_copied - computed;
    if (exec) ANeuralNetworksExecution_free(exec);
    t.total_ms = now_ms() - start;
    emit("{\"kind\":\"call\",\"device\":\"%s\",\"phase\":\"%s\",\"index\":%u,"
         "\"input_frame\":%u,\"status\":%d,\"stage\":\"%s\",\"input_memcpy_ms\":%.6f,"
         "\"nnapi_ms\":%.6f,\"output_memcpy_ms\":%.6f,\"end_to_end_ms\":%.6f}\n",
         device, phase, index, !strcmp(phase, "frame") ? index : 0, status, stage,
         t.input_ms, t.nnapi_ms, t.output_ms, t.total_ms);
    if (status) fail("execution", status, 4);
    return t;
}

static int compare_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void path_for(char *path, size_t size, const char *prefix, const char *suffix) {
    int n = snprintf(path, size, "%s.%s", prefix, suffix);
    if (n < 0 || (size_t)n >= size) fail("output_path_too_long", -1, 2);
}

static void save_output(const char *prefix, const char *suffix, const uint8_t *data, size_t bytes) {
    char path[4096];
    path_for(path, sizeof(path), prefix, suffix);
    FILE *f = fopen(path, "wbx"); // no stale output overwrite
    if (!f) fail("open_output", -1, 2);
    bool bad = fwrite(data, 1, bytes, f) != bytes;
    bad |= fclose(f) != 0;
    if (bad) fail("write_output", -1, 2);
    emit("{\"kind\":\"output\",\"suffix\":\"%s\",\"bytes\":%zu,\"status\":0}\n", suffix, bytes);
}

static uint8_t *run(Graph *g, ANeuralNetworksDevice *device, const char *name,
                    const uint8_t *frames, unsigned count, int repetitions,
                    const char *prefix, const char *suffix) {
    const ANeuralNetworksDevice *devices[] = {device};
    ANeuralNetworksCompilation *comp = NULL;
    emit("{\"kind\":\"compile_start\",\"device\":\"%s\",\"explicit_device_count\":1}\n", name);
    double start = now_ms();
    const char *stage = "create_for_devices";
    int status = ANeuralNetworksCompilation_createForDevices(g->model, devices, 1, &comp);
    if (!status) { stage = "preference"; status = ANeuralNetworksCompilation_setPreference(comp, ANEURALNETWORKS_PREFER_FAST_SINGLE_ANSWER); }
    if (!status) { stage = "finish"; status = ANeuralNetworksCompilation_finish(comp); }
    double compile_ms = now_ms() - start;
    emit("{\"kind\":\"compile\",\"device\":\"%s\",\"status\":%d,\"stage\":\"%s\",\"compile_ms\":%.6f}\n",
         name, status, stage, compile_ms);
    if (status) fail("compile", status, 4);
    uint8_t *input = allocate(INPUT_BYTES), *output = allocate(OUTPUT_BYTES);
    uint8_t *destination = allocate((size_t)count * OUTPUT_BYTES);
    double samples[MAX_REPETITIONS], warmup_ms = 0, first_ms = 0;
    emit("{\"kind\":\"execution_start\",\"device\":\"%s\"}\n", name);
    for (unsigned i = 0; i < 3; ++i) {
        Timing t = execute(comp, frames, input, output, destination, name, "warmup", i);
        if (i == 0) first_ms = t.total_ms;
        warmup_ms += t.total_ms;
    }
    for (int i = 0; i < repetitions; ++i) {
        Timing t = execute(comp, frames, input, output, destination, name, "repeat", (unsigned)i);
        samples[i] = t.total_ms;
    }
    qsort(samples, (size_t)repetitions, sizeof(double), compare_double);
    emit("{\"kind\":\"timing\",\"device\":\"%s\",\"compile_ms\":%.6f,\"first_call_ms\":%.6f,"
         "\"warmup_total_ms\":%.6f,\"warmups\":3,\"repetitions\":%d,\"fixed_input_frame\":0,"
         "\"p50_ms\":%.6f,\"p95_ms\":%.6f,\"fallback_requested\":false}\n",
         name, compile_ms, first_ms, warmup_ms, repetitions,
         samples[(int)ceil(repetitions * .5) - 1], samples[(int)ceil(repetitions * .95) - 1]);
    for (unsigned i = 0; i < count; ++i)
        execute(comp, frames + (size_t)i * INPUT_BYTES, input, output,
                destination + (size_t)i * OUTPUT_BYTES, name, "frame", i);
    save_output(prefix, suffix, destination, (size_t)count * OUTPUT_BYTES);
    free(input);
    free(output);
    ANeuralNetworksCompilation_free(comp);
    return destination;
}

static void comparison(const uint8_t *a, const uint8_t *b, size_t n, int frame) {
    size_t unequal = 0, over_two = 0;
    unsigned max_abs = 0, min_a = 255, max_a = 0, min_b = 255, max_b = 0;
    uint64_t sum = 0;
    int64_t signed_sum = 0;
    for (size_t i = 0; i < n; ++i) {
        int delta = (int)a[i] - (int)b[i];
        unsigned diff = (unsigned)abs(delta);
        unequal += diff != 0;
        over_two += diff > 2;
        if (diff > max_abs) max_abs = diff;
        if (a[i] < min_a) min_a = a[i];
        if (a[i] > max_a) max_a = a[i];
        if (b[i] < min_b) min_b = b[i];
        if (b[i] > max_b) max_b = b[i];
        sum += diff;
        signed_sum += delta;
    }
    emit("{\"kind\":\"comparison\",\"frame\":%d,\"elements\":%zu,\"error_units\":\"integer_codes\","
         "\"unequal\":%zu,\"over_two_codes\":%zu,\"max_abs\":%u,\"mean_abs\":%.9g,"
         "\"mean_signed_tpu_minus_cpu\":%.9g,\"tpu_range\":[%u,%u],\"cpu_range\":[%u,%u],"
         "\"quality_gate_applied\":false}\n", frame, n, unequal, over_two, max_abs,
         (double)sum / n, (double)signed_sum / n, min_a, max_a, min_b, max_b);
}

int main(int argc, char **argv) {
    if (argc != 4 && argc != 5) {
        fprintf(stderr, "usage: trained_depth MODEL INPUT OUT_PREFIX [repetitions]\n");
        return 2;
    }
    char path[4096];
    path_for(path, sizeof(path), argv[3], "report.jsonl");
    report = fopen(path, "wx");
    if (!report) { fprintf(stderr, "Cannot create report (prefix must be new)\n"); return 2; }
    setvbuf(stdout, NULL, _IONBF, 0);
    int repetitions = 5;
    if (argc == 5) {
        char *end;
        long value = strtol(argv[4], &end, 10);
        if (!argv[4][0] || *end || value < 1 || value > MAX_REPETITIONS)
            fail("repetitions_must_be_1_to_100", -1, 2);
        repetitions = (int)value;
    }
    emit("{\"kind\":\"start\",\"contract\":\"DCLN0001\",\"quality_claim\":\"not_evaluated\"}\n");
    Parameters params = read_parameters(argv[1]);
    size_t input_bytes;
    uint8_t *frames = load(argv[2], INPUT_BYTES, MAX_FRAMES * INPUT_BYTES, INPUT_BYTES, &input_bytes);
    unsigned count = (unsigned)(input_bytes / INPUT_BYTES);
    emit("{\"kind\":\"graph\",\"operations\":3,\"op\":\"CONV_2D\",\"shape\":[1,120,160,3],"
         "\"channels\":[12,12,1],\"fused\":[\"RELU\",\"RELU\",\"NONE\"],\"padding\":\"SAME\","
         "\"stride\":1,\"frames\":%u,\"input_frame_bytes\":%d,\"output_frame_bytes\":%d,"
         "\"warmups\":3,\"repetitions\":%d,\"fallback_requested\":false}\n",
         count, INPUT_BYTES, OUTPUT_BYTES, repetitions);
    for (int i = 0; i < 4; ++i)
        emit("{\"kind\":\"activation\",\"index\":%d,\"scale\":%.9g,\"zero\":%d}\n",
             i, params.activation[i].scale, params.activation[i].zero);
    for (int i = 0; i < 3; ++i)
        emit("{\"kind\":\"layer\",\"index\":%d,\"ohwi\":[%u,3,3,%u],\"weight_scale\":%.9g,"
             "\"weight_zero\":128,\"bias_scale\":%.9g}\n", i, channels[i + 1], channels[i],
             params.layer[i].scale, params.activation[i].scale * params.layer[i].scale);
    Graph g = build(&params);
    ANeuralNetworksDevice *tpu = NULL, *cpu = NULL;
    uint32_t devices;
    CHECK(ANeuralNetworks_getDeviceCount(&devices));
    for (uint32_t i = 0; i < devices; ++i) {
        ANeuralNetworksDevice *d;
        const char *name, *version;
        int32_t type;
        int64_t feature;
        CHECK(ANeuralNetworks_getDevice(i, &d));
        CHECK(ANeuralNetworksDevice_getName(d, &name));
        CHECK(ANeuralNetworksDevice_getVersion(d, &version));
        CHECK(ANeuralNetworksDevice_getType(d, &type));
        CHECK(ANeuralNetworksDevice_getFeatureLevel(d, &feature));
        emit("{\"kind\":\"device\",\"name\":"); json_string(name);
        emit(",\"version\":"); json_string(version);
        emit(",\"type\":%d,\"feature_level\":%" PRId64 "}\n", type, feature);
        if (!strcmp(name, "google-edgetpu") && type == ANEURALNETWORKS_DEVICE_ACCELERATOR) tpu = d;
        if (!strcmp(name, "nnapi-reference") && type == ANEURALNETWORKS_DEVICE_CPU) cpu = d;
    }
    if (!tpu) fail("google-edgetpu_accelerator_unavailable", -1, 3);
    if (!cpu) fail("nnapi-reference_cpu_unavailable", -1, 3);
    bool tpu_ok = supported(&g, tpu, "google-edgetpu");
    bool cpu_ok = supported(&g, cpu, "nnapi-reference");
    if (!tpu_ok) fail("unsupported_accelerator_graph", -1, 3);
    if (!cpu_ok) fail("unsupported_reference_graph", -1, 3);
    uint8_t *a = run(&g, tpu, "google-edgetpu", frames, count, repetitions, argv[3], "tpu.bin");
    uint8_t *b = run(&g, cpu, "nnapi-reference", frames, count, repetitions, argv[3], "cpu.bin");
    for (unsigned i = 0; i < count; ++i)
        comparison(a + (size_t)i * OUTPUT_BYTES, b + (size_t)i * OUTPUT_BYTES, OUTPUT_BYTES, (int)i);
    comparison(a, b, (size_t)count * OUTPUT_BYTES, -1);
    free(a); free(b); free(frames);
    ANeuralNetworksModel_free(g.model);
    emit("{\"kind\":\"complete\",\"status\":0,\"frames\":%u,\"quality_gate_applied\":false}\n", count);
    if (fclose(report)) return 2;
    return 0;
}
