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
     * - Max: 5000 (Allows adaptation to medium latencies)
     * - Min: 100 (Avoids complete waste if thrashing)
     * - Inc: 200 (Aggressive increase)
     * - Dec: 100 (Gradual backoff)
     */
    WaitStrategy(int min = 100, int max = 5000, int inc = 200, int dec = 100)
        : currentLimit(max), minSpin(min), maxSpin(max), incStep(inc), decStep(dec) {
            // Start with a reasonable middle ground or max
            currentLimit = max;
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
            sleepAction();

            // Check again after waking up
            if (condition()) {
                ready = true;
            }
        }

        return ready;
    }
};

}
