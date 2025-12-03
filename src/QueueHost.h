#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <thread>
#include <cstring>
#include <functional>
#include "SPSCQueue.h"
#include "Platform.h"
#include "IPCUtils.h"

namespace shm {

template <typename QueueT>
class QueueHost {
    void* shmBase;
    std::string shmName;
    uint64_t queueSize;
    ShmHandle hMapFile;

    std::unique_ptr<QueueT> toGuestQueue;
    std::unique_ptr<QueueT> fromGuestQueue;
    EventHandle hToGuestEvent;
    EventHandle hFromGuestEvent;

    std::thread readerThread;
    std::atomic<bool> running;

    // Mutex for direct writing to Queue
    // SPSCQueue is NOT thread-safe for multiple producers, so we need a lock.
    std::mutex sendMutex;

    // Callback for received messages: (data)
    std::function<void(std::vector<uint8_t>&&, uint32_t)> onMessage;

public:
    QueueHost() : shmBase(nullptr), hMapFile(0), running(false) {}
    ~QueueHost() { Shutdown(); }

    bool Init(const std::string& shmName, uint64_t queueSize,
              std::function<void(std::vector<uint8_t>&&, uint32_t)> msgHandler) {
        this->shmName = shmName;
        this->queueSize = queueSize;
        this->onMessage = msgHandler;

        size_t qTotalSize = QueueT::GetRequiredSize(queueSize);
        size_t totalShmSize = qTotalSize * 2;

        bool exists = false;
        shmBase = Platform::CreateNamedShm(shmName.c_str(), totalShmSize, hMapFile, exists);
        if (!shmBase) return false;

        memset(shmBase, 0, totalShmSize);

        uint8_t* reqBase = (uint8_t*)shmBase;
        QueueT::Init(reqBase, queueSize);

        uint8_t* respBase = reqBase + qTotalSize;
        QueueT::Init(respBase, queueSize);

        hToGuestEvent = Platform::CreateNamedEvent((shmName + "_event_req").c_str());
        hFromGuestEvent = Platform::CreateNamedEvent((shmName + "_event_resp").c_str());

        toGuestQueue = std::unique_ptr<QueueT>(new QueueT(reqBase, queueSize, hToGuestEvent));
        fromGuestQueue = std::unique_ptr<QueueT>(new QueueT(respBase, queueSize, hFromGuestEvent));

        running = true;
        readerThread = std::thread(&QueueHost::ReaderLoop, this);

        return true;
    }

    void Shutdown() {
        if (!running) return;
        running = false;

        if (readerThread.joinable()) readerThread.join();

        if (shmBase) Platform::CloseShm(hMapFile, shmBase);
        Platform::CloseEvent(hToGuestEvent);
        Platform::CloseEvent(hFromGuestEvent);
    }

    void Send(const uint8_t* data, uint32_t size, uint32_t msgId = MSG_ID_NORMAL) {
        EnqueueHelper(data, size, msgId);
    }

private:
    void EnqueueHelper(const uint8_t* data, uint32_t size, uint32_t msgId) {
        EnqueueImpl(data, size, msgId, std::is_same<QueueT, SPSCQueue>());
    }

    void EnqueueImpl(const uint8_t* data, uint32_t size, uint32_t msgId, std::true_type /* is_spsc */) {
        std::lock_guard<std::mutex> lock(sendMutex);
        toGuestQueue->Enqueue(data, size, msgId);
    }

    void EnqueueImpl(const uint8_t* data, uint32_t size, uint32_t msgId, std::false_type /* is_mpsc */) {
        // No lock needed for MPSC
        toGuestQueue->Enqueue(data, size, msgId);
    }

    void ReaderLoop() {
        std::vector<uint8_t> buffer;
        buffer.reserve(1024);

        while (running) {
            uint32_t msgId = fromGuestQueue->Dequeue(buffer, &running);
            if (msgId == 0xFFFFFFFF) break;

            if (onMessage) {
                // Copy buffer to pass to handler (or move)
                // Since buffer is reused in Dequeue (wait, SPSCQueue::Dequeue returns a copy inside buffer, but here we pass 'buffer' by reference to be filled)
                // Correct: fromGuestQueue->Dequeue(buffer...) fills buffer.
                // We must create a copy or move it.
                // Since we reuse 'buffer' in next iteration, we should create a new vector or copy.
                // Optim: onMessage takes rvalue?
                // Let's copy for safety as 'buffer' is thread-local workspace
                std::vector<uint8_t> msgCopy = buffer;
                onMessage(std::move(msgCopy), msgId);
            }
        }
    }
};

}
