#pragma once

#include <cstdint>
#include <string>

#include "index/file_index.h"

namespace filo {

struct EnumStats {
    uint64_t recordCount = 0;   // records read from the MFT
    uint64_t bytesRead = 0;     // bytes transferred by the NTFS driver
    double   elapsedSeconds = 0.0;
};

// Reads the entire MFT of an NTFS volume into a FileIndex.
//
// The MFT is NTFS's internal database: every file and folder has an entry, and
// that entry holds ONLY the local name ("photo.jpg"), never the full path.
// Asking the driver to dump that table costs seconds; recursing through the
// directories would cost minutes.
class MftEnumerator {
public:
    explicit MftEnumerator(wchar_t driveLetter);
    ~MftEnumerator();

    MftEnumerator(const MftEnumerator&) = delete;
    MftEnumerator& operator=(const MftEnumerator&) = delete;

    // Full scan into `index`, which is overwritten.
    bool enumerate(FileIndex* index, std::wstring* error);

    const EnumStats& stats() const { return stats_; }

private:
    wchar_t drive_;
    void*   volume_ = nullptr;  // HANDLE, kept as void* so this header does not
                                // drag in windows.h
    EnumStats stats_;
};

}  // namespace filo
