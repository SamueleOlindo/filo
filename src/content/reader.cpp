#include "content/reader.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <thread>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace filo {
namespace {

using Clock = std::chrono::steady_clock;

double secondsBetween(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

// How many threads open files at once.
//
// More than the available cores, deliberately: these threads spend their lives
// BLOCKED inside the kernel waiting for the disk, not burning CPU. The useful
// parallelism here is the number of requests in flight, not the number of
// cores. It is also the only lever we have, because on Win32 opening a file is
// always synchronous: no asynchronous mechanism parallelizes it.
// Measured over 40,174 real files, on a 6-core/12-thread desktop CPU with
// Defender on:
//
//    8 threads   62.3 s
//   16 threads   54.9 s     <- where the real gain ends
//   32 threads   51.8 s     (+6% over 16, for four times the resources)
//   48 threads   53.6 s     worse
//
// The curve flattens because the bottleneck is OPENING the files, which goes
// through the antivirus minifilter and is largely serialized: adding threads
// adds waiting in parallel, not work. Sixteen is where nearly all of it has
// been taken without disturbing the machine.
unsigned defaultThreadCount() {
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    return std::min(16u, cores * 2);
}

void applyLowIoPriority(HANDLE file) {
    // Asks Windows to serve these reads only while the disk is idle. Without
    // it, an indexing run makes the PC visibly slow.
    FILE_IO_PRIORITY_HINT_INFO hint = {};
    hint.PriorityHint = IoPriorityHintVeryLow;
    SetFileInformationByHandle(file, FileIoPriorityHintInfo, &hint, sizeof(hint));
}

struct ThreadState {
    std::vector<char> buffer;
    ReaderStats stats;
};

}  // namespace

ReaderStats ContentReader::run(const FileIndex& index,
                               const std::vector<uint32_t>& candidates,
                               const Options& options, const Sink& sink) {
    const auto start = Clock::now();

    unsigned threadCount = options.threads ? options.threads : defaultThreadCount();
    threadCount = std::max(1u, std::min<unsigned>(threadCount,
                                                 static_cast<unsigned>(candidates.size())));

    if (options.lowPriority) {
        // Pages read during indexing are the first to be dropped when someone
        // else needs memory: we do not want to evict from the cache the
        // programs the user is actually using.
        MEMORY_PRIORITY_INFORMATION memory = {};
        memory.MemoryPriority = MEMORY_PRIORITY_LOW;
        SetProcessInformation(GetCurrentProcess(), ProcessMemoryPriority, &memory,
                              sizeof(memory));
    }

    std::atomic<size_t> cursor{0};
    std::vector<ThreadState> states(threadCount);
    std::vector<std::thread> workers;
    workers.reserve(threadCount - 1);

    auto worker = [&](unsigned slotIndex) {
        ThreadState& state = states[slotIndex];

        for (;;) {
            const size_t i = cursor.fetch_add(1, std::memory_order_relaxed);
            if (i >= candidates.size()) break;

            const uint32_t slot = candidates[i];
            ++state.stats.filesSeen;

            ReadResult result;
            result.slot = slot;
            result.declaredKind = classifyByName(index.name(slot));

            const std::wstring path = index.fullPath(slot);

            // --- gate 0: metadata, without opening the file ------------------
            FileFacts facts;
            if (!readFileFacts(path.c_str(), &facts)) {
                result.verdict = GateVerdict::Unreadable;
                ++state.stats.byVerdict[static_cast<size_t>(result.verdict)];
                sink(result);
                continue;
            }
            result.size = facts.size;
            result.mtime = facts.mtime;

            if (isCloudPlaceholder(facts.attributes)) {
                result.verdict = GateVerdict::CloudPlaceholder;
                ++state.stats.byVerdict[static_cast<size_t>(result.verdict)];
                sink(result);
                continue;
            }
            if (facts.size == 0) {
                result.verdict = GateVerdict::Empty;
                ++state.stats.byVerdict[static_cast<size_t>(result.verdict)];
                sink(result);
                continue;
            }
            if (facts.size > sizeLimitFor(result.declaredKind)) {
                result.verdict = GateVerdict::TooBig;
                ++state.stats.byVerdict[static_cast<size_t>(result.verdict)];
                sink(result);
                continue;
            }

            // --- opening -----------------------------------------------------
            const auto openStart = Clock::now();
            HANDLE file = CreateFileW(
                path.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING,
                // SEQUENTIAL_SCAN: we read start to finish exactly once.
                // OPEN_NO_RECALL: if despite the checks above the file turns
                // out to be remote, do NOT download it. This is the second
                // safety net against cloud hydration.
                FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_NO_RECALL,
                nullptr);
            const auto openEnd = Clock::now();
            state.stats.openSeconds += secondsBetween(openStart, openEnd);

            if (file == INVALID_HANDLE_VALUE) {
                result.verdict = GateVerdict::Unreadable;
                ++state.stats.byVerdict[static_cast<size_t>(result.verdict)];
                sink(result);
                continue;
            }
            if (options.lowPriority) applyLowIoPriority(file);

            // --- reading -----------------------------------------------------
            const size_t wanted = static_cast<size_t>(facts.size);
            if (state.buffer.size() < wanted) state.buffer.resize(wanted);

            const auto readStart = Clock::now();
            size_t got = 0;
            bool ok = true;
            while (got < wanted) {
                const DWORD chunk =
                    static_cast<DWORD>(std::min<size_t>(wanted - got, 1u << 24));
                DWORD read = 0;
                if (!ReadFile(file, state.buffer.data() + got, chunk, &read, nullptr)) {
                    ok = false;
                    break;
                }
                if (read == 0) break;  // the file shrank in the meantime
                got += read;
            }
            const auto readEnd = Clock::now();
            state.stats.readSeconds += secondsBetween(readStart, readEnd);
            CloseHandle(file);

            if (!ok || got == 0) {
                result.verdict = ok ? GateVerdict::Empty : GateVerdict::Unreadable;
                ++state.stats.byVerdict[static_cast<size_t>(result.verdict)];
                sink(result);
                continue;
            }

            ++state.stats.filesRead;
            state.stats.bytesRead += got;

            // The size reported to the caller is the one ACTUALLY read, not the
            // one observed before opening. On a file being written to (a log)
            // the two differ, and recording the second would mark as indexed
            // content we never saw: on the next pass the file would count as
            // unchanged and its tail would never enter the index.
            result.size = got;

            const char* data = state.buffer.data();

            if (options.hashContent) {
                // Hash the bytes while they are still in the CPU cache: this is
                // the gate that will avoid re-extracting a file already done.
                const XXH128_hash_t hash = XXH3_128bits(data, got);
                result.hashLow = hash.low64;
                result.hashHigh = hash.high64;
            }

            // --- gate 1: the real format -----------------------------------
            result.actualKind = detectByMagic(data, got, result.declaredKind);
            if (result.actualKind == ContentKind::Skip) {
                result.verdict = GateVerdict::BinaryContent;
                ++state.stats.byVerdict[static_cast<size_t>(result.verdict)];
                sink(result);
                continue;
            }

            // --- gate 2: text or binary ------------------------------------
            const bool expectsText = result.actualKind == ContentKind::PlainText ||
                                     result.actualKind == ContentKind::Code ||
                                     result.actualKind == ContentKind::Markup;
            if (expectsText) {
                result.encoding = detectEncoding(data, got);
                if (result.encoding == TextEncoding::Binary) {
                    result.verdict = GateVerdict::BinaryContent;
                    ++state.stats.byVerdict[static_cast<size_t>(result.verdict)];
                    ++state.stats.byEncoding[static_cast<size_t>(result.encoding)];
                    sink(result);
                    continue;
                }
            }

            result.data = data;
            result.dataSize = got;
            result.verdict = GateVerdict::Pass;
            // (the buffer is shrunk after the sink call, see below)
            ++state.stats.byVerdict[static_cast<size_t>(result.verdict)];
            ++state.stats.byEncoding[static_cast<size_t>(result.encoding)];
            ++state.stats.byActualKind[static_cast<size_t>(result.actualKind)];
            sink(result);

            // The buffer grows to the size of the largest file seen and would
            // never shrink back: with sixteen threads and a few big PDFs that
            // is hundreds of MB held until the end of the run, on a machine
            // the user is using for something else.
            constexpr size_t kKeepBytes = 4u << 20;
            if (state.buffer.size() > kKeepBytes) {
                state.buffer.resize(kKeepBytes);
                state.buffer.shrink_to_fit();
            }
        }
    };

    for (unsigned t = 1; t < threadCount; ++t) workers.emplace_back(worker, t);
    worker(0);
    for (std::thread& thread : workers) thread.join();

    if (options.lowPriority) {
        MEMORY_PRIORITY_INFORMATION memory = {};
        memory.MemoryPriority = MEMORY_PRIORITY_NORMAL;
        SetProcessInformation(GetCurrentProcess(), ProcessMemoryPriority, &memory,
                              sizeof(memory));
    }

    ReaderStats total;
    total.threadsUsed = threadCount;
    for (const ThreadState& state : states) {
        total.filesSeen += state.stats.filesSeen;
        total.filesRead += state.stats.filesRead;
        total.bytesRead += state.stats.bytesRead;
        total.openSeconds += state.stats.openSeconds;
        total.readSeconds += state.stats.readSeconds;
        for (size_t i = 0; i < 8; ++i) {
            total.byVerdict[i] += state.stats.byVerdict[i];
            total.byEncoding[i] += state.stats.byEncoding[i];
        }
        for (size_t i = 0; i < 16; ++i) total.byActualKind[i] += state.stats.byActualKind[i];
    }
    total.elapsedSeconds = secondsBetween(start, Clock::now());
    return total;
}

}  // namespace filo
