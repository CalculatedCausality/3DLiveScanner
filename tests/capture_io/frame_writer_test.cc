#include <thread/frame_writer.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>

struct FailedLaunch {
    bool allocation;
    template<class Task> std::future<bool> operator()(Task) const {
        if (allocation) throw std::bad_alloc();
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    }
};
int main() {
    using namespace oc;
    // Deliberately hold the borrowed image while the capture worker can write
    // the independent preview. No commit or input release until Wait completes.
    std::promise<void> entered, previewWritten;
    auto enteredFuture = entered.get_future();
    auto release = previewWritten.get_future();
    std::atomic<bool> finished{false};
    int borrowed = 17;
    FrameWriter writer([&] {
        entered.set_value();
        release.wait();
        assert(borrowed == 17);
        finished.store(true);
        return true;
    }, true);
    assert(writer.Parallel());
    assert(enteredFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    assert(!finished.load());
    previewWritten.set_value();
    assert(writer.Wait() && finished.load());
    borrowed = 0;
    assert(writer.Wait());

    for (bool allocation : {false, true}) {
        int calls = 0;
        FrameWriter fallback([&] { ++calls; return true; }, true, FailedLaunch{allocation});
        assert(!fallback.Parallel() && fallback.Wait() && calls == 1);
    }
    for (bool parallel : {false, true}) {
        FrameWriter rejected([] { return false; }, parallel);
        assert(!rejected.Wait());
        FrameWriter exception([]() -> bool { throw std::runtime_error("write failure"); }, parallel);
        assert(!exception.Wait());
        FrameWriter allocation([]() -> bool { throw std::bad_alloc(); }, parallel);
        assert(!allocation.Wait());
    }
    // Destruction also joins during an early exit, before borrowed storage dies.
    std::atomic<bool> completed{false};
    {
        int input = 23;
        FrameWriter cleanup([&] { assert(input == 23); completed.store(true); return true; }, true);
    }
    assert(completed.load());
    std::cout << "PASS: overlapping independent work, joined input lifetime, task errors, serial launch fallback\n";
}
