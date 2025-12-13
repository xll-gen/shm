#pragma once

#include <coroutine>
#include <atomic>
#include <thread>
#include "common.h"

namespace pingpong {

struct SlotReady {
    Packet* packet;
    bool await_ready() {
        return packet->state.load(std::memory_order_acquire) == STATE_RESP_READY;
    }
    void await_suspend(std::coroutine_handle<>) {
    }
    void await_resume() {
    }
};

Task WaitResponse(Packet* packet) {
    while (packet->state.load(std::memory_order_acquire) != STATE_RESP_READY) {
        co_await std::suspend_always{};
    }
    co_return;
}

} // namespace pingpong
