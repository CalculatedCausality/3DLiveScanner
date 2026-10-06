// Standalone on-device export verification. Does not load or restart the app.
#include "dataset_texturing.h"
#include <data/dataset.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/resource.h>

static uint64_t faces(const std::string& path, uint64_t* materials=nullptr) {
    FILE* input = fopen(path.c_str(), "rb");
    if (!input) return 0;
    setvbuf(input, nullptr, _IOFBF, 1u << 20);
    char line[8192]; uint64_t count = 0;
    while (fgets(line, sizeof(line), input)) {
        if (!strchr(line, '\n') && !feof(input)) { fclose(input); return 0; }
        const char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (p[0] == 'f' && (p[1] == ' ' || p[1] == '\t')) ++count;
        if (materials && !strncmp(p,"usemtl ",7)) ++*materials;
    }
    bool okay = !ferror(input); fclose(input);
    return okay ? count : 0;
}

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    const std::string source = argv[2], output = argv[3];
    oc::Dataset dataset(argv[1]);
    oc::DatasetTextureSettings options;
    options.textureSize = 2048; options.textureCount = 4;
    options.scratchDirectory = output.substr(0, output.find_last_of('/'));
    oc::DatasetTextureReport report;
    const auto start = std::chrono::steady_clock::now();
    bool okay = oc::ExportDatasetTexturedObj(source, output, &dataset, options,
        [](const std::string& stage, double fraction) {
            fprintf(stderr, "EXPORT_PROGRESS %s %.1f%%\n", stage.c_str(), fraction*100);
            fflush(stderr);
            return true;
        }, report);
    const double milliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    struct rusage usage{}; getrusage(RUSAGE_SELF, &usage);
    if (!okay) { fprintf(stderr, "EXPORT_FAILED %s\n", report.error.c_str()); return 3; }
    uint64_t materials=0;
    const uint64_t before = faces(source), after = faces(output,&materials);
    if (!before || before != after || report.faces != after) {
        fprintf(stderr, "EXPORTED_FACE_COUNT_MISMATCH\n"); return 4;
    }
    if (materials>uint64_t(report.atlasPages+1)) { fprintf(stderr,"EXCESS_MATERIAL_SWITCHES\n");return 5; }
    printf("{\"success\":true,\"input_faces\":%llu,\"output_faces\":%llu,"
           "\"textured_faces\":%llu,\"export_ms\":%.3f,\"peak_rss_kib\":%ld,"
           "\"atlas_pages\":%d,\"tile_width\":%d,\"tile_height\":%d,\"photo_scale\":%.6f,\"scratch_bytes\":%llu,"
           "\"surface_area\":%.9f,\"textured_area\":%.9f,\"material_runs\":%llu}\n",
           (unsigned long long)before, (unsigned long long)after,
           (unsigned long long)report.observedFaces, milliseconds, usage.ru_maxrss,
           report.atlasPages, report.tileWidth, report.tileHeight, report.photoScale,
           (unsigned long long)report.scratchBytes, report.surfaceArea, report.observedArea,
           (unsigned long long)materials);
    return 0;
}
