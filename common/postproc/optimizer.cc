#include <data/file3d.h>
#include <postproc/optimizer.h>

#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace oc {
    namespace {
        bool Finite(const glm::dvec3& v) {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        bool Rigid(const glm::dmat4& matrix) {
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    if (!std::isfinite(matrix[i][j])) return false;
            if (matrix[3] != glm::dvec4(0, 0, 0, 1) ||
                matrix[0][3] != 0 || matrix[1][3] != 0 || matrix[2][3] != 0) return false;
            const glm::dmat3 rotation(matrix);
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    if (std::abs(glm::dot(rotation[i], rotation[j]) - (i == j ? 1.0 : 0.0)) > 1e-12)
                        return false;
            return std::abs(glm::determinant(rotation) - 1.0) <= 1e-12;
        }
    }

    void Optimizer::Process(std::string filename) {
        // The only caller supplies an OBJ path. Never reinterpret a PLY/PCL as OBJ.
        if (filename.size() < 4 || filename.substr(filename.size() - 4) != ".obj") return;
        std::vector<Mesh> data;
        File3d(filename, false).ReadModel(std::numeric_limits<int>::max(), data);

        glm::dmat4 matrix(1);
        const bool valid = CalculateRotation(data, matrix) && Finish(data, filename, matrix);
        for (Mesh& mesh : data) mesh.Destroy();
        if (!valid) {
            LOGI("Object alignment skipped: no valid rigid transform or OBJ rewrite failed");
        }
    }

    bool Optimizer::CalculateRotation(const std::vector<Mesh>& data, glm::dmat4& matrix) {
        // Retain the existing largest-perimeter rule and winding sign. Derive
        // normals from geometry, not supplied (possibly zero/nonfinite) normals.
        double best = 0;
        glm::dvec3 normal(0);
        for (const Mesh& mesh : data) {
            // File3d's OBJ loader returns flat triangles. Do not mutate this data.
            for (size_t i = 0; i + 2 < mesh.vertices.size(); i += 3) {
                const glm::dvec3 a(mesh.vertices[i]), b(mesh.vertices[i + 1]), c(mesh.vertices[i + 2]);
                if (!Finite(a) || !Finite(b) || !Finite(c)) continue;
                const glm::dvec3 cross = glm::cross(b - a, c - a);
                const double length = glm::length(cross);
                if (!(length > 0) || !std::isfinite(length)) continue;
                const double perimeter = glm::length(b - a) + glm::length(c - a) + glm::length(c - b);
                if (perimeter > best && std::isfinite(perimeter)) {
                    best = perimeter;
                    normal = -cross / length;
                }
            }
        }
        if (!(best > 0)) return false;

        glm::dvec3 xaxis = glm::cross(glm::dvec3(0, 1, 0), normal);
        // Y-parallel normals have no heading from cross(up, normal). In the
        // float-input uncertainty band use projected world +X, a deterministic
        // roll choice (+Y yields identity, -Y a half-turn about X).
        const double poleTolerance = 8.0 * std::numeric_limits<float>::epsilon();
        if (glm::length(xaxis) <= poleTolerance)
            xaxis = glm::dvec3(1, 0, 0) - normal * normal.x;
        xaxis = glm::normalize(xaxis);
        const glm::dvec3 yaxis = glm::normalize(glm::cross(normal, xaxis));

        // Include the historic (x,z,-y) conversion in this ONE rotation. Bounds,
        // positions and normals must all use exactly the same coordinate frame.
        matrix = glm::dmat4(1);
        for (int i = 0; i < 3; ++i) {
            matrix[i][0] = xaxis[i];
            matrix[i][1] = normal[i];
            matrix[i][2] = -yaxis[i];
        }
        return Rigid(matrix);
    }

    bool Optimizer::Finish(const std::vector<Mesh>& data, const std::string& filename,
                           const glm::dmat4& matrix) {
        if (!Rigid(matrix)) return false;
        glm::dvec3 min(std::numeric_limits<double>::infinity());
        glm::dvec3 max(-std::numeric_limits<double>::infinity());
        bool hasVertex = false;
        for (const Mesh& mesh : data) {
            for (const glm::vec3& vertex : mesh.vertices) {
                const glm::dvec3 v(matrix * glm::dvec4(vertex, 1));
                if (!Finite(v)) return false;
                min = glm::min(min, v);
                max = glm::max(max, v);
                hasVertex = true;
            }
        }
        if (!hasVertex) return false;
        // Keep the existing floor-centering policy, not an arbitrary new origin.
        glm::dvec3 center = min * 0.5 + max * 0.5;
        center.y = min.y;

        std::ifstream input(filename);
        if (!input) return false;
        std::string temporary = filename + ".align-XXXXXX";
        std::vector<char> name(temporary.begin(), temporary.end());
        name.push_back('\0');
        int fd = mkstemp(name.data());
        if (fd < 0) return false;
        FILE* output = fdopen(fd, "w");
        if (!output) { close(fd); unlink(name.data()); return false; }
        struct stat status;
        bool ok = stat(filename.c_str(), &status) == 0 && fchmod(fd, status.st_mode & 0777) == 0;

        // Stream into a sibling temporary file: malformed coordinates, failed
        // writes or overflow must not leave the original OBJ half transformed.
        std::string line;
        while (ok && std::getline(input, line)) {
            const bool newline = !input.eof();
            std::istringstream record(line);
            record.imbue(std::locale::classic());
            std::string kind;
            record >> kind;
            if (kind == "v" || kind == "vn") {
                glm::dvec3 p;
                if (!(record >> p.x >> p.y >> p.z) || !Finite(p)) { ok = false; break; }
                std::string suffix;
                std::getline(record, suffix);
                if (kind == "v") {
                    // File3d does not dehomogenize v x y z w. Do not silently
                    // translate weighted coordinates. RGB(A) suffixes survive.
                    std::istringstream extra(suffix);
                    extra.imbue(std::locale::classic());
                    double value;
                    std::vector<double> values;
                    while (extra >> value) values.push_back(value);
                    if ((values.size() == 1 && values[0] != 1.0) || values.size() == 2) {
                        ok = false;
                        break;
                    }
                }
                const glm::dvec3 transformed = glm::dvec3(matrix * glm::dvec4(p, 0)) -
                                               (kind == "v" ? center : glm::dvec3(0));
                const double limit = std::numeric_limits<float>::max();
                if (!Finite(transformed) || std::abs(transformed.x) > limit ||
                    std::abs(transformed.y) > limit || std::abs(transformed.z) > limit) {
                    ok = false;
                    break;
                }
                std::ostringstream text;
                text.imbue(std::locale::classic());
                text << std::setprecision(17) << kind << ' ' << transformed.x << ' '
                     << transformed.y << ' ' << transformed.z << suffix;
                line = text.str();
            }
            if (fwrite(line.data(), 1, line.size(), output) != line.size() ||
                (newline && fputc('\n', output) == EOF)) ok = false;
        }
        if (input.bad()) ok = false;
        if (fflush(output) != 0 || fsync(fd) != 0) ok = false;
        if (fclose(output) != 0) ok = false;
        input.close();
        if (ok && rename(name.data(), filename.c_str()) == 0) return true;
        unlink(name.data());
        return false;
    }
}
