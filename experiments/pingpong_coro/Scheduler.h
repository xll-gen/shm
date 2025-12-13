#pragma once

#include <vector>
#include <coroutine>
#include <thread>
#include "CoroutineUtils.h"

namespace pingpong {

class Scheduler {
public:
    void addTask(Task&& task) {
        task.resume();
        tasks.push_back(std::move(task));
    }

    void run() {
        while (!tasks.empty()) {
            bool any_active = false;
            for (auto it = tasks.begin(); it != tasks.end(); ) {
                if (it->done()) {
                    it = tasks.erase(it);
                } else {
                    it->resume();
                    any_active = true;
                    ++it;
                }
            }
            if (!any_active && !tasks.empty()) {
                std::this_thread::yield();
            }
        }
    }

private:
    std::vector<Task> tasks;
};

} // namespace pingpong
