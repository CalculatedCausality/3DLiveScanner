// SPDX-License-Identifier: Apache-2.0
#ifndef SCANNER_EXPERIMENTAL_DEPTH_H
#define SCANNER_EXPERIMENTAL_DEPTH_H
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace oc { namespace depth_test {
struct Link {
    uint32_t pixel = 0, point = 0;
    float depth = 0;
    float minimumDepth = .05f;
    glm::vec4 original = glm::vec4(0);
    glm::dvec3 worldPerMetre = glm::dvec3(0);
};
struct Frame {
    int width = 0, height = 0;
    int64_t cameraTimestamp = 0, depthTimestamp = 0;
    uint64_t generation = 0;
    size_t pointCount = 0;
    glm::dmat4 worldToCamera = glm::dmat4(1);
    double minDepth = 0, maxDepth = 1e9;
    std::vector<float> depth, confidence;
    std::vector<Link> links;
};
struct Stats {
    uint32_t eligible = 0, changed = 0;
    double milliseconds = 0;
    bool inferred = false;
};
// Configure/Invalidate/Status are nonblocking with respect to NNAPI execution.
// Apply is worker-only and owns its state until execution finishes. It changes
// points transactionally only after success and a matching generation check.
class Runtime {
public:
    Runtime();
    ~Runtime();
    void SetEnabled(bool enabled);
    void Invalidate();
    bool Enabled() const;
    uint64_t Generation() const;
    std::string Status() const;
    void Fallback(const std::string& reason);
    bool Apply(const Frame& frame, std::vector<glm::vec4>& points, Stats& stats);
private:
    struct State;
    std::shared_ptr<State> state_;
};
} }
#endif
