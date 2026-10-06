#include <postproc/optimizer.h>
#include <cassert>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <sys/stat.h>

using glm::dvec3;
using glm::dmat3;
static std::string directory;
static double maxDistanceError = 0;
static double maxOrthogonalityError = 0;
static std::string Path(const std::string& name) { return directory + "/" + name + ".obj"; }
static void Put(const std::string& path, const std::string& text) {
    std::ofstream output(path); output << text; assert(output.good());
}
static std::string Get(const std::string& path) {
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}
static void Near(const dvec3& a, const dvec3& b, double tolerance = 1e-10) {
    assert(glm::length(a - b) <= tolerance);
}
struct Model {
    std::vector<dvec3> vertices, normals;
    std::vector<std::string> other;
};
static Model Parse(const std::string& text) {
    Model model;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream record(line);
        std::string kind; record >> kind;
        if (kind == "v" || kind == "vn") {
            dvec3 v;
            assert(record >> v.x >> v.y >> v.z);
            assert(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z));
            (kind == "v" ? model.vertices : model.normals).push_back(v);
        } else model.other.push_back(line);
    }
    return model;
}
static std::string Obj(const std::vector<dvec3>& vertices) {
    std::ostringstream output;
    output << std::setprecision(17) << "# keep metadata\n\nmtllib absent.mtl\n";
    for (const dvec3& v : vertices) output << "v " << v.x << ' ' << v.y << ' ' << v.z << '\n';
    output << "vn 0 0 0\nvn 2 3 4\nvt 0.25 0.75\nvp 1 2 3\n"
              "usemtl first\nf 1 2 3\nusemtl second\nf 4 5 6\nf 4 6 7\nf 4 7 5\n";
    return output.str();
}
static std::vector<dvec3> Shape(dvec3 normal, double scale = 1, dvec3 translation = dvec3(0)) {
    normal = glm::normalize(normal);
    dvec3 u = std::abs(normal.x) < 0.9 ? dvec3(1, 0, 0) : dvec3(0, 0, 1);
    u = glm::normalize(u - normal * glm::dot(u, normal));
    const dvec3 v = glm::cross(-normal, u);
    std::vector<dvec3> vertices = {dvec3(0), 10.0 * u, 10.0 * v,
                                  dvec3(0), dvec3(1, 0, 0), dvec3(0, 1, 0), dvec3(0, 0, 1)};
    // File3d stores floats. Use representable source positions when testing the
    // centering policy rather than conflating it with decimal-to-float error.
    for (dvec3& p : vertices) p = dvec3(glm::vec3(p * scale + translation));
    return vertices;
}
static dmat3 Legacy(dvec3 normal) {
    normal = glm::normalize(normal);
    const dvec3 x = glm::normalize(glm::cross(dvec3(0, 1, 0), normal));
    const dvec3 y = glm::normalize(glm::cross(normal, x));
    dmat3 matrix;
    for (int i = 0; i < 3; ++i) { matrix[i][0] = x[i]; matrix[i][1] = normal[i]; matrix[i][2] = -y[i]; }
    return matrix;
}
static Model Check(const std::string& name, const std::vector<dvec3>& source, const dmat3* expected = nullptr) {
    const std::string path = Path(name);
    const std::string original = Obj(source);
    Put(path, original);
    assert(chmod(path.c_str(), 0640) == 0);
    oc::Optimizer().Process(path);
    Model result = Parse(Get(path));
    const Model before = Parse(original);
    assert(result.vertices.size() == source.size());
    assert(result.normals.size() == before.normals.size() && result.other == before.other);
    struct stat status; assert(stat(path.c_str(), &status) == 0 && (status.st_mode & 0777) == 0640);

    // The last four vertices are an origin plus three axis probes. Recover the
    // actual exported linear transform independently of the optimizer internals.
    dmat3 rotation;
    for (int i = 0; i < 3; ++i) {
        const double distance = source[4 + i][i] - source[3][i];
        assert(distance > 0);
        rotation[i] = (result.vertices[4 + i] - result.vertices[3]) / distance;
    }
    for (int i = 0; i < 3; ++i) {
        if (expected) Near(rotation[i], (*expected)[i], 2e-6);
        for (int j = 0; j < 3; ++j) {
            double error = std::abs(glm::dot(rotation[i], rotation[j]) - (i == j ? 1.0 : 0.0));
            maxOrthogonalityError = std::max(maxOrthogonalityError, error);
            assert(error < 2e-11);
        }
    }
    assert(std::abs(glm::determinant(rotation) - 1.0) < 2e-11);
    Near(glm::normalize(glm::cross(result.vertices[1] - result.vertices[0],
                                   result.vertices[2] - result.vertices[0])), dvec3(0, -1, 0));
    dvec3 min(1e300), max(-1e300);
    for (const dvec3& v : result.vertices) { min = glm::min(min, v); max = glm::max(max, v); }
    const double extent = glm::length(max - min);
    assert(std::abs(min.y) < std::max(1e-30, extent * 1e-12));
    assert(std::abs(min.x + max.x) < std::max(1e-30, extent * 1e-12));
    assert(std::abs(min.z + max.z) < std::max(1e-30, extent * 1e-12));
    const dvec3 translation = result.vertices[3] - rotation * source[3];
    for (size_t i = 0; i < source.size(); ++i) {
        Near(result.vertices[i], rotation * source[i] + translation, std::max(1e-30, extent * 2e-11));
        for (size_t j = 0; j < source.size(); ++j) {
            const double distance = glm::length(source[i] - source[j]);
            if (distance == 0) { Near(result.vertices[i], result.vertices[j], 1e-30); continue; }
            const double error = std::abs(glm::length(result.vertices[i] - result.vertices[j]) / distance - 1.0);
            maxDistanceError = std::max(maxDistanceError, error);
            assert(error < 2e-11);
        }
    }
    // A zero normal remains zero; normals rotate without recentering or scaling.
    Near(result.normals[0], dvec3(0));
    Near(result.normals[1], rotation * before.normals[1]);
    return result;
}
static void InvalidIsUnchanged(const std::string& name, const std::string& text) {
    const std::string path = Path(name);
    Put(path, text);
    oc::Optimizer().Process(path);
    assert(Get(path) == text);
}

int main(int argc, char** argv) {
    assert(argc == 2); directory = argv[1];
    const dvec3 directions[] = {dvec3(1, 0, 0), dvec3(-1, 0, 0), dvec3(0, 0, 1),
                               dvec3(0, 0, -1), dvec3(1, 2, 3), dvec3(-2, 1, -3)};
    int n = 0;
    for (const dvec3& normal : directions) {
        const dmat3 expected = Legacy(normal);
        Check("ordinary" + std::to_string(n++), Shape(normal), &expected);
    }
    const dmat3 identity(1);
    dmat3 flip(1); flip[1][1] = flip[2][2] = -1;
    const auto up = Check("up", Shape(dvec3(0, 1, 0)), &identity);
    oc::Optimizer().Process(Path("up"));
    const auto upAgain = Parse(Get(Path("up")));
    assert(up.vertices == upAgain.vertices && up.normals == upAgain.normals);
    Check("down", Shape(dvec3(0, -1, 0)), &flip);
    Check("near-up", Shape(dvec3(1e-8, 1, -1e-8)), &identity);
    Check("near-down", Shape(dvec3(-1e-8, -1, 1e-8)), &flip);
    // Outside the documented polar uncertainty band, keep the old heading.
    const dvec3 nearPole(1e-4, 1, 2e-4);
    const dmat3 ordinaryPole = Legacy(nearPole);
    Check("outside-pole-band", Shape(nearPole), &ordinaryPole);

    const auto baseline = Check("base", Shape(dvec3(0, 0, 1)));
    const auto translated = Check("translated", Shape(dvec3(0, 0, 1), 1, dvec3(123, -456, 789)));
    const auto scaled = Check("scaled", Shape(dvec3(0, 0, 1), 16));
    for (size_t i = 0; i < baseline.vertices.size(); ++i) {
        Near(baseline.vertices[i], translated.vertices[i]);
        Near(16.0 * baseline.vertices[i], scaled.vertices[i]);
    }
    Check("tiny", Shape(dvec3(0, 0, 1), 1e-20));
    Check("large", Shape(dvec3(0, 0, 1), 1e20));

    // A much larger-perimeter collinear triangle must not poison selection.
    std::string valid = Obj(Shape(dvec3(0, 0, 1)));
    Put(Path("degenerate-plus-valid"), valid + "v 0 0 0\nv 10000 0 0\nv 20000 0 0\nf 8 9 10\n");
    oc::Optimizer().Process(Path("degenerate-plus-valid"));
    auto degenerate = Parse(Get(Path("degenerate-plus-valid")));
    for (size_t i = 0; i < baseline.vertices.size(); ++i) Near(degenerate.vertices[i], baseline.vertices[i]);
    // Preserve perimeter ranking, even when a different face has more area.
    Put(Path("perimeter-rule"), valid + "v 0 0 0\nv 100 0 0\nv 0 0 0.001\nf 8 9 10\n");
    oc::Optimizer().Process(Path("perimeter-rule"));
    const auto perimeter = Parse(Get(Path("perimeter-rule")));
    Near(perimeter.vertices[4] - perimeter.vertices[3], dvec3(1, 0, 0));
    Near(perimeter.vertices[5] - perimeter.vertices[3], dvec3(0, 1, 0));
    Near(perimeter.vertices[6] - perimeter.vertices[3], dvec3(0, 0, 1));
    InvalidIsUnchanged("degenerate-only", "v 0 0 0\nv 1 0 0\nv 2 0 0\nf 1 2 3\n");
    InvalidIsUnchanged("empty", "# no geometry\n\n");
    InvalidIsUnchanged("nonfinite-position", valid + "v nan 0 0\n");
    InvalidIsUnchanged("nonfinite-normal", valid + "vn inf 0 0\n");
    InvalidIsUnchanged("malformed", valid + "v 1 2\n");
    InvalidIsUnchanged("weighted", valid + "v 1 2 3 2\n");
    // A second rewrite failure after many valid records must leave all bytes intact.
    InvalidIsUnchanged("overflow", valid + "v 1e300 1e300 1e300\n");
    oc::Optimizer().Process(Path("missing"));

    Put(Path("suffix"), valid + "v 1 2 3 0.1 0.2 0.3 # color\n");
    oc::Optimizer().Process(Path("suffix"));
    assert(Get(Path("suffix")).find(" 0.1 0.2 0.3 # color\n") != std::string::npos);
    assert(Get(Path("suffix")).find("vp 1 2 3\n") != std::string::npos);
    Parse(Get(Path("suffix")));
    std::cout << std::setprecision(8) << "PASS: max relative pairwise-distance error " << maxDistanceError
              << "; max orthogonality error " << maxOrthogonalityError << '\n';
    std::cout << "PASS: ordinary/polar orientation, rigid geometry, scale, floor-origin, finite export and failure preservation\n";
}
