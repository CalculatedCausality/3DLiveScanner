#include <data/dataset.h>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>
#include <unistd.h>

struct Segment {
    Tango3DR_Mesh mesh{};
    std::unique_ptr<Tango3DR_Vector3[]> vertices, normals;
    std::unique_ptr<Tango3DR_Color[]> colors;
    std::unique_ptr<Tango3DR_Face[]> faces;
    Segment(unsigned count, unsigned seed)
        : vertices(new Tango3DR_Vector3[count]), normals(new Tango3DR_Vector3[count]),
          colors(new Tango3DR_Color[count]), faces(new Tango3DR_Face[count]) {
        mesh.num_vertices = mesh.num_faces = count;
        mesh.vertices = vertices.get(); mesh.normals = normals.get();
        mesh.colors = colors.get(); mesh.faces = faces.get();
        for (unsigned i = 0; i < count; ++i) {
            for (int axis = 0; axis < 3; ++axis) {
                vertices[i][axis] = float((i * 17 + axis + seed) % 251) / 31;
                normals[i][axis] = axis == 1 ? 1 : 0;
                faces[i][axis] = (i + axis) % count;
            }
            for (int channel = 0; channel < 4; ++channel) colors[i][channel] = (i + channel + seed) % 256;
        }
    }
};

static void append(std::vector<char>& bytes, const void* source, size_t count) {
    if (!count) return;
    const char* data = static_cast<const char*>(source);
    bytes.insert(bytes.end(), data, data + count);
}

static void check(oc::Dataset& dataset, unsigned segments, unsigned vertices, int index) {
    std::vector<std::unique_ptr<Segment>> owned;
    std::vector<std::pair<oc::GridIndex, Tango3DR_Mesh*>> preview;
    for (unsigned i = 0; i < segments; ++i) {
        owned.emplace_back(new Segment(vertices, i));
        oc::GridIndex grid{};
        grid.indices[0] = i; grid.indices[1] = -int(i); grid.indices[2] = 7;
        preview.emplace_back(grid, &owned.back()->mesh);
    }
    assert(dataset.WritePreview(index, preview));
    std::vector<char> expected;
    int count = segments;
    append(expected, &count, sizeof(count));
    for (const auto& item : preview) append(expected, item.first.indices, sizeof(Tango3DR_GridIndex));
    for (const auto& item : preview) {
        const auto& m = *item.second;
        append(expected, &m.num_faces, sizeof(uint32_t));
        append(expected, &m.num_vertices, sizeof(uint32_t));
        append(expected, m.vertices, sizeof(Tango3DR_Vector3) * m.num_vertices);
        append(expected, m.normals, sizeof(Tango3DR_Vector3) * m.num_vertices);
        append(expected, m.colors, sizeof(Tango3DR_Color) * m.num_vertices);
        append(expected, m.faces, sizeof(Tango3DR_Face) * m.num_faces);
    }
    std::ifstream input(dataset.GetFileName(index, ".bin"), std::ios::binary);
    const std::vector<char> actual((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    assert(actual == expected);
    bool valid = false;
    auto restored = dataset.ReadPreview(index, false, &valid);
    assert(valid && restored.size() == preview.size());
    for (size_t i = 0; i < restored.size(); ++i) {
        Tango3DR_Mesh* m = restored[i].second;
        assert(std::memcmp(restored[i].first.indices, preview[i].first.indices, sizeof(Tango3DR_GridIndex)) == 0);
        assert(m->num_vertices == vertices && m->num_faces == vertices);
        assert(std::memcmp(m->vertices, preview[i].second->vertices, sizeof(Tango3DR_Vector3) * vertices) == 0);
        assert(std::memcmp(m->normals, preview[i].second->normals, sizeof(Tango3DR_Vector3) * vertices) == 0);
        assert(std::memcmp(m->colors, preview[i].second->colors, sizeof(Tango3DR_Color) * vertices) == 0);
        assert(std::memcmp(m->faces, preview[i].second->faces, sizeof(Tango3DR_Face) * vertices) == 0);
        delete[] m->vertices; delete[] m->normals; delete[] m->colors; delete[] m->faces; delete m;
    }
}

int main(int argc, char** argv) {
    oc::Dataset dataset(argv[1]);
    if (argc > 2) {
        check(dataset, 64, 512, 0);
        return 0;
    }
    check(dataset, 0, 0, 0);
    check(dataset, 1, 0, 1);
    check(dataset, 1, 3, 2);
    check(dataset, 64, 512, 3);
    check(dataset, 64, 512, 3); // Reusing the path must not leave a tail or alter bytes.
    std::vector<std::pair<oc::GridIndex, Tango3DR_Mesh*>> empty;
    const std::string failure = dataset.GetFileName(9, ".bin");
    assert(symlink("/dev/full", failure.c_str()) == 0);
    assert(!dataset.WritePreview(9, empty)); // Buffered success cannot hide final flush failure.
    unlink(failure.c_str());
    std::cout << "PASS: preview bytes/readback, empty/deleted segments, repeat writes, and flush failure\n";
}
