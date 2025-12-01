#include "../include/SPSCQueue.h"
#include <iostream>
#include <vector>
#include <cassert>
#include <thread>
#include <cstring>
#include "../src/Platform.h" // For CreateNamedEvent

using namespace shm;

void test_batch_basic() {
    std::cout << "Testing Batch Basic..." << std::endl;
    // 1MB buffer
    size_t shmSize = SPSCQueue::GetRequiredSize(1024 * 1024);
    void* buffer = malloc(shmSize);
    SPSCQueue::Init(buffer, 1024 * 1024);

    // Create real event
    EventHandle hEvent = Platform::CreateNamedEvent("test_batch_basic");

    SPSCQueue queue(buffer, 1024 * 1024, hEvent);

    std::vector<std::vector<uint8_t>> inputs;
    for(int i=0; i<10; ++i) {
        std::vector<uint8_t> msg(100);
        memset(msg.data(), i, 100);
        inputs.push_back(msg);
    }

    // Writer
    queue.EnqueueBatch(inputs);

    // Reader
    std::vector<std::vector<uint8_t>> outputs;
    queue.DequeueBatch(outputs, 100);

    assert(outputs.size() == 10);
    for(int i=0; i<10; ++i) {
        assert(outputs[i].size() == 100);
        assert(outputs[i][0] == i);
    }

    Platform::CloseEvent(hEvent);
    free(buffer);
    std::cout << "PASS" << std::endl;
}

void test_wrapping() {
    std::cout << "Testing Wrapping..." << std::endl;
    // Small buffer: 128 header + 100 capacity
    size_t cap = 200;
    size_t shmSize = SPSCQueue::GetRequiredSize(cap);
    void* buffer = malloc(shmSize);
    SPSCQueue::Init(buffer, cap);

    EventHandle hEvent = Platform::CreateNamedEvent("test_wrapping");
    SPSCQueue queue(buffer, cap, hEvent);

    // Fill slightly
    std::vector<uint8_t> d1(50, 1);
    queue.Enqueue(d1.data(), 50);

    // Read it
    std::vector<uint8_t> out;
    queue.Dequeue(out);
    assert(out.size() == 50);

    // Now wPos is advanced. 50 + 8 = 58.
    // Capacity 200.
    // Let's write enough to wrap.
    // Free space at end: 200 - 58 = 142.
    // Write 100 bytes (total 108).
    std::vector<uint8_t> d2(100, 2);
    queue.Enqueue(d2.data(), 100);

    // wPos: 58 + 108 = 166.
    // Free space at end: 200 - 166 = 34.

    // Write batch that forces wrap.
    // Need > 34 bytes (header+data).
    // Try writing 40 bytes data (+8 = 48).
    // Should pad 34 bytes, wrap to 0, write 48 bytes.
    std::vector<std::vector<uint8_t>> batch;
    batch.push_back(std::vector<uint8_t>(40, 3));

    queue.EnqueueBatch(batch);

    // Read d2
    queue.Dequeue(out);
    assert(out.size() == 100);
    assert(out[0] == 2);

    // Read batch
    std::vector<std::vector<uint8_t>> batchOut;
    queue.DequeueBatch(batchOut, 10);

    assert(batchOut.size() == 1);
    assert(batchOut[0].size() == 40);
    assert(batchOut[0][0] == 3);

    Platform::CloseEvent(hEvent);
    free(buffer);
    std::cout << "PASS" << std::endl;
}

int main() {
    test_batch_basic();
    test_wrapping();
    return 0;
}
