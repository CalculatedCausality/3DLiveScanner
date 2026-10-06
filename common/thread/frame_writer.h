#ifndef SCANNER_FRAME_WRITER_H
#define SCANNER_FRAME_WRITER_H

#include <future>
#include <new>
#include <system_error>

namespace oc {
    struct AsyncFrameLaunch {
        template<class Task> std::future<bool> operator()(Task task) const {
            return std::async(std::launch::async, task);
        }
    };

    // One joined task owned by the current binder-locked capture. There is no
    // cross-frame queue and no publication until Wait() has returned success.
    class FrameWriter {
    public:
        template<class Task, class Launch = AsyncFrameLaunch>
        FrameWriter(Task task, bool parallel, Launch launch = Launch()) {
            auto safeTask = [task]() -> bool {
                try { return task(); }
                catch (...) { return false; }
            };
            if (parallel) {
                try {
                    pending = launch(safeTask);
                    parallel_ = true;
                    return;
                } catch (const std::system_error&) {
                    // Thread resources unavailable: preserve the serial path.
                } catch (const std::bad_alloc&) {
                }
            }
            result = safeTask();
        }
        ~FrameWriter() { Wait(); }
        bool Parallel() const { return parallel_; }
        bool Wait() noexcept {
            if (pending.valid()) {
                try { result = pending.get(); }
                catch (...) { result = false; }
            }
            return result;
        }
    private:
        std::future<bool> pending;
        bool result = false;
        bool parallel_ = false;
    };
}
#endif
