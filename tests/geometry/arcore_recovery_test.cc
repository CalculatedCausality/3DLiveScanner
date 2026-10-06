#include <arcore/session_state.h>
#include <arcore/geometry_validation.h>
#include <cassert>
#include <iostream>

using namespace oc;

int main() {
    // Google's software-depth path must choose the documented AUTOMATIC mode,
    // never the locally added private depth-mode value 4.
    assert(arcore_state::SelectDepthMode(false, true, true) == AR_DEPTH_MODE_AUTOMATIC);
    assert(arcore_state::SelectDepthMode(false, false, true) == AR_DEPTH_MODE_RAW_DEPTH_ONLY);
    assert(arcore_state::SelectDepthMode(true, true, true) == AR_DEPTH_MODE_RAW_DEPTH_ONLY);
    assert(arcore_state::SelectDepthMode(false, true, false) == AR_DEPTH_MODE_AUTOMATIC);
    assert(arcore_state::SelectDepthMode(false, false, false) == AR_DEPTH_MODE_DISABLED);

    arcore_state::SessionState session;
    assert(!session.Running());
    session.Resumed(AR_ERROR_CAMERA_NOT_AVAILABLE, 1000);
    assert(!session.Running() && !session.RetryDue(1249));
    assert(session.RetryDue(1250));
    session.Resumed(AR_ERROR_CAMERA_NOT_AVAILABLE, 1250);
    assert(!session.RetryDue(1499) && session.RetryDue(1500));
    session.Resumed(AR_SUCCESS, 1500);
    assert(session.Running() && !session.RetryDue(5000));

    // Unexpected session interruption recovers without replacing the session
    // (and therefore without discarding its anchors/world coordinate system).
    session.Updated(AR_ERROR_SESSION_PAUSED, 1600);
    assert(!session.Running() && session.RetryDue(1850));
    session.Resumed(AR_SUCCESS, 1850);
    assert(session.Running());
    session.Updated(AR_ERROR_CAMERA_NOT_AVAILABLE, 2000);
    session.Paused(); // deliberate Activity pause cancels automatic retries
    assert(!session.Running() && !session.RetryDue(10000));
    session.Resumed(AR_SUCCESS, 10000);
    assert(session.Running());
    session.Updated(AR_ERROR_TEXTURE_NOT_SET, 10100);
    assert(session.Running()); // texture rebind, not a world/session reset
    session.Updated(AR_ERROR_FATAL, 10200);
    assert(!session.Running() && !session.RetryDue(11000));
    session.Resumed(AR_ERROR_CAMERA_PERMISSION_NOT_GRANTED, 12000);
    assert(!session.Running() && !session.RetryDue(13000));

    // Raw-depth warm-up / brief tracking loss must not become permanent errors.
    assert(arcore_state::DepthUnavailable(AR_ERROR_NOT_YET_AVAILABLE));
    assert(arcore_state::DepthUnavailable(AR_ERROR_NOT_TRACKING));
    assert(arcore_state::DepthUnavailable(AR_ERROR_DEADLINE_EXCEEDED));
    assert(!arcore_state::DepthUnavailable(AR_ERROR_ILLEGAL_STATE));
    assert(!arcore_state::DepthUnavailable(AR_ERROR_RESOURCE_EXHAUSTED));

    // Missing/malformed confidence pair followed by a valid pair on the same
    // buffers is accepted; no stateful failure flag disables depth acquisition.
    uint8_t depthBytes[] = {0x28, 0x23}; // 9000 mm: retain all sixteen bits
    uint8_t confidenceBytes[] = {200};
    geometry::Plane depth;
    depth.data = depthBytes; depth.length = 2;
    depth.width = depth.height = 1; depth.rowStride = depth.pixelStride = 2;
    geometry::Plane confidence;
    confidence.width = confidence.height = 1;
    confidence.rowStride = confidence.pixelStride = 1;
    assert(depth.Valid(2) && !confidence.Valid(1));
    confidence.data = confidenceBytes;
    confidence.length = 1;
    assert(confidence.Valid(1) && confidence.SameSize(depth));
    assert(depth.Depth(0, 0) == 9000 && confidence.Byte(0, 0) == 200);
    confidence.width = 2;
    assert(!confidence.Valid(1) && !confidence.SameSize(depth));
    confidence.width = 1;
    assert(confidence.Valid(1) && confidence.SameSize(depth));
    std::cout << "ARCore depth/session interruption recovery regressions passed\n";
}
