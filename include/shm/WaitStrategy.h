#pragma once

#include "Platform.h"

namespace shm {

/**
 * @class WaitStrategy
 * @brief Implements an adaptive spin-wait strategy for synchronization.
 *
 * This class encapsulates the logic for hybrid spinning and sleeping.
 * It dynamically adjusts the spin limit based on successful acquisitions
 * to optimize latency in varying load conditions (dedicated vs oversubscribed).
 */
class WaitStrategy {
public:
    int currentLimit;
    const int minSpin;
    const int maxSpin;
    const int incStep;
    const int decStep;

    /**
     * @brief Constructs a WaitStrategy with specified parameters.
     *
     * Default values are tuned for high-performance IPC:
     * - Max: 20000 (Allows adaptation to medium latencies)
     * - Min: 100 (Avoids complete waste if thrashing)
     * - Inc: 100 (Gradual increase)
     * - Dec: 500 (Aggressive backoff on failure)
     */
    WaitStrategy(int min = 100, int max = 20000, int inc = 100, int dec = 500)
        : currentLimit(min), minSpin(min), maxSpin(max), incStep(inc), decStep(dec) {
            // Start with a reasonable middle ground or min?
            // Usually starting at min allows quick ramp up if needed,
            // but starting at max might be better for "optimistic" start.
            // Existing code initialized to 5000 or 2000.
            // Let's initialize to a safe middle value or max/4.
            currentLimit = 2000;
    }

    /**
     * @brief Waits for a condition to become true.
     *
     * @tparam Condition Function-like object returning bool (true if ready).
     * @tparam SleepAction Function-like object to execute when spinning fails (e.g. semaphore wait).
     *
     * @param condition The condition to check.
     * @param sleepAction The action to take if spinning times out.
     * @return true if condition met, false if sleepAction returned (and condition might be true or false).
     *         The return value effectively mirrors whether we successfully acquired the resource.
     */
    template <typename Condition, typename SleepAction>
    bool Wait(Condition condition, SleepAction sleepAction) {
        bool ready = false;

        // 1. Spin Phase
        for (int i = 0; i < currentLimit; ++i) {
            if (condition()) {
                ready = true;
                break;
            }
            Platform::CpuRelax();
        }

        if (ready) {
            // Success: Reward (Increase spin limit)
            if (currentLimit < maxSpin) {
                currentLimit += incStep;
                if (currentLimit > maxSpin) currentLimit = maxSpin;
            }
        } else {
            // Failure: Punish (Decrease spin limit)
            if (currentLimit > minSpin) {
                currentLimit -= decStep;
                if (currentLimit < minSpin) currentLimit = minSpin;
            }

            // 2. Sleep Phase
            // execute the sleep action (which should handle the "Wait for Event" logic)
            // The sleep action is responsible for checking the condition again if necessary
            // or handling spurious wakeups internally, OR it just waits once.
            // Typically, the caller's SleepAction wraps the "Set Waiting Flag -> Check -> Wait" sequence.
            sleepAction();

            // We assume sleepAction returns when the event is signaled or timeout.
            // We check ready again? Or assume the caller handles the loop?
            // "WaitResponse" in DirectHost loops if timeout is used.
            // But basic WaitStrategy usually implies "Wait until ready".
            // However, DirectHost::WaitResponse has a timeout.

            // Let's look at how we want to use this.
            // If we move the ENTIRE logic here, we need to handle timeouts.
            // But timeouts are complex.

            // Implementation choice: This WaitStrategy only decides "Spin or Sleep".
            // It runs the spin loop. If that fails, it calls sleepAction.
            // It updates the stats.
            // It does NOT loop the sleepAction. The sleepAction itself might be a blocking call.

            // If sleepAction returns, we consider it a "cycle".
            // If the condition is true after sleepAction, we are good.
            if (condition()) {
                ready = true;
            }
        }

        return ready;
    }
};

}
