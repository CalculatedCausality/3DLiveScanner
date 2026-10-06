// SPDX-License-Identifier: Apache-2.0
// Built twice by performance.py, against frozen and current complete sources.
#include RUNTIME_TEST_SOURCE
#include <iomanip>

namespace {
uint64_t Hash(const std::vector<uint8_t>& bytes, uint64_t hash = 14695981039346656037ULL) {
    for (uint8_t b : bytes) { hash ^= b; hash *= 1099511628211ULL; }
    return hash;
}
template<class T> void Write(std::ofstream& stream, const T& value) {
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
}
void Compare(const char* inputPath, const char* outputPath) {
    std::ifstream input(inputPath, std::ios::binary);
    std::ofstream output(outputPath, std::ios::binary);
    assert(input.good() && output.good());
    int cases = 0;
    uint64_t totalChanged = 0;
    while (input.peek() != EOF) {
        Frame f;
        input.read(reinterpret_cast<char*>(&f.width), 4);
        input.read(reinterpret_cast<char*>(&f.height), 4);
        assert(f.width > 0 && f.width <= 1024 && f.height > 0 && f.height <= 1024);
        const size_t n = size_t(f.width)*f.height;
        assert(n <= 262144);
        f.depth.resize(n); f.confidence.resize(n);
        input.read(reinterpret_cast<char*>(f.depth.data()), n*4);
        input.read(reinterpret_cast<char*>(f.confidence.data()), n*4);
        assert(input.good());
        auto features = Preprocess(f);
        Write(output, f.width); Write(output, f.height);
        output.write(reinterpret_cast<const char*>(features.input.data()), features.input.size());
        output.write(reinterpret_cast<const char*>(features.eligible.data()), features.eligible.size());
        fake.Reset(); Install();
        Runtime runtime; runtime.SetEnabled(true);
        f.generation = runtime.Generation(); f.pointCount = n;
        f.minDepth = .2; f.maxDepth = 3.;
        f.worldToCamera[3][2] = -.01;
        std::vector<glm::vec4> originals(n);
        for (size_t i = 0; i < n; ++i) {
            originals[i] = glm::vec4(float(i % f.width)*.01f, float(i / f.width)*.01f,
                                     f.depth[i], i % 19 ? .75f : 0.f);
            if (i % 11 == 0) continue; // Unmapped points must also compare byte-exactly.
            Link link; link.pixel = i; link.point = i; link.depth = f.depth[i];
            link.original = originals[i]; link.worldPerMetre = glm::dvec3(.3, -.2, 1.);
            link.minimumDepth = i % 13 ? .05f : 1.03f;
            f.links.push_back(link);
        }
        for (int direction : {0, 1}) {
            // Deterministic input-dependent synthetic model output, covering both
            // correction signs without executing NNAPI or claiming model speed.
            fake.duringCompute = [direction] {
                auto bytes = static_cast<uint8_t*>(fake.output);
                for (size_t i = 0; i < fake.outputSize; ++i)
                    bytes[i] = uint8_t((fake.input[i*3] + fake.input[i*3+1] + i + direction*127) & 255);
            };
            auto points = originals;
            Stats stats;
            assert(runtime.Apply(f, points, stats) && stats.inferred);
            assert(fake.input == features.input);
            totalChanged += stats.changed;
            Write(output, stats.eligible); Write(output, stats.changed);
            Write(output, uint8_t(stats.inferred));
            for (const auto& p : points) for (int j = 0; j < 4; ++j) Write(output, p[j]);
        }
        fake.duringCompute = {};
        ++cases;
    }
    assert(output.good() && totalChanged > 0);
    std::cout << "Compared-output serialization: " << cases << " cases, " << totalChanged
              << " changed points across both correction directions\n";
}
void Bench(int iterations, const char* outputPath) {
    assert(iterations >= 1 && iterations <= 100);
    std::ofstream output;
    if (outputPath) { output.open(outputPath, std::ios::binary); assert(output.good()); }
    uint64_t checksum = 14695981039346656037ULL;
    for (auto shape : {std::make_pair(160,90), std::make_pair(160,120),
                       std::make_pair(320,240), std::make_pair(512,512)}) {
        for (bool holes : {false, true}) {
            Frame f; f.width = shape.first; f.height = shape.second;
            size_t n = size_t(f.width)*f.height;
            f.depth.resize(n); f.confidence.resize(n);
            uint32_t seed = 20261001;
            for (size_t i = 0; i < n; ++i) {
                seed = seed*1664525U + 1013904223U;
                f.depth[i] = 1.f + float(seed % 5001)*.00001f;
                if (holes && seed % 7 == 0) f.depth[i] = 0;
                f.confidence[i] = float((seed >> 16) % 257)/256.f;
            }
            auto expected = Preprocess(f); // warm-up; comparison/hash outside timed region
            if (outputPath) {
                output.write(reinterpret_cast<const char*>(expected.input.data()), expected.input.size());
                output.write(reinterpret_cast<const char*>(expected.eligible.data()), expected.eligible.size());
            }
            std::vector<double> timings;
            for (int repeat = 0; repeat < iterations; ++repeat) {
                auto start = Clock::now();
                auto actual = Preprocess(f);
                double ms = Elapsed(start);
                assert(actual.input == expected.input && actual.eligible == expected.eligible);
                checksum = Hash(actual.eligible, Hash(actual.input, checksum));
                timings.push_back(ms);
            }
            std::sort(timings.begin(), timings.end());
            size_t m = timings.size()/2;
            double median = timings.size()%2 ? timings[m] : (timings[m-1]+timings[m])*.5;
            std::cout << f.width << 'x' << f.height << (holes ? " holes" : " dense")
                      << " preprocessing_component_ms=" << std::fixed << std::setprecision(6) << median
                      << " iterations=" << iterations << " feature_mask_hash=" << std::hex
                      << Hash(expected.eligible, Hash(expected.input)) << std::dec << '\n';
        }
    }
    if (outputPath) assert(output.good());
    std::cout << "All-byte timing checksum=" << std::hex << checksum << std::dec << '\n';
}
}
int main(int argc, char** argv) {
    if (argc >= 2 && argc <= 4 && std::string(argv[1]) == "bench") {
        Bench(argc >= 3 ? std::stoi(argv[2]) : 8, argc == 4 ? argv[3] : nullptr); return 0;
    }
    if (argc == 4 && std::string(argv[1]) == "compare") {
        Compare(argv[2], argv[3]); return 0;
    }
    std::cerr << "Usage: performance bench [iterations [output.bin]] | compare corpus.bin output.bin\n";
    return 1;
}
