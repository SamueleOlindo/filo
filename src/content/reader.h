#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "content/gate.h"
#include "content/selector.h"
#include "index/file_index.h"

namespace filo {

struct ReadResult {
    uint32_t slot = 0;                 // position in the FileIndex
    GateVerdict verdict = GateVerdict::Pass;
    ContentKind declaredKind = ContentKind::Skip;  // from the extension
    ContentKind actualKind = ContentKind::Skip;    // from the magic bytes
    TextEncoding encoding = TextEncoding::Unknown;

    uint64_t size = 0;
    int64_t  mtime = 0;
    uint64_t hashLow = 0;   // XXH3-128 of the content
    uint64_t hashHigh = 0;

    // Pointer into the buffer of the thread that read the file: valid ONLY for
    // the duration of the callback. Anyone wanting to keep the bytes copies
    // them.
    const char* data = nullptr;
    size_t dataSize = 0;
};

struct ReaderStats {
    uint64_t filesSeen = 0;
    uint64_t filesRead = 0;
    uint64_t bytesRead = 0;
    uint64_t byVerdict[8] = {};
    uint64_t byEncoding[8] = {};
    uint64_t byActualKind[16] = {};

    double elapsedSeconds = 0.0;
    // Opens and reads measured separately: across tens of thousands of small
    // files the dominant cost is CreateFile/CloseHandle traversing the
    // antivirus filters, not moving the bytes.
    double openSeconds = 0.0;
    double readSeconds = 0.0;
    unsigned threadsUsed = 0;
};

class ContentReader {
public:
    struct Options {
        unsigned threads = 0;      // 0 = automatic
        bool lowPriority = true;   // background I/O and memory priority
        bool hashContent = true;
    };

    // The callback is invoked FROM SEVERAL THREADS: it must be thread-safe.
    using Sink = std::function<void(const ReadResult&)>;

    static ReaderStats run(const FileIndex& index,
                           const std::vector<uint32_t>& candidates,
                           const Options& options, const Sink& sink);
};

}  // namespace filo
