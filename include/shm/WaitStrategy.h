#pragma once

#include "Platform.h"

namespace shm {

/**
 * @class WaitStrategy
 * @brief Single adaptive spin/sleep strategy used by every Direct Exchange slot.
 *
 * Mirrors the Go-side collapse landed in shm v0.6.1. The defaults are tuned
 * (see EXPERIMENTS.md, "Exp 3"): they keep 1-thread latency near the spin
 * floor while still hitting peak multi-thread throughput. The constants are
 * intentionally not configurable — one strategy fits all Direct Exchange call
 * sites. If a new workload requires different tuning, adjust the constants
 * here, re-run the harness, and update EXPERIMENTS.md.
 *
 * Adaptation: on each successful spin we grow `currentLimit`; on each failed
 * spin we shrink it. The OS-wait fallback runs only after the punished spin
 * window expires.
 */
class WaitStrategy {
public:
    // Spin window is sized to bridge the typical Direct Exchange inter-request
    // gap on a non-oversubscribed system. kMaxSpin × per-iter cost (~1 ns on
    // x86 via Platform::CpuRelax) yields a ~50–100 µs ceiling before falling
    // back to OS-wait. kIncStep ≫ kDecStep so a single failed spin doesn't
    // collapse the window; adaptive shrink toward kMinSpin only on sustained
    // failures (oversubscribed environments self-throttle).
    static constexpr int kMinSpin = 100;
    static constexpr int kMaxSpin = 50000;
    static constexpr int kIncStep = 5000;
    static constexpr int kDecStep = 1000;

    int currentLimit;

    WaitStrategy() : currentLimit(kMaxSpin) {}

    /**
     * @brief Waits for a condition to become true.
     *
     * @tparam Condition Function-like object returning bool (true if ready).
     * @tparam SleepAction Function-like object to execute when spinning fails (e.g. semaphore wait).
     *
     * @param condition The condition to check.
     * @param sleepAction The action to take if spinning times out.
     * @return true if condition met, false if sleepAction returned without it becoming true.
     */
    template <typename Condition, typename SleepAction>
    bool Wait(Condition condition, SleepAction sleepAction) {
        bool ready = false;

        for (int i = 0; i < currentLimit; ++i) {
            if (condition()) {
                ready = true;
                break;
            }
            Platform::CpuRelax();
        }

        if (ready) {
            if (currentLimit < kMaxSpin) {
                currentLimit += kIncStep;
                if (currentLimit > kMaxSpin) currentLimit = kMaxSpin;
            }
        } else {
            if (currentLimit > kMinSpin) {
                currentLimit -= kDecStep;
                if (currentLimit < kMinSpin) currentLimit = kMinSpin;
            }

            sleepAction();

            if (condition()) {
                ready = true;
            }
        }

        return ready;
    }
};

}
