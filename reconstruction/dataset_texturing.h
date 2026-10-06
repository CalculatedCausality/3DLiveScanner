// SPDX-License-Identifier: Apache-2.0
#ifndef SCANNER_DATASET_TEXTURING_H
#define SCANNER_DATASET_TEXTURING_H
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace oc {
class Dataset;
struct DatasetTextureSettings {
    int textureSize = 2048;
    int textureCount = 4; // 0 selects the maximum within the 64 MiB budget.
    // Optional existing writable parent for disposable vertex/face scratch only.
    // Empty uses destination-side staging; no platform /tmp default. The exporter
    // creates/removes its own exclusive child, never the parent or its contents.
    // An invalid explicit directory fails rather than silently falling back.
    std::string scratchDirectory;
};
struct DatasetTextureReport {
    std::string error;
    uint64_t faces = 0, observedFaces = 0, vertices = 0, normals = 0;
    uint64_t scratchBytes = 0;
    int frames = 0, usedFrames = 0, sourceWidth = 0, sourceHeight = 0;
    int atlasPages = 0, tileWidth = 0, tileHeight = 0;
    double photoScale = 0, photoScaleX = 0, photoScaleY = 0, seconds = 0;
    double surfaceArea = 0, observedArea = 0;
    // Populated only after publication. Includes outputObj, MTL and PNGs.
    std::vector<std::string> artifacts;
};
// Progress runs synchronously on the caller's thread: (stage, fraction), with a
// monotonic fraction in [0,1] across parsing, visibility and writing. Return false
// to cancel; throwing also reports failure. An empty callback is valid. Fraction
// 1 is emitted just BEFORE publication: success depends on the returned bool.
// Input must be a regular triangular scanner OBJ
// in canonical app coordinates. COLOR_CAMERA is camera-to-world (+Z forward,
// +Y down). No orientation/scale change, decimation, or source mutation.
// Output must differ from input (including hard/symbolic aliases). Existing
// output is replaced atomically ONLY once all unique resources are complete.
// Serialize with other legacy Image operations (Image's PNG codec is global).
typedef std::function<bool(const std::string&,double)> DatasetTextureProgress;
bool ExportDatasetTexturedObj(const std::string& inputObj,
                             const std::string& outputObj,
                             Dataset* dataset,
                             const DatasetTextureSettings& settings,
                             const DatasetTextureProgress& progress,
                             DatasetTextureReport& report);
}
#endif
