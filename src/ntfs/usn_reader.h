#pragma once

#include <cstdint>
#include <string>

#include "index/file_index.h"

namespace filo {

struct CatchupStats {
    uint64_t recordsRead = 0;   // entries read from the journal
    uint64_t created = 0;
    uint64_t deleted = 0;
    uint64_t updated = 0;       // rename or move
    bool     needsFullScan = false;  // the journal no longer covers our position
    double   elapsedSeconds = 0.0;
};

// Brings an index loaded from a snapshot up to date with what changed since.
//
// NTFS keeps a change log (the USN Journal): every creation, deletion and
// rename leaves an entry there with a sequence number. The snapshot remembers
// which number it reached, so here we ask only for the delta instead of
// re-reading the whole MFT.
class UsnReader {
public:
    explicit UsnReader(wchar_t driveLetter);
    ~UsnReader();

    UsnReader(const UsnReader&) = delete;
    UsnReader& operator=(const UsnReader&) = delete;

    // Applies the changes to `index`. If the journal was reset or has scrolled
    // past our resume point, it sets needsFullScan: the index is then left
    // untouched and a full scan has to be redone.
    bool catchUp(FileIndex* index, CatchupStats* stats, std::wstring* error);

private:
    wchar_t drive_;
    void*   volume_ = nullptr;
};

}  // namespace filo
