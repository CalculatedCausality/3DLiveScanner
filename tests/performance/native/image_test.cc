// Actual common/data/image.cc: pixel equivalence and allocation regression.
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>
#include "data/image.h"

static size_t arrayAllocations = 0;
void* operator new[](size_t size) {
    ++arrayAllocations;
    void* p = std::malloc(size ? size : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete[](void* p) noexcept { std::free(p); }

int main() {
    unsigned cases = 0;
    for (int w : {1, 2, 7, 16, 641}) {
        for (int h : {1, 2, 9, 16, 479}) {
            oc::Image image(w, h);
            const size_t bytes = size_t(w) * h * 4;
            std::vector<unsigned char> original(bytes);
            for (size_t i = 0; i < bytes; ++i) original[i] = (i * 71 + i / 11) & 255;
            std::memcpy(image.GetData(), original.data(), bytes);
            unsigned char* const address = image.GetData();
            const size_t allocations = arrayAllocations;
            image.UpsideDown();
            assert(arrayAllocations == allocations);
            assert(image.GetData() == address);
            for (int y = 0; y < h; ++y)
                assert(!std::memcmp(image.GetData() + size_t(y) * w * 4,
                                    original.data() + size_t(h - 1 - y) * w * 4, w * 4));
            image.UpsideDown();
            assert(arrayAllocations == allocations);
            assert(!std::memcmp(image.GetData(), original.data(), bytes));
            for (int s : {1, 2, 3, 4}) {
                if (w < s || h < s) continue;
                oc::Image* small = image.Downscale(s);
                assert(small->GetWidth() == w / s && small->GetHeight() == h / s);
                for (int y = 0; y < h / s; ++y)
                    for (int x = 0; x < w / s; ++x)
                        assert(!std::memcmp(small->GetData() + (y * (w / s) + x) * 4,
                                            original.data() + (y * s * w + x * s) * 4, 4));
                delete small;
                ++cases;
            }
        }
    }
    std::printf("PASS: 25 flip/roundtrip cases, %u downscales, zero flip new[] allocations\n", cases);
}
