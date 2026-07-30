#pragma once

#include <cstdint>

namespace filo {

// Protocol between the coordinator and the extraction workers.
//
// A worker is THE SAME executable relaunched with --extractor-worker: there is
// no second file to ship and the single-binary constraint still holds. They
// talk through a shared memory section and two events, one per direction. No
// file handle crosses over: the worker has no disk access at all, and the bytes
// to parse are placed there by the coordinator.

constexpr uint32_t kProtocolMagic = 0x574F4C46;   // "FLOW"
constexpr uint32_t kProtocolVersion = 1;

// Capacity of the shared section. Larger files do not go through a worker: they
// are marked as too big rather than growing the memory every process reserves.
constexpr uint64_t kInputCapacity  = 32ull << 20;
constexpr uint64_t kOutputCapacity =  8ull << 20;
constexpr uint64_t kHeaderSize     = 4096;
constexpr uint64_t kSectionSize    = kHeaderSize + kInputCapacity + kOutputCapacity;

enum class WorkerStatus : uint32_t {
    Idle = 0,
    Ok = 1,
    Empty = 2,        // extracted, but no useful text
    Failed = 3,       // the parser rejected the file
    TooBig = 4,
    Encrypted = 5,
    NeedsOcr = 6,
    Crashed = 7,      // the worker died: the file's fault
    Timeout = 8,      // the worker did not answer in time
    Unavailable = 9,  // no worker available
};

// Faults triggered on purpose, to check the isolation really holds. Crash
// containment that has never been put under a fault is only a hope.
enum class FaultInjection : uint32_t {
    None = 0,
    AccessViolation = 1,
    InfiniteLoop = 2,
    StackOverflow = 3,
    Abort = 4,
};

#pragma pack(push, 8)
struct SharedHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t kind;          // ContentKind of the incoming content
    uint32_t fault;         // FaultInjection, zero in production
    uint64_t inputSize;
    uint64_t outputSize;
    uint32_t status;        // WorkerStatus
    uint32_t reserved;
    uint64_t sequence;      // grows per request: catches late replies
};
#pragma pack(pop)

static_assert(sizeof(SharedHeader) <= kHeaderSize, "header past the reserved page");

const wchar_t* workerStatusName(WorkerStatus status);

}  // namespace filo
