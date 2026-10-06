#ifndef POSTPROCESSOR_OPTIMIZER_H
#define POSTPROCESSOR_OPTIMIZER_H

#include <data/mesh.h>
#include <gl/opengl.h>

namespace oc {

    class Optimizer {
    public:
        // Object-only OBJ normalization: largest valid face by perimeter, its
        // negative winding normal becomes +Y, then X/Z AABB-center and floor Y.
        // This does not update captured camera poses or solve scan registration.
        // Invalid/unsupported input is left unchanged; Process reports via LOGI.
        void Process(std::string filename);
    private:
        bool CalculateRotation(const std::vector<Mesh>& data, glm::dmat4& matrix);
        bool Finish(const std::vector<Mesh>& data, const std::string& filename,
                    const glm::dmat4& matrix);
    };
}
#endif
