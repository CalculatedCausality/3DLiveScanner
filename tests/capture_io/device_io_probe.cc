// Synthetic Android filesystem/codec probe, not the application pipeline.
// Uses the production JPEG archive/settings. Never opens a captured scan.
#include <turbojpeg.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
static void require(bool ok, const char* operation) {
    if (!ok) throw std::runtime_error(operation);
}
struct Scratch {
    std::string directory;
    std::vector<std::string> files;
    explicit Scratch(const char* parent) {
        std::string pattern = std::string(parent) + "/.scanner-io-probe-XXXXXX";
        std::vector<char> name(pattern.begin(), pattern.end()); name.push_back(0);
        require(mkdtemp(name.data()) != nullptr, "create isolated scratch directory");
        directory = name.data();
    }
    std::string path(const std::string& name) {
        files.push_back(directory + "/" + name);
        return files.back();
    }
    ~Scratch() {
        for (const auto& file : files) unlink(file.c_str());
        rmdir(directory.c_str());
    }
};
static void write(const std::string& path, const void* data, size_t size, bool sync = false) {
    FILE* file = fopen(path.c_str(), "wb");
    require(file != nullptr, "open scratch file");
    std::vector<char> buffer;
    if (sync) { buffer.resize(256 * 1024); setvbuf(file, buffer.data(), _IOFBF, buffer.size()); }
    bool ok = fwrite(data, 1, size, file) == size;
    if (sync) ok = fflush(file) == 0 && fsync(fileno(file)) == 0 && ok;
    ok = fclose(file) == 0 && ok;
    require(ok, "write/flush scratch file");
}
static void report(const char* name, std::vector<double> values) {
    std::sort(values.begin(), values.end());
    double sum = 0; for (double v : values) sum += v;
    printf("%s mean_ms=%.3f median_ms=%.3f p95_ms=%.3f\n", name,
           sum / values.size(), values[values.size()/2], values[(values.size()-1)*95/100]);
}
int main(int argc, char** argv) {
    try {
        require(argc == 2 || argc == 3, "usage: device_io_probe EXISTING_SCRATCH_PARENT [parallel]");
        bool parallel = argc == 3 && std::string(argv[2]) == "parallel";
        Scratch scratch(argv[1]);
        const int width = 360, height = 640, count = 32;
        std::vector<unsigned char> rgba(width * height * 4), preview(1312004), cloud(4 + 9216 * 16);
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            size_t p = (y * width + x) * 4;
            rgba[p] = (x * 3 + y * 7) & 255; rgba[p+1] = (x ^ y) & 255;
            rgba[p+2] = (x + y * 2) & 255; rgba[p+3] = 255;
        }
        for (size_t i = 0; i < preview.size(); ++i) preview[i] = (i * 17 + i/251) & 255;
        for (size_t i = 0; i < cloud.size(); ++i) cloud[i] = (i * 23 + i/199) & 255;
        const std::string pose(432, '0'), timestamp("12345.6789\n"), state("32 360 640 180 320 500 500\n");
        tjhandle codec = tjInitCompress(); require(codec != nullptr, "JPEG initialization");
        std::vector<double> encode, image, metadata, previews, clouds, commits, totals;
        std::vector<unsigned char> reference;
        unsigned long jpegBytes = 0;
        for (int frame = -4; frame < count; ++frame) {
            auto frameStart = Clock::now();
            unsigned char* jpeg = nullptr; unsigned long size = 0;
            auto start = Clock::now();
            int status = tjCompress2(codec, rgba.data(), width, 0, height, TJPF_RGBA,
                &jpeg, &size, TJSAMP_444, 85, TJFLAG_FASTDCT | TJFLAG_FASTUPSAMPLE | TJFLAG_BOTTOMUP);
            double elapsed = ms(start);
            require(status == 0 && jpeg, "JPEG encode");
            if (reference.empty()) reference.assign(jpeg, jpeg + size);
            require(size == reference.size() && memcmp(jpeg, reference.data(), size) == 0,
                    "JPEG output changed across identical frames");
            jpegBytes = size;
            if (frame < 0) { tjFree(jpeg); continue; }
            encode.push_back(elapsed);
            const std::string prefix = std::to_string(frame);
            // Register paths on the main thread; only independent file contents
            // are written concurrently. There is never more than one writer task.
            const std::string jpgPath = scratch.path(prefix + ".jpg");
            const std::string posePath = scratch.path(prefix + ".mat");
            const std::string timePath = scratch.path(prefix + ".tms");
            const std::string previewPath = scratch.path(prefix + ".bin");
            const std::string cloudPath = scratch.path(prefix + ".pcl");
            auto imageWork = [&] {
                auto began = Clock::now(); write(jpgPath, jpeg, size);
                image.push_back(ms(began));
                began = Clock::now();
                write(posePath, pose.data(), pose.size());
                write(timePath, timestamp.data(), timestamp.size());
                metadata.push_back(ms(began));
            };
            std::future<void> writer;
            if (parallel) writer = std::async(std::launch::async, imageWork);
            else imageWork();
            start = Clock::now(); write(previewPath, preview.data(), preview.size(), true);
            previews.push_back(ms(start));
            start = Clock::now(); write(cloudPath, cloud.data(), cloud.size());
            clouds.push_back(ms(start));
            if (writer.valid()) writer.get();
            tjFree(jpeg);
            start = Clock::now();
            std::string temporary = scratch.path("state.tmp"), final = scratch.path("state.txt");
            write(temporary, state.data(), state.size());
            require(rename(temporary.c_str(), final.c_str()) == 0, "publish scratch state");
            commits.push_back(ms(start));
            totals.push_back(ms(frameStart));
        }
        tjDestroy(codec);
        printf("parallel=%d synthetic_frames=%d rgba=%dx%d jpeg_bytes=%lu preview_bytes=%zu cloud_bytes=%zu\n",
               parallel, count, width, height, jpegBytes, preview.size(), cloud.size());
        report("jpeg_encode", encode); report("jpeg_write", image); report("pose_timestamp", metadata);
        report("preview_write_sync", previews); report("cloud_write", clouds); report("state_publish", commits);
        report("total_persistence", totals);
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "probe failed: %s\n", error.what()); return 1;
    }
}
