#include <thread/capture_backoff.h>
#include <cassert>
#include <iostream>

int main() {
    oc::CaptureBackoff backoff;
    int64_t now = 1000;
    assert(backoff.Ready(now));
    for (int delay : {250, 500, 1000, 2000, 2000, 2000}) {
        backoff.Rejected(now);
        assert(!backoff.Ready(now));
        assert(!backoff.Ready(now + delay - 1));
        assert(backoff.Ready(now + delay));
        now += delay;
    }
    backoff.Reset();
    assert(backoff.Ready(now));
    backoff.Rejected(now);
    assert(backoff.Ready(now + 250)); // Success/explicit Resume resets escalation.
    std::cout << "PASS: bounded exponential capture cooldown, exact deadlines and reset\n";
}
