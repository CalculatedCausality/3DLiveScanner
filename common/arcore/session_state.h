#ifndef ARCORE_SESSION_STATE_H
#define ARCORE_SESSION_STATE_H

#include <arcore_c_api.h>
#include <cstdint>

namespace oc {
namespace arcore_state {

inline ArDepthMode SelectDepthMode(bool preferRawOnly, bool automatic, bool raw) {
    if (preferRawOnly && raw) return AR_DEPTH_MODE_RAW_DEPTH_ONLY;
    if (automatic) return AR_DEPTH_MODE_AUTOMATIC;
    return raw ? AR_DEPTH_MODE_RAW_DEPTH_ONLY : AR_DEPTH_MODE_DISABLED;
}

inline bool DepthUnavailable(ArStatus status) {
    return status == AR_ERROR_NOT_YET_AVAILABLE || status == AR_ERROR_NOT_TRACKING ||
           status == AR_ERROR_DEADLINE_EXCEEDED;
}

// A failed resume must not leave update() polling SESSION_PAUSED forever.
// Retry camera contention on the same session; never recreate its world map.
class SessionState {
public:
    void Paused() { running_ = retry_ = false; }
    void Resumed(ArStatus result, int64_t nowMs) {
        running_ = result == AR_SUCCESS;
        retry_ = result == AR_ERROR_CAMERA_NOT_AVAILABLE;
        retryAtMs_ = nowMs + 250;
    }
    void Updated(ArStatus result, int64_t nowMs) {
        if (result == AR_ERROR_CAMERA_NOT_AVAILABLE || result == AR_ERROR_SESSION_PAUSED) {
            running_ = false;
            retry_ = true;
            retryAtMs_ = nowMs + 250;
        } else if (result == AR_ERROR_FATAL) {
            running_ = retry_ = false;
        }
    }
    bool Running() const { return running_; }
    bool RetryDue(int64_t nowMs) const { return retry_ && nowMs >= retryAtMs_; }
private:
    bool running_ = false;
    bool retry_ = false;
    int64_t retryAtMs_ = 0;
};

} // namespace arcore_state
} // namespace oc
#endif
