#include <data/image.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

static void fill(oc::Image& image) {
    uint32_t state = 431;
    for (size_t i = 0; i < size_t(image.GetWidth()) * image.GetHeight() * 4; ++i) {
        state = state * 1664525u + 1013904223u;
        image.GetData()[i] = state >> 24;
    }
}

// Slow reference preserves the old channel/rounding contract but indexes the
// declared output dimensions explicitly. No speed optimizations in this oracle.
static std::vector<unsigned char> reference(oc::Image& image, unsigned scale) {
    const int w = image.GetWidth() / scale, h = image.GetHeight() / scale;
    std::vector<unsigned char> output(size_t(w) * h * 3 / 2);
    size_t luma = 0, chroma = size_t(w) * h;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t pixel = (size_t(image.GetHeight() - 1 - y * scale) * image.GetWidth() + x * scale) * 4;
#if SCANNER_MODERN
            int r = image.GetData()[pixel], g = image.GetData()[pixel + 1], b = image.GetData()[pixel + 2];
#else
            int b = image.GetData()[pixel], g = image.GetData()[pixel + 1], r = image.GetData()[pixel + 2];
#endif
            int Y = ((66*r + 129*g + 25*b + 128) >> 8) + 16;
#if SCANNER_MODERN
            int V = ((112*r - 94*g - 18*b + 128) >> 8) + 128;
            int U = ((-38*r - 74*g + 112*b + 128) >> 8) + 128;
#else
            int V = ((-38*r - 74*g + 112*b + 128) >> 8) + 128;
            int U = ((112*r - 94*g - 18*b + 128) >> 8) + 128;
#endif
            output[luma++] = std::max(0, std::min(255, Y));
            if ((y % 2) == 0 && (x % 2) == 0) {
                output[chroma++] = std::max(0, std::min(255, V));
                output[chroma++] = std::max(0, std::min(255, U));
            }
        }
    }
    return output;
}

static void check(int w, int h, unsigned scale) {
    oc::Image image(w, h);
    fill(image);
    std::vector<unsigned char> before(image.GetData(), image.GetData() + size_t(w) * h * 4);
    const auto expected = reference(image, scale);
    unsigned char* actual = image.ExtractYUVDownscaled(scale);
    assert(actual && std::memcmp(actual, expected.data(), expected.size()) == 0);
    assert(std::memcmp(image.GetData(), before.data(), before.size()) == 0);
    delete[] actual;
}

static void benchmark(int w, int h, unsigned scale, const char* label) {
    oc::Image image(w, h);
    fill(image);
    const size_t bytes = size_t(w / scale) * (h / scale) * 3 / 2;
    std::vector<double> samples;
    uint64_t checksum = 0;
    for (int trial = 0; trial < 7; ++trial) {
        auto begin = std::chrono::steady_clock::now();
        for (int frame = 0; frame < 80; ++frame) {
            unsigned char* result = image.ExtractYUVDownscaled(scale);
            assert(result);
            checksum += result[(frame * 997) % bytes];
            delete[] result;
        }
        samples.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count() / 80);
    }
    // Hash all meaningful bytes outside the timed region so both binaries can
    // prove identical output, not just identical sampled benchmark values.
    unsigned char* result = image.ExtractYUVDownscaled(scale);
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < bytes; ++i) hash = (hash ^ result[i]) * 1099511628211ULL;
    delete[] result;
    std::sort(samples.begin(), samples.end());
    std::cout << "YUV " << label << " " << w << "x" << h << "/" << scale
              << " median_us=" << samples[3] << " hash=" << hash << " sample_sum=" << checksum << '\n';
}

int main(int argc, char** argv) {
    if (argc > 1) {
        benchmark(360, 640, 1, argv[2]);
        benchmark(1080, 1920, 3, argv[2]);
        return 0;
    }
    {
        oc::Image image(2, 2);
        const unsigned char pixels[] = {0,0,0,255, 255,255,255,255, 0,0,255,255, 255,0,0,255};
#if SCANNER_MODERN
        const unsigned char golden[] = {41,82,16,235,110,240};
#else
        const unsigned char golden[] = {82,41,16,235,90,240};
#endif
        std::memcpy(image.GetData(), pixels, sizeof(pixels));
        unsigned char* result = image.ExtractYUVDownscaled(1);
        assert(result && std::memcmp(result, golden, sizeof(golden)) == 0);
        delete[] result;
    }
    check(2, 2, 1); check(4, 8, 1); check(12, 18, 3);
    check(360, 640, 1); check(1080, 1920, 3);
    check(7, 7, 3); check(13, 25, 3); // Floor-sized, even outputs for nondivisible inputs.
    for (int width = 2; width <= 30; width += 2)
        for (int height = 2; height <= 24; height += 2) check(width, height, 1);
    oc::Image invalid(3, 4);
    assert(invalid.ExtractYUVDownscaled(1) == nullptr);
    assert(invalid.ExtractYUVDownscaled(0) == nullptr);
    assert(invalid.ExtractYUVDownscaled(10) == nullptr);
    oc::Image empty(0, 0);
    assert(empty.ExtractYUVDownscaled(1) == nullptr);
    std::cout << "PASS: YUV golden bytes, 187 resampling fixtures, original pixels retained, and invalid layouts rejected\n";
}
