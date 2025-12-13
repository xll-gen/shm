#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <getopt.h>

#include "Aeron.h"
#include "common.h"

using namespace aeron;
using namespace pingpong;

std::atomic<bool> running(true);

void sig_handler(int) {
    running.store(false);
}

// FragmentHandler for Pong (Echo)
void pong_handler(const AtomicBuffer& buffer, util::index_t offset, util::index_t length, const Header& header, std::shared_ptr<Publication> pub) {
    if (length != sizeof(Message)) return;

    const Message* req = reinterpret_cast<const Message*>(buffer.buffer() + offset);
    Message resp = *req;
    resp.sum = req->val_a + req->val_b;

    // Echo back
    while (pub->offer(AtomicBuffer((uint8_t*)&resp, sizeof(Message))) < 0) {
        if (!running) break;
        std::this_thread::yield(); // Backoff lightly if buffer full
    }
}

// Pong Worker Thread
void pong_worker(int id, std::shared_ptr<Aeron> aeron) {
    int sub_stream_id = STREAM_ID_BASE_PING + id;
    int pub_stream_id = STREAM_ID_BASE_PONG + id;

    // Create Subscription (Receive from Ping)
    std::shared_ptr<Subscription> sub = aeron->addSubscription(CHANNEL, sub_stream_id);

    // Create Publication (Send to Ping)
    std::shared_ptr<Publication> pub = aeron->addPublication(CHANNEL, pub_stream_id);

    // Wait for connection
    while (!sub->isConnected() || !pub->isConnected()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!running) return;
    }

    auto handler = [&](const AtomicBuffer& buffer, util::index_t offset, util::index_t length, const Header& header) {
        pong_handler(buffer, offset, length, header, pub);
    };

    BusySpinIdleStrategy idle_strategy;
    while (running) {
        int fragments = sub->poll(handler, 10);
        idle_strategy.idle(fragments);
    }
}

// Ping Worker Thread
struct PingResult {
    double ops;
    long long operations;
};

void ping_worker(int id, int iterations, std::shared_ptr<Aeron> aeron, PingResult& result) {
    int pub_stream_id = STREAM_ID_BASE_PING + id;
    int sub_stream_id = STREAM_ID_BASE_PONG + id;

    std::shared_ptr<Publication> pub = aeron->addPublication(CHANNEL, pub_stream_id);
    std::shared_ptr<Subscription> sub = aeron->addSubscription(CHANNEL, sub_stream_id);

    while (!pub->isConnected() || !sub->isConnected()) {
        std::this_thread::yield();
    }

    Message msg;
    bool response_received = false;

    auto handler = [&](const AtomicBuffer& buffer, util::index_t offset, util::index_t length, const Header& header) {
        if (length != sizeof(Message)) return;
        const Message* resp = reinterpret_cast<const Message*>(buffer.buffer() + offset);
        if (resp->sum == (resp->val_a + resp->val_b)) {
            response_received = true;
        } else {
            std::cerr << "Mismatch!" << std::endl;
            exit(1);
        }
    };

    BusySpinIdleStrategy idle_strategy;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        msg.req_id = i;
        msg.val_a = i;
        msg.val_b = i * 2;
        msg.sum = 0;

        // Send
        while (pub->offer(AtomicBuffer((uint8_t*)&msg, sizeof(Message))) < 0) {
            idle_strategy.idle(0);
        }

        // Wait for response
        response_received = false;
        while (!response_received) {
            int fragments = sub->poll(handler, 1);
            idle_strategy.idle(fragments);
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    result.operations = iterations;
    result.ops = iterations / diff.count();
}

int main(int argc, char** argv) {
    signal(SIGINT, sig_handler);

    int threads = 1;
    std::string mode = "ping"; // ping or pong

    int opt;
    while ((opt = getopt(argc, argv, "m:t:")) != -1) {
        switch (opt) {
            case 'm': mode = optarg; break;
            case 't': threads = std::atoi(optarg); break;
            default:
                std::cerr << "Usage: " << argv[0] << " -m [ping|pong] -t [threads]" << std::endl;
                return 1;
        }
    }

    try {
        Context context;
        // Connect to local Media Driver (must be running)
        std::shared_ptr<Aeron> aeron = Aeron::connect(context);

        std::cout << "Starting " << mode << " with " << threads << " threads..." << std::endl;

        if (mode == "pong") {
            std::vector<std::thread> workers;
            for (int i = 0; i < threads; ++i) {
                workers.emplace_back(pong_worker, i, aeron);
            }
            for (auto& t : workers) t.join();
        } else {
            const int ITERATIONS = 100000;
            std::vector<std::thread> workers;
            std::vector<PingResult> results(threads);

            auto start_total = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < threads; ++i) {
                workers.emplace_back(ping_worker, i, ITERATIONS, aeron, std::ref(results[i]));
            }

            for (auto& t : workers) t.join();

            auto end_total = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> diff_total = end_total - start_total;

            long long total_ops = 0;
            for (auto& r : results) total_ops += r.operations;

            double system_ops = total_ops / diff_total.count();
            std::cout << "Done. Total Time: " << diff_total.count() << "s. System Effective OPS: " << system_ops << std::endl;
        }

    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
