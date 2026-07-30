#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "content/selector.h"
#include "worker/protocol.h"

namespace filo {

struct PoolStats {
    uint64_t requests = 0;
    uint64_t ok = 0;
    uint64_t crashes = 0;
    uint64_t timeouts = 0;
    uint64_t restarts = 0;
};

// Governs the pool of processes that extract content.
//
// Three layers of containment, each covering a different failure:
//   1. __try/__except in the worker   -> access violations and the like
//   2. Job Object with limits         -> runaway memory, orphaned processes
//   3. watchdog in the coordinator    -> infinite loops, which the other two miss
//
// The most important flag is KILL_ON_JOB_CLOSE: if the coordinator dies in any
// way, the workers die with it. Without it, an application that hangs leaves
// stray processes on the user's machine.
class ExtractorPool {
public:
    ExtractorPool() = default;
    ~ExtractorPool();

    ExtractorPool(const ExtractorPool&) = delete;
    ExtractorPool& operator=(const ExtractorPool&) = delete;

    bool start(unsigned workerCount, std::wstring* error);
    void stop();

    // Extracts the text. Blocking: waits for a worker to become free.
    // `fault` exists only for the resilience tests and is zero in production.
    WorkerStatus extract(ContentKind kind, const char* data, size_t size,
                         std::string* text, uint32_t fault = 0);

    const PoolStats& stats() const { return stats_; }
    unsigned workerCount() const { return static_cast<unsigned>(slots_.size()); }

private:
    struct Slot;

    bool launch(Slot& slot, std::wstring* error);
    void recycle(Slot& slot);

    std::vector<Slot*> slots_;
    void* job_ = nullptr;
    void* available_ = nullptr;   // semaphore: how many workers are free
    void* slotLock_ = nullptr;    // critical section over the free list
    std::vector<unsigned> freeList_;
    PoolStats stats_;
};

}  // namespace filo
