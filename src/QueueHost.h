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

/**
 * @brief Host implementation for Queue IPC Mode.
 *
 * Uses two shared memory ring buffers (SPSC Queues): one for sending requests,
 * and one for receiving responses.
 *
 * @tparam QueueT The queue implementation (usually SPSCQueue).
 */
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

    /**
     * @brief Mutex for synchronizing access to the Request Queue.
     * Required because SPSCQueue is single-producer, but QueueHost might be shared.
     */
    std::mutex sendMutex;

    /** @brief Callback for processing received messages. */
    std::function<void(std::vector<uint8_t>&&, uint32_t)> onMessage;

public:
    /** @brief Constructor. */
    QueueHost() : shmBase(nullptr), hMapFile(0), running(false) {}

    /** @brief Destructor. Calls Shutdown. */
    ~QueueHost() { Shutdown(); }

    /**
     * @brief Initializes the Queue Host.
     *
     * @param shmName Name of the shared memory.
     * @param queueSize Size of each queue (Req/Resp) in bytes.
     * @param msgHandler Callback for responses.
     * @return true on success.
     */
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

    /**
     * @brief Stops the reader thread and closes resources.
     */
    void Shutdown() {
        if (!running) return;
        running = false;

        if (readerThread.joinable()) readerThread.join();

        if (shmBase) Platform::CloseShm(hMapFile, shmBase);
        Platform::CloseEvent(hToGuestEvent);
        Platform::CloseEvent(hFromGuestEvent);
    }

    /**
     * @brief Enqueues a message to the Guest.
     *
     * @param data Data pointer.
     * @param size Data size.
     * @param msgId Message ID (default NORMAL).
     */
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
        // No lock needed for MPSC (if supported in future)
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
