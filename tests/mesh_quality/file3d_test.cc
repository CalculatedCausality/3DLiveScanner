#include <data/file3d.h>
#include <exporter/ply.h>
#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <unistd.h>

using oc::File3d;
using oc::Mesh;
static std::string directory;
static std::string Path(const std::string& name) { return directory + "/" + name; }
static void Put(const std::string& name, const std::string& text) {
    std::ofstream file(Path(name)); file << text; assert(file.good());
}
static std::string Get(const std::string& name) {
    std::ifstream file(Path(name));
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}
static void Destroy(std::vector<Mesh>& meshes) { for (Mesh& m : meshes) m.Destroy(); }
static size_t Corners(const std::vector<Mesh>& meshes) {
    size_t n = 0;
    for (const Mesh& m : meshes) {
        assert(m.indices.empty()); // Loader's runtime contract stays flat.
        n += m.vertices.size();
    }
    return n;
}
static Mesh Triangle(float z = 0) {
    Mesh m;
    m.vertices = {glm::vec3(0, 0, z), glm::vec3(1, 0, z), glm::vec3(0, 1, z)};
    return m;
}
static bool Write(const std::string& name, std::vector<Mesh>& meshes, bool faces = true) {
    return File3d(Path(name), true).WriteModel(meshes, faces);
}
static std::vector<Mesh> Read(const std::string& name, int subdivision = INT_MAX) {
    std::vector<Mesh> output;
    File3d(Path(name), false).ReadModel(subdivision, output);
    return output;
}

// Independent inspection: do not use the production reader to validate counts/indices.
static void InspectPly(const std::string& name, size_t vertices, size_t faces, size_t columns) {
    std::istringstream file(Get(name));
    std::string line;
    size_t declaredV = 0, declaredF = 0;
    while (std::getline(file, line) && line != "end_header") {
        if (line.find("element vertex ") == 0) declaredV = std::stoul(line.substr(15));
        if (line.find("element face ") == 0) declaredF = std::stoul(line.substr(13));
    }
    assert(declaredV == vertices && declaredF == faces);
    for (size_t i = 0; i < vertices; ++i) {
        assert(std::getline(file, line));
        std::istringstream row(line); double value; size_t n = 0;
        while (row >> value) { assert(std::isfinite(value)); ++n; }
        assert(n == columns);
    }
    for (size_t i = 0; i < faces; ++i) {
        assert(std::getline(file, line));
        std::istringstream row(line); size_t n, a, b, c;
        assert(row >> n >> a >> b >> c);
        assert(n == 3 && a < vertices && b < vertices && c < vertices);
    }
    assert(!std::getline(file, line));
}

static void IndexedAndMixedPly() {
    Mesh indexed;
    indexed.vertices = {glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), glm::vec3(1, 1, 0), glm::vec3(0, 1, 0)};
    indexed.indices = {0, 1, 2, 0, 2, 3};
    indexed.normals.assign(4, glm::vec3(0, 0, 1));
    indexed.colors = {0x0000ff, 0x00ff00, 0xff0000, 0xffffff};
    std::vector<Mesh> meshes = {indexed, Triangle(2)};
    assert(Write("mixed.ply", meshes));
    InspectPly("mixed.ply", 7, 3, 9);
    assert(meshes[0].vertices.size() == 4 && meshes[0].indices == indexed.indices);
    auto read = Read("mixed.ply");
    assert(Corners(read) == 9 && read.size() == 1);
    for (size_t i = 0; i < 6; ++i) {
        assert(read[0].vertices[i] == indexed.vertices[indexed.indices[i]]);
        assert(read[0].normals[i] == glm::vec3(0, 0, 1));
        assert(read[0].colors[i] == indexed.colors[indexed.indices[i]]);
    }
    for (size_t i = 6; i < 9; ++i) {
        assert(read[0].normals[i] == glm::vec3(0));
        assert(read[0].colors[i] == 0);
    }
    auto points = Read("mixed.ply", -1);
    assert(Corners(points) == 7 && points[0].normals.size() == 7);
    assert(Write("cloud.ply", meshes, false));
    InspectPly("cloud.ply", 7, 0, 9);
    Destroy(points); Destroy(read);
}

static void ObjOffsetsAndMaterials() {
    std::vector<Mesh> meshes(5);
    for (size_t i = 0; i < meshes.size(); ++i) meshes[i] = Triangle(i);
    meshes[1].normals.assign(3, glm::vec3(0, 0, 1));
    meshes[2].uv = {glm::vec2(0, 0), glm::vec2(1, 0), glm::vec2(0, 1)};
    meshes[3].normals.assign(3, glm::vec3(0, 0, -1));
    meshes[3].uv = meshes[2].uv;
    oc::Image texture(255, 255, 255, 255);
    texture.SetName("same.png");
    meshes[0].image = &texture;
    meshes[2].image = &texture;
    assert(Write("offsets.obj", meshes));
    std::string obj = Get("offsets.obj");
    assert(obj.find("f 1 2 3\n") != std::string::npos);
    assert(obj.find("f 4//1 5//2 6//3\n") != std::string::npos);
    assert(obj.find("f 7/1 8/2 9/3\n") != std::string::npos);
    assert(obj.find("f 10/4/4 11/5/5 12/6/6\n") != std::string::npos);
    assert(obj.find("usemtl 4\nf 13 14 15\n") != std::string::npos);
    auto read = Read("offsets.obj");
    assert(Corners(read) == 15);
    size_t j = 0;
    for (const Mesh& m : read) {
        if (m.vertices.empty()) continue;
        assert(m.vertices == meshes[j].vertices);
        if (!meshes[j].normals.empty()) assert(m.normals == meshes[j].normals);
        if (!meshes[j].uv.empty()) assert(m.uv == meshes[j].uv);
        ++j;
    }
    assert(j == 5);
    Destroy(read);
    for (const std::string name : {"folder//same.png", "/same.png", "folder/", "C:\\same.png", ""}) {
        texture.SetName(name);
        assert(Write("offsets.obj", meshes));
        std::string mtl = Get("offsets.mtl");
        std::string expected = name.empty() ? "" : "map_Kd " +
            (name == "folder/" ? "" : name == "C:\\same.png" ? name : "same.png") + "\n\n";
        assert(expected.empty() ? mtl.find("map_Kd") == std::string::npos :
               mtl.find(expected) != std::string::npos);
    }
}

static void DegenerateAndPrecision() {
    Mesh m = Triangle();
    m.vertices.insert(m.vertices.end(), {glm::vec3(0), glm::vec3(1, 0, 0), glm::vec3(2, 0, 0)});
    // A tiny, genuine surface must survive, even when float cross-product length underflows.
    m.vertices.insert(m.vertices.end(), {glm::vec3(0), glm::vec3(1e-20f, 0, 0), glm::vec3(0, 1e-20f, 0)});
    // Opposite winding/coincident faces may be intentional. Preserve them.
    m.vertices.insert(m.vertices.end(), {glm::vec3(0), glm::vec3(0, 1, 0), glm::vec3(1, 0, 0)});
    std::vector<Mesh> meshes = {m};
    assert(Write("degenerate.ply", meshes));
    InspectPly("degenerate.ply", 12, 3, 3);
    auto read = Read("degenerate.ply");
    assert(Corners(read) == 9);
    std::vector<glm::vec3> expected(m.vertices.begin(), m.vertices.begin() + 3);
    expected.insert(expected.end(), m.vertices.begin() + 6, m.vertices.end());
    assert(read[0].vertices == expected); // Every retained corner, scale and winding unchanged.
    assert(read[0].vertices[4].x == 1e-20f);
    Destroy(read);
    meshes = {Triangle(0.123456789f)};
    meshes[0].vertices[1].x = 0.000000123456789f;
    assert(Write("precision.obj", meshes));
    read = Read("precision.obj");
    for (const Mesh& mesh : read)
        if (!mesh.vertices.empty()) assert(mesh.vertices == meshes[0].vertices);
    assert(Corners(read) == 3);
    Destroy(read);
    for (glm::vec3& v : meshes[0].vertices) v = v * 0.123456789f + glm::vec3(123.456789f, -2.3456789f, 0.000000123456f);
    // Precision check on points: float coordinates round-trip exactly, no pose/scale change.
    assert(Write("precision.ply", meshes, false));
    read = Read("precision.ply", -1);
    assert(read[0].vertices == meshes[0].vertices);
    Destroy(read);
    std::cout << "PASS: zero-area fixture 4 -> 3 triangles; tiny and opposite-winding surfaces retained\n";
    std::cout << "PASS: coordinate round-trip maximum error = 0 (float equality)\n";
}

static void RejectMalformedOutput() {
    std::vector<Mesh> meshes = {Triangle()};
    meshes[0].indices = {0, 1, 99}; assert(!Write("bad.ply", meshes));
    meshes[0].indices = {0, 1}; assert(!Write("bad.obj", meshes));
    meshes[0].indices.clear(); meshes[0].normals.resize(1); assert(!Write("bad.ply", meshes));
    meshes[0].normals.clear(); meshes[0].uv.resize(1); assert(!Write("bad.obj", meshes));
    meshes[0].uv.clear(); meshes[0].vertices[0].x = std::numeric_limits<float>::infinity();
    assert(!Write("bad.ply", meshes, false));
    meshes[0] = Triangle(); meshes[0].vertices.push_back(glm::vec3(2));
    assert(!Write("bad.ply", meshes));
    assert(Write("points.ply", meshes, false)); InspectPly("points.ply", 4, 0, 3);
}

static void ObjInput() {
    Put("input.obj", "v\t0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
        "mtllib missing.mtl\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 1\n"
        "f -4/-4/1\t-3/-3/1 -2/-2/1 -1/-1/1 # quad\n"
        "f\t1 2 3\n" // Attributes declared globally but omitted on this face.
        "f 0 2 3\nf 1 2 90\nf 1/99 2/2 3/3\nf 1//99 2//1 3//1\n"
        "f 1 1 3\nf 1 2\nf 1 2 3 4 1\nf -999 2 3\nf 1/ 2/ 3/\n"
        "f 1//1 2/2 3\n");
    auto meshes = Read("input.obj");
    assert(Corners(meshes) == 12);
    for (const Mesh& m : meshes) {
        assert(m.vertices.size() == m.normals.size() && m.vertices.size() == m.uv.size());
        for (const glm::vec3& n : m.normals) assert(n == glm::vec3(0, 0, 1));
    }
    Destroy(meshes);
    meshes = Read("input.obj", 1);
    assert(Corners(meshes) == 12);
    for (const Mesh& m : meshes) assert(m.vertices.size() <= 3);
    Destroy(meshes);
    meshes = Read("input.obj", -1); assert(Corners(meshes) == 12); Destroy(meshes);
    assert(Read("nonexistent.obj").empty());
    Put("bare.obj", "mtllib");
    meshes = Read("bare.obj"); assert(Corners(meshes) == 0); Destroy(meshes);
    Put("nonfinite.obj", "v nan 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    meshes = Read("nonfinite.obj"); assert(Corners(meshes) == 0); Destroy(meshes);
    // Read a long face line as one record; do not create a second accidental face.
    Put("long.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf" + std::string(1500, ' ') + " 1 2 3\n");
    meshes = Read("long.obj"); assert(Corners(meshes) == 3); Destroy(meshes);
    assert(chdir(directory.c_str()) == 0);
    meshes = {Triangle()}; assert(File3d("relative.obj", true).WriteModel(meshes));
    assert(Get("relative.obj").find("mtllib relative.mtl\n") == 0);
    std::vector<Mesh> relative;
    File3d("relative.obj", false).ReadModel(INT_MAX, relative);
    assert(Corners(relative) == 3); Destroy(relative);
}

static std::string PlyHeader(int vertices, int faces) {
    return "ply\nformat ascii 1.0\nelement vertex " + std::to_string(vertices) +
        "\nproperty float x\nproperty float y\nproperty float z\nelement face " + std::to_string(faces) +
        "\nproperty list uchar int vertex_indices\nend_header\n";
}
static void PlyInput() {
    // These disconnected triangles have vertices that collide under the old %.3f key.
    Put("seams.ply", PlyHeader(6, 8) +
        "0 0 0\n1 0 0\n0 1 0\n0.0001 0 0\n0.0001 0 1\n0.0001 1 0\n"
        "3 0 1 2\n3 3 4 5\n3 -1 1 2\n3 0 1 999\n3 0 0 2\n4 0 1 2 3\n3 0 1\n");
    std::vector<Mesh> meshes = {Triangle(99)};
    File3d(Path("seams.ply"), false).ReadModel(1, meshes);
    assert(meshes.size() == 3 && meshes[0].normals.empty()); // Appended output untouched.
    assert(meshes[1].normals[0] == glm::vec3(0, 0, 1));
    assert(meshes[2].normals[0] == glm::vec3(-1, 0, 0));
    assert(Corners(meshes) == 9);
    Put("opposite.ply", PlyHeader(3, 2) + "0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n3 0 2 1\n");
    auto opposite = Read("opposite.ply");
    assert(Corners(opposite) == 6);
    assert(opposite[0].normals[0] == glm::vec3(0, 0, 1));
    assert(opposite[0].normals[3] == glm::vec3(0, 0, -1));
    Put("properties.ply", "ply\r\nformat ascii 1.0\r\nelement vertex 1\r\n"
        "property uchar blue\nproperty float z\nproperty float x\nproperty float y\n"
        "property uchar red\nproperty uchar green\nproperty float confidence\nend_header\n"
        "30 3 1 2 10 20 0.9\n");
    auto properties = Read("properties.ply");
    assert(properties[0].vertices[0] == glm::vec3(1, 2, 3));
    assert(properties[0].colors[0] == 0x1e140a);
    Put("truncated.ply", PlyHeader(3, 1) + "0 0 0\n1 0 0\n");
    assert(Read("truncated.ply").empty());
    Put("binary.ply", "ply\nformat binary_little_endian 1.0\nelement vertex 0\nend_header\n");
    assert(Read("binary.ply").empty());
    Destroy(meshes); Destroy(opposite); Destroy(properties);
}

static void ReusedExporter() {
    oc::Dataset dataset(directory + "/");
    oc::ExporterPLY exporter;
    exporter.Process(&dataset, Path("export1.ply"));
    exporter.Process(&dataset, Path("export2.ply"));
    InspectPly("export1.ply", 2, 0, 6);
    InspectPly("export2.ply", 2, 0, 6);
    assert(Get("export1.ply") == Get("export2.ply"));
    std::cout << "PASS: reused exporter emits 2 points per run (no accumulated duplicate frames)\n";
}

int main(int argc, char** argv) {
    assert(argc == 2); directory = argv[1];
    IndexedAndMixedPly(); ObjOffsetsAndMaterials(); DegenerateAndPrecision();
    RejectMalformedOutput(); ObjInput(); PlyInput(); ReusedExporter();
    std::cout << "PASS: mesh-quality production-code regressions\n";
}
