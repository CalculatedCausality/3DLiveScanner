#ifndef SCANNER_CAPTURE_BACKOFF_H
#define SCANNER_CAPTURE_BACKOFF_H

#include <atomic>
#include <chrono>
#include <cstdint>

namespace oc {
    // A rejected frame must not trigger another GPU readback/integration attempt
    // on every preview draw. No sleeping thread or queued capture is introduced.
    class CaptureBackoff {
    public:
        static int64_t NowMs() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
        }
        bool Ready(int64_t now = NowMs()) const { return now >= deadline.load(); }
        void Rejected(int64_t now = NowMs()) {
            unsigned previous = failures.load();
            unsigned count = previous < 4 ? previous + 1 : 4;
            failures.store(count);
            deadline.store(now + (int64_t(250) << (count - 1)));
        }
        void Reset() {
            failures.store(0);
            deadline.store(0);
        }
    private:
        std::atomic<int64_t> deadline{0};
        std::atomic<unsigned> failures{0};
    };
}
#endif
