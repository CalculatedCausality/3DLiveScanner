// Actual common/thread/scene.cc; GL, GLSL and GLRenderer are instrumented mocks.
// This verifies pass state/draw submission, not shader execution or GPU pixels.
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include "thread/scene.h"

void glViewport(GLint, GLint, GLsizei, GLsizei) {}
void glDeleteTextures(GLsizei, const GLuint*) {}
void glActiveTexture(GLenum) {}
void glBindTexture(GLenum, GLuint) {}
void glDepthFunc(GLenum) {}
void glLineWidth(float) {}
void glEnable(GLenum) {}
void glDisable(GLenum) {}
void glBlendFunc(GLenum, GLenum) {}

static oc::GLSL* current;
static std::map<oc::GLSL*, std::map<std::string, unsigned>> writes;
static std::map<oc::GLSL*, std::map<std::string, float>> floats;
static std::map<oc::GLSL*, glm::mat4> matrices;
static unsigned draws, uploads, prepared;
static std::vector<const float*> drawn;
namespace oc {
    GLSL::GLSL(std::string, std::string, std::string) {}
    GLSL::~GLSL() {
        if (current == this) current = nullptr;
        matrices.erase(this);
        floats.erase(this);
    }
    void GLSL::AbandonGlContext() { if (current == this) current = nullptr; }
    void GLSL::Bind() { current = this; }
    GLSL* GLSL::CurrentShader() { return current; }
    void GLSL::UniformFloat(const char* n, float value) { ++writes[this][n]; floats[this][n] = value; }
    void GLSL::UniformInt(const char* n, int) { ++writes[this][n]; }
    void GLSL::UniformVec3(const char* n, float, float, float) { ++writes[this][n]; }
    void GLSL::UniformMatrix(const char* n, const float* value) {
        ++writes[this][n];
        matrices[this] = glm::make_mat4(value);
    }
    GLuint GLSL::Image2GLTexture(Image*, bool) { ++uploads; return 42; }
    GLRenderer::GLRenderer() { camera.position = glm::vec3(0); }
    GLRenderer::~GLRenderer() {}
    void GLRenderer::AbandonGlContext() {}
    void GLRenderer::Init(int, int, int, int) {}
    void GLRenderer::PrepareRender() {
        ++prepared;
        const glm::mat4 m(1);
        current->UniformMatrix("MVP", glm::value_ptr(m));
    }
    void GLRenderer::RenderPrepared(float* v, float*, float*, unsigned int*, unsigned long size, unsigned int*, int) {
        assert(size == 3 && v && current && matrices.count(current));
        drawn.push_back(v);
        ++draws;
    }
    void GLRenderer::Render(float* v, float* n, float* uv, unsigned int* c, unsigned long size, unsigned int* i, int type) {
        PrepareRender();
        RenderPrepared(v, n, uv, c, size, i, type);
    }
}
static void resetCounts() { writes.clear(); draws = prepared = 0; drawn.clear(); }
static void verifyDrawOrder(oc::Scene& scene) {
    assert(draws == 100);
    for (unsigned i = 0; i < drawn.size(); ++i)
        assert(drawn[i] == &scene.static_meshes_[i].vertices[0].x);
}
int main() {
    oc::Image image(255, 255, 255, 255);
    image.SetTexture(42);
    oc::Scene scene;
    scene.renderer = new oc::GLRenderer();
    scene.uniform = scene.uniformPitch = 0;
    scene.uniformPos = glm::vec3(0);
    // Alternate shaders to ensure hoisting preserves per-program state.
    for (int i = 0; i < 100; ++i) {
        oc::Mesh m;
        m.vertices.assign(3, glm::vec3(i));
        m.normals.assign(3, glm::vec3(0, 1, 0));
        m.uv.assign(3, glm::vec2(0));
        m.colors.assign(3, 0xffffffff);
        if (i % 2) m.image = &image;
        scene.static_meshes_.push_back(m);
    }
    scene.static_meshes_.push_back(oc::Mesh()); // Must not dereference empty vectors.
    scene.Render(false);
    verifyDrawOrder(scene);
    assert(prepared == 2 && writes[scene.textured_shader]["MVP"] == 1);
    assert(writes[scene.textured_shader]["u_texture"] == 1);
    assert(writes[scene.textured_shader]["u_uniformPos"] == 1);
    assert(writes[scene.color_vertex_shader]["MVP"] == 1);
    resetCounts();
    scene.uniform = 4;
    scene.Render(false);
    verifyDrawOrder(scene);
    assert(prepared == 2 && writes[scene.textured_shader]["u_uniform"] == 1);
    assert(floats[scene.textured_shader]["u_uniform"] == 4);
    resetCounts();
    glm::mat4 custom(2);
    scene.CustomRender(custom);
    verifyDrawOrder(scene);
    assert(prepared == 1 && matrices[scene.textured_shader] == custom);
    assert(writes[scene.textured_shader]["MVP"] == 1);
    assert(writes[scene.color_vertex_shader]["u_uniformCamera"] == 1);
    scene.AbandonGlContext();
    assert(!scene.renderer && !scene.textured_shader && image.GetTexture() == -1);
    resetCounts();
    scene.renderer = new oc::GLRenderer();
    scene.Render(false);
    verifyDrawOrder(scene);
    assert(uploads == 1 && prepared == 2);
    std::puts("PASS: 100 interleaved draws, per-pass uniforms, custom matrix, empty mesh, context reset/reupload (mock GL)");
}
