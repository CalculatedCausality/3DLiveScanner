// Standalone NNAPI support/latency experiment. No trained weights or scanner code.
#define _POSIX_C_SOURCE 200809L
#include <android/NeuralNetworks.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define H 120
#define W 160
#define SCALE (1.0f / 128)
#define CHECK(call) do { int s_ = (call); if (s_) { \
    fprintf(stderr, "%s: NNAPI status=%d line=%d\n", #call, s_, __LINE__); exit(2); } } while (0)

typedef struct {
    ANeuralNetworksModel *model;
    int quant, operands, operations, inputs, normal_input;
    uint32_t in[2], out, shape[160][4];
    const char *names[20];
    void *constants[80];
    int allocated;
} Graph;

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void *alloc(size_t n) {
    void *p = calloc(1, n);
    if (!p) { perror("calloc"); exit(2); }
    return p;
}

static uint32_t operand(Graph *g, int type, unsigned rank, uint32_t *dims,
                        float scale, int zero) {
    uint32_t id = g->operands++;
    if (id >= 160) exit(2);
    ANeuralNetworksOperandType t = {type, rank, dims, scale, zero};
    CHECK(ANeuralNetworksModel_addOperand(g->model, &t));
    if (rank == 4) memcpy(g->shape[id], dims, 4 * sizeof(uint32_t));
    return id;
}

static uint32_t scalar(Graph *g, int32_t value) {
    uint32_t id = operand(g, ANEURALNETWORKS_INT32, 0, NULL, 0, 0);
    CHECK(ANeuralNetworksModel_setOperandValue(g->model, id, &value, sizeof(value)));
    return id;
}

static uint32_t tensor(Graph *g, int h, int w, int c) {
    uint32_t dims[] = {1, h, w, c};
    return operand(g, g->quant ? ANEURALNETWORKS_TENSOR_QUANT8_ASYMM :
                   ANEURALNETWORKS_TENSOR_FLOAT32, 4, dims,
                   g->quant ? SCALE : 0, g->quant ? 128 : 0);
}

static size_t elements(Graph *g, uint32_t id) {
    uint32_t *s = g->shape[id];
    return (size_t)s[0] * s[1] * s[2] * s[3];
}

static void op(Graph *g, const char *name, int code, int n, uint32_t *ins, uint32_t out) {
    CHECK(ANeuralNetworksModel_addOperation(g->model, code, n, ins, 1, &out));
    g->names[g->operations++] = name;
}

static uint32_t conv(Graph *g, uint32_t in, int channels, int kernel, int depthwise) {
    int ic = g->shape[in][3];
    uint32_t dims[] = {depthwise ? 1 : channels, kernel, kernel, depthwise ? channels : ic};
    uint32_t weights = operand(g, g->quant ? ANEURALNETWORKS_TENSOR_QUANT8_ASYMM :
                              ANEURALNETWORKS_TENSOR_FLOAT32, 4, dims,
                              g->quant ? SCALE : 0, g->quant ? 128 : 0);
    size_t n = elements(g, weights), bytes = n * (g->quant ? 1 : sizeof(float));
    void *data = alloc(bytes);
    g->constants[g->allocated++] = data;
    // Dense, deterministic positive kernels; power-of-two coefficients exactly quantize.
    // These are diagnostic filters, not learned depth/normal predictors.
    for (size_t i = 0; i < n; ++i) {
        unsigned oc = depthwise ? i % channels : i / (kernel * kernel * ic);
        float base = kernel == 3 ? (depthwise ? 1.0f/16 : 1.0f/32) : 1.0f/ic;
        float coeff = base * ((i + oc * 3) % 5 < 2 ? 0.5f : 1.0f);
        if (g->quant) ((unsigned char *)data)[i] = 128 + (int)lroundf(coeff / SCALE);
        else ((float *)data)[i] = coeff;
    }
    CHECK(ANeuralNetworksModel_setOperandValue(g->model, weights, data, bytes));
    uint32_t bias_dims[] = {channels};
    uint32_t bias = operand(g, g->quant ? ANEURALNETWORKS_TENSOR_INT32 :
                           ANEURALNETWORKS_TENSOR_FLOAT32, 1, bias_dims,
                           g->quant ? SCALE*SCALE : 0, 0);
    data = alloc(channels * 4);
    g->constants[g->allocated++] = data;
    CHECK(ANeuralNetworksModel_setOperandValue(g->model, bias, data, channels * 4));
    uint32_t out = tensor(g, g->shape[in][1], g->shape[in][2], channels);
    uint32_t args[] = {in, weights, bias, scalar(g, ANEURALNETWORKS_PADDING_SAME),
                       scalar(g, 1), scalar(g, 1), scalar(g, depthwise ? 1 : ANEURALNETWORKS_FUSED_RELU),
                       scalar(g, ANEURALNETWORKS_FUSED_RELU)};
    op(g, depthwise ? "DEPTHWISE_CONV_2D" : "CONV_2D", depthwise ?
       ANEURALNETWORKS_DEPTHWISE_CONV_2D : ANEURALNETWORKS_CONV_2D,
       depthwise ? 8 : 7, args, out);
    return out;
}

static uint32_t pool(Graph *g, uint32_t in) {
    uint32_t out = tensor(g, g->shape[in][1]/2, g->shape[in][2]/2, g->shape[in][3]);
    uint32_t args[] = {in, scalar(g, ANEURALNETWORKS_PADDING_VALID), scalar(g, 2),
                       scalar(g, 2), scalar(g, 2), scalar(g, 2), scalar(g, 0)};
    op(g, "AVERAGE_POOL_2D", ANEURALNETWORKS_AVERAGE_POOL_2D, 7, args, out);
    return out;
}

static uint32_t resize(Graph *g, uint32_t in) {
    uint32_t out = tensor(g, H, W, g->shape[in][3]);
    uint32_t args[] = {in, scalar(g, W), scalar(g, H)};
    op(g, "RESIZE_BILINEAR", ANEURALNETWORKS_RESIZE_BILINEAR, 3, args, out);
    return out;
}

static uint32_t concat(Graph *g, uint32_t a, uint32_t b) {
    uint32_t out = tensor(g, H, W, g->shape[a][3] + g->shape[b][3]);
    uint32_t args[] = {a, b, scalar(g, 3)};
    op(g, "CONCATENATION", ANEURALNETWORKS_CONCATENATION, 3, args, out);
    return out;
}

static uint32_t normalize(Graph *g, uint32_t in) {
    uint32_t out = tensor(g, H, W, g->shape[in][3]);
    op(g, "L2_NORMALIZATION", ANEURALNETWORKS_L2_NORMALIZATION, 1, &in, out);
    return out;
}

static Graph build(const char *name, int quant) {
    Graph g = {.quant = quant, .inputs = 1};
    g.normal_input = !strcmp(name, "l2");
    CHECK(ANeuralNetworksModel_create(&g.model));
    int decoder = !strcmp(name, "decoder") || !strcmp(name, "decoder_l2");
    int small = !strcmp(name, "resize");
    int channels = decoder || !strcmp(name, "conv") ? 2 : !strcmp(name, "l2") ? 3 : 8;
    uint32_t x = tensor(&g, small ? H/2 : H, small ? W/2 : W, channels);
    g.in[0] = x;
    if (decoder) {
        uint32_t skip = conv(&g, x, 8, 3, 0);
        x = conv(&g, skip, 8, 3, 1);
        x = conv(&g, x, 8, 1, 0);
        x = pool(&g, x);
        x = resize(&g, x);
        x = concat(&g, x, skip);
        x = conv(&g, x, 3, 1, 0);
        if (!strcmp(name, "decoder_l2")) x = normalize(&g, x);
    } else if (!strcmp(name, "conv")) x = conv(&g, x, 8, 3, 0);
    else if (!strcmp(name, "depthwise")) x = conv(&g, x, 8, 3, 1);
    else if (!strcmp(name, "pointwise")) x = conv(&g, x, 8, 1, 0);
    else if (!strcmp(name, "pool")) x = pool(&g, x);
    else if (!strcmp(name, "resize")) x = resize(&g, x);
    else if (!strcmp(name, "l2")) x = normalize(&g, x);
    else {
        uint32_t b = tensor(&g, H, W, channels);
        g.in[g.inputs++] = b;
        if (!strcmp(name, "concat")) x = concat(&g, x, b);
        else {
            uint32_t out = tensor(&g, H, W, channels);
            uint32_t args[] = {x, b, scalar(&g, 0)};
            int div = !strcmp(name, "div"), mul = !strcmp(name, "mul");
            op(&g, div ? "DIV" : mul ? "MUL" : "ADD", div ? ANEURALNETWORKS_DIV :
               mul ? ANEURALNETWORKS_MUL : ANEURALNETWORKS_ADD, 3, args, out);
            x = out;
        }
    }
    g.out = x;
    CHECK(ANeuralNetworksModel_identifyInputsAndOutputs(g.model, g.inputs, g.in, 1, &x));
    CHECK(ANeuralNetworksModel_finish(g.model));
    return g;
}

static void fill(Graph *g, int input, void *buffer, int variant) {
    uint32_t *s = g->shape[g->in[input]];
    for (unsigned y = 0; y < s[1]; ++y) for (unsigned x = 0; x < s[2]; ++x)
        for (unsigned c = 0; c < s[3]; ++c) {
            // Sloped plane + depth step + vertical gap; channels alternate depth and mask.
            int gap = x > s[2]/3 && x < s[2]/3 + 5;
            float value = gap ? 0 : 0.15f + 0.2f*x/s[2] + 0.1f*y/s[1] +
                          (x > s[2]/2 ? 0.25f : 0) + variant*0.001f;
            if (c & 1) value = gap ? 0 : 0.875f;
            if (g->normal_input) value = gap ? 0 : c == 0 ? value - 0.4f :
                c == 1 ? 0.2f - 0.4f*y/s[1] : 0.7f;
            if (input) value = 0.5f + 0.125f * ((x+y+c) % 3); // nonzero DIV denominator
            size_t i = ((size_t)y*s[2] + x)*s[3] + c;
            if (g->quant) ((unsigned char *)buffer)[i] = 128 + (int)lroundf(value / SCALE);
            else ((float *)buffer)[i] = value;
        }
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static int run(Graph *g, ANeuralNetworksDevice *device, const char *device_name,
               int repetitions, void *result) {
    bool supported[20] = {0};
    const ANeuralNetworksDevice *devices[] = {device};
    CHECK(ANeuralNetworksModel_getSupportedOperationsForDevices(g->model, devices, 1, supported));
    int all = 1;
    printf("{\"kind\":\"support\",\"device\":\"%s\",\"ops\":[", device_name);
    for (int i = 0; i < g->operations; ++i) {
        printf("%s{\"name\":\"%s\",\"supported\":%s}", i ? "," : "", g->names[i], supported[i] ? "true" : "false");
        all &= supported[i];
    }
    puts("]}");
    if (!all) return 0;
    ANeuralNetworksCompilation *comp = NULL;
    double start = now_ms();
    int status = ANeuralNetworksCompilation_createForDevices(g->model, devices, 1, &comp);
    if (!status) status = ANeuralNetworksCompilation_setPreference(comp, ANEURALNETWORKS_PREFER_FAST_SINGLE_ANSWER);
    if (!status) status = ANeuralNetworksCompilation_finish(comp);
    double compile_ms = now_ms() - start;
    if (status) {
        printf("{\"kind\":\"compile_error\",\"device\":\"%s\",\"status\":%d}\n", device_name, status);
        if (comp) ANeuralNetworksCompilation_free(comp);
        return -1;
    }
    size_t bytes[2], out_bytes = elements(g, g->out) * (g->quant ? 1 : 4);
    void *source[2], *input[2], *output = alloc(out_bytes);
    for (int j = 0; j < g->inputs; ++j) {
        bytes[j] = elements(g, g->in[j]) * (g->quant ? 1 : 4);
        source[j] = alloc(bytes[j]); input[j] = alloc(bytes[j]);
    }
    double *times = alloc(repetitions * sizeof(double)), *computes = alloc(repetitions * sizeof(double));
    double first = 0;
    for (int i = 0; i < repetitions + 3; ++i) {
        for (int j = 0; j < g->inputs; ++j) fill(g, j, source[j], i);
        start = now_ms();
        for (int j = 0; j < g->inputs; ++j) memcpy(input[j], source[j], bytes[j]);
        ANeuralNetworksExecution *exec = NULL;
        CHECK(ANeuralNetworksExecution_create(comp, &exec));
        for (int j = 0; j < g->inputs; ++j)
            CHECK(ANeuralNetworksExecution_setInput(exec, j, NULL, input[j], bytes[j]));
        CHECK(ANeuralNetworksExecution_setOutput(exec, 0, NULL, output, out_bytes));
        double cstart = now_ms();
        CHECK(ANeuralNetworksExecution_compute(exec));
        double compute_ms = now_ms() - cstart;
        memcpy(result, output, out_bytes);
        ANeuralNetworksExecution_free(exec);
        double total = now_ms() - start;
        if (i == 0) first = total;
        if (i >= 3) { times[i-3] = total; computes[i-3] = compute_ms; }
    }
    // Emit unsorted samples, then nearest-rank p50/p95 summaries.
    printf("{\"kind\":\"timing\",\"device\":\"%s\",\"compile_ms\":%.6f,\"first_ms\":%.6f,\"input_bytes\":%zu,\"output_bytes\":%zu,\"samples_ms\":[",
           device_name, compile_ms, first, bytes[0] + (g->inputs == 2 ? bytes[1] : 0), out_bytes);
    for (int i = 0; i < repetitions; ++i) printf("%s%.6f", i ? "," : "", times[i]);
    qsort(times, repetitions, sizeof(double), cmp_double);
    qsort(computes, repetitions, sizeof(double), cmp_double);
    int p50 = (int)ceil(repetitions * .5) - 1, p95 = (int)ceil(repetitions * .95) - 1;
    printf("],\"p50_ms\":%.6f,\"p95_ms\":%.6f,\"compute_call_p50_ms\":%.6f,\"fallback_requested\":false}\n",
           times[p50], times[p95], computes[p50]);
    for (int j = 0; j < g->inputs; ++j) { free(source[j]); free(input[j]); }
    free(times); free(computes); free(output);
    ANeuralNetworksCompilation_free(comp);
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 4 && argc != 5) { fprintf(stderr, "usage: depth_ops CASE q8|f32 REPETITIONS [OUTPUT_PREFIX]\n"); return 2; }
    const char *cases[] = {"conv", "depthwise", "pointwise", "pool", "resize", "concat", "add", "mul", "l2", "div", "decoder", "decoder_l2"};
    int valid = 0;
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) valid |= !strcmp(argv[1], cases[i]);
    int q = !strcmp(argv[2], "q8"), repetitions = atoi(argv[3]);
    if (!valid || (!q && strcmp(argv[2], "f32")) || repetitions < 1 || repetitions > 100 ||
        (q && !strcmp(argv[1], "div"))) return 2; // NNAPI DIV has no quant8 signature.
    setvbuf(stdout, NULL, _IONBF, 0);
    Graph g = build(argv[1], q);
    printf("{\"kind\":\"graph\",\"case\":\"%s\",\"dtype\":\"%s\",\"height\":%d,\"width\":%d,\"operations\":%d,\"warmups\":3,\"repetitions\":%d,\"trained\":false}\n",
           argv[1], argv[2], H, W, g.operations, repetitions);
    ANeuralNetworksDevice *tpu = NULL, *cpu = NULL;
    uint32_t count;
    CHECK(ANeuralNetworks_getDeviceCount(&count));
    for (uint32_t i = 0; i < count; ++i) {
        ANeuralNetworksDevice *d; const char *name, *version; int32_t type; int64_t feature;
        CHECK(ANeuralNetworks_getDevice(i, &d));
        CHECK(ANeuralNetworksDevice_getName(d, &name));
        CHECK(ANeuralNetworksDevice_getVersion(d, &version));
        CHECK(ANeuralNetworksDevice_getType(d, &type));
        CHECK(ANeuralNetworksDevice_getFeatureLevel(d, &feature));
        printf("{\"kind\":\"device\",\"name\":\"%s\",\"version\":\"%s\",\"type\":%d,\"feature\":%" PRId64 "}\n", name, version, type, feature);
        if (!strcmp(name, "google-edgetpu") && type == ANEURALNETWORKS_DEVICE_ACCELERATOR) tpu = d;
        if (!strcmp(name, "nnapi-reference") && type == ANEURALNETWORKS_DEVICE_CPU) cpu = d;
    }
    if (!tpu || !cpu) { fprintf(stderr, "Required named devices unavailable\n"); return 3; }
    size_t n = elements(&g, g.out);
    void *a = alloc(n * (q ? 1 : 4)), *b = alloc(n * (q ? 1 : 4));
    int ta = run(&g, tpu, "google-edgetpu", repetitions, a);
    int cb = run(&g, cpu, "nnapi-reference", repetitions, b);
    if (argc == 5) {
        const char *suffixes[] = {"tpu", "cpu"};
        void *outputs[] = {a, b};
        int states[] = {ta, cb};
        for (int i = 0; i < 2; ++i) if (states[i] == 1) {
            char path[512];
            int len = snprintf(path, sizeof(path), "%s.%s.bin", argv[4], suffixes[i]);
            if (len < 0 || (size_t)len >= sizeof(path)) return 2;
            FILE *f = fopen(path, "wb");
            size_t bytes = n * (q ? 1 : 4);
            if (!f || fwrite(outputs[i], 1, bytes, f) != bytes || fclose(f)) return 2;
        }
    }
    size_t bad = 0, nonfinite = 0; double max_error = 0, mean_error = 0;
    if (ta == 1 && cb == 1) {
        double min_value = INFINITY, max_value = -INFINITY;
        unsigned histogram[256] = {0}, unique = 0;
        for (size_t i = 0; i < n; ++i) {
            double av = q ? ((unsigned char *)a)[i] : ((float *)a)[i];
            double bv = q ? ((unsigned char *)b)[i] : ((float *)b)[i];
            double err = fabs(av - bv), tolerance = q ? 2 : 1e-4 + 1e-4*fabs(bv);
            if (av < min_value) min_value = av;
            if (av > max_value) max_value = av;
            if (q) histogram[(unsigned char)av]++;
            nonfinite += !isfinite(av) || !isfinite(bv);
            bad += !isfinite(err) || err > tolerance;
            if (isfinite(err)) { mean_error += err; if (err > max_error) max_error = err; }
        }
        for (int i = 0; i < 256; ++i) unique += histogram[i] != 0;
        printf("{\"kind\":\"output_range\",\"min\":%.9g,\"max\":%.9g,\"unique_q8_codes\":%u}\n", min_value, max_value, unique);
        printf("{\"kind\":\"comparison\",\"reference\":\"nnapi-reference\",\"elements\":%zu,\"error_units\":\"%s\",\"max_abs\":%.9g,\"mean_abs\":%.9g,\"outside_tolerance\":%zu,\"nonfinite\":%zu}\n",
               n, q ? "quantized_codes" : "float", max_error, mean_error/n, bad, nonfinite);
    }
    free(a); free(b);
    ANeuralNetworksModel_free(g.model);
    for (int i = 0; i < g.allocated; ++i) free(g.constants[i]);
    return ta < 0 || cb != 1 || bad ? 4 : 0;
}
