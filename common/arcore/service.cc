#include <arcore/service.h>
#include <data/dataset.h>

namespace oc {

    ARCoreService::ARCoreService(void *env, void *context, Mode mode, bool flashlight) {
        renderer = new GLRenderer();
        backend = nullptr;
        mode_ = mode;

#if !SCANNER_MODERN
        if (mode >= HUAWEI_SFM)
            backend = new AREngine(env, context, mode == Mode::HUAWEI_TOF, mode == Mode::HUAWEI_FACE, flashlight);
        else
#else
        // Guard stale callers as well as Java: never construct a missing provider.
        if (mode >= HUAWEI_SFM)
            mode_ = mode = GOOGLE_SFM;
#endif
            backend = new ARCore(env, context, mode == Mode::GOOGLE_FACE, mode == Mode::GOOGLE_TOF);
    }

    ARCoreService::~ARCoreService() {
        delete backend;
        delete renderer;
    }

    void ARCoreService::Clear(bool detach) {
        backend->Clear(detach);
        last_diff = -1;
    }

    void ARCoreService::OnPause() {
        backend->OnPause();
    }

    void ARCoreService::OnGlContextLost() {
        if (renderer) renderer->AbandonGlContext();
    }

    void ARCoreService::OnResume() {
        backend->OnResume();
    }

    void ARCoreService::OnDisplayGeometryChanged(int display_rotation, int width, int height, bool fullhd) {
        backend->OnDisplayGeometryChanged(display_rotation, width, height);

        glViewport(0, 0, width, height);
        int w = 360;
        int h = 640;
        if (fullhd) {
            w = 1080;
            h = 1920;
        }
        renderer->Init(width, height, w, h);
    }

    void ARCoreService::Configure(void *session, void *frame) {
        backend->Configure(session, frame);
    }

    float ARCoreService::CountFrameError() {
        return backend->CountFrameError();
    }

    bool ARCoreService::Process(bool update) {
        bool output;
        output = backend->Process(update);

        if (output) {
            glm::mat4 matrix = GetPose()[COLOR_CAMERA];
            glm::vec3 pos = glm::vec3(matrix[3][0], matrix[3][1], matrix[3][2]);
            glm::quat rot = glm::quat_cast(matrix);

            float value = oc::GLCamera::Diff(pos, image_position, rot, image_rotation);
            if (last_diff >= 0) {
                last_diff = value > last_diff ? value : 0.95f * last_diff + 0.05f * value;
            } else {
                last_diff = value;
            }
            image_position = pos;
            image_rotation = rot;
        }
        return output;
    }


    std::vector<glm::vec3> ARCoreService::GetActiveAnchors() {
        return backend->GetActiveAnchors();
    }

    std::vector<float> ARCoreService::GetDistortion() {
        return backend->GetDistortion();
    }

    Mesh ARCoreService::GetFace() {
        if (mode_ >= HUAWEI_SFM)
            return backend->GetFace(GetProjection());
        return backend->GetFace(GetProjection() * glm::inverse(GetPose()[OPENGL_CAMERA]));
    }

    Image *ARCoreService::GetImage(ARCoreCamera::Effect effect) {
        int w = renderer->rWidth;
        int h = renderer->rHeight;
        if ((effect == ARCoreCamera::Effect::DEPTH) || (effect == ARCoreCamera::Effect::EDGES)) {
            renderer->rWidth = 360;
            renderer->rHeight = 640;
        }

        renderer->Rtt(true);
        RenderCamera(effect);
        renderer->Rtt(false);
        Image* output = renderer->ReadRtt();

        renderer->rWidth = w;
        renderer->rHeight = h;
        return output;
    }

    ARCoreService::Mode ARCoreService::GetMode() {
        return mode_;
    }

    std::vector<glm::vec4> ARCoreService::GetPointCloud(float maxDiff) {
        backend->TakeDepthTestFrame(); // Never reuse a packet from a rejected call.
        std::vector<glm::vec4> output;
        bool validFrame = !GetActiveAnchors().empty() || IsFaceMode();
        if (!validFrame && HasCoordinateSystem())
            return output;

        if (GetPoseDiff() >= maxDiff)
            return output;

        output = backend->GetPointCloud();

        return output;
    }

    std::vector<glm::mat4> ARCoreService::GetPose() {
        return GetPose(GetProjection(), GetView());

    }

    std::vector<glm::mat4> ARCoreService::GetPose(glm::mat4 projection, glm::mat4 view) {
#if SCANNER_MODERN
        const glm::mat4 transform=glm::inverse(view);
#else
        glm::vec3 scale;
        glm::quat rotation;
        glm::vec3 translation;
        glm::vec3 skew;
        glm::vec4 perspective;
        glm::decompose(view, scale, rotation, translation, skew, perspective);

        GLCamera device;
        device.position = rotation * -translation;
        device.rotation = rotation;
        device.scale = glm::vec3(1);
        glm::mat4 transform = device.GetTransformation();
#endif
        std::vector<glm::mat4> output;
        output.reserve(3);
        output.push_back(glm::rotate(transform, glm::radians(180.0f), glm::vec3(1, 0, 0)));
        output.push_back(transform);
        output.push_back(projection * view);
        return output;
    }

    glm::mat4 ARCoreService::GetProjection() {
        return backend->GetProjection();
    }

    glm::mat4 ARCoreService::GetView() {
        return backend->GetView();
    }

    bool ARCoreService::HasCoordinateSystem() {
        return backend->HasCoordinateSystem();
    }

    glm::vec3 ARCoreService::HitTest(int x, int y) {
        return backend->HitTest(x, y);
    }

    bool ARCoreService::IsFaceMode() {
        if (mode_ == GOOGLE_FACE)
            return true;
        else if (mode_ == HUAWEI_FACE)
            return true;
        else
            return false;
    }

    void ARCoreService::RemoveFaceDetails() {
        backend->RemoveFaceDetails();
    }

    void ARCoreService::RenderCamera(int effect, int scale) {
        backend->RenderCamera((ARCoreCamera::Effect)effect, scale);
    }

    void ARCoreService::SetNVScheme(ARCoreCamera::NightVisionScheme s) {
        backend->SetNVScheme(s);
    }

    void ARCoreService::SetOffset(float offset) {
        backend->SetOffset(offset);
    }

    void ARCoreService::SetResolution(float res) {
        backend->SetResolution(res);
    }

    Image *ARCoreService::GetDepthmap() {
        return backend->GetDepthMap(false, true, 1);
    }
}
