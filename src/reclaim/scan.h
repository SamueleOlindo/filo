#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "index/file_index.h"

namespace filo {

// Size and modification time for one file.
//
// NOT STORED IN THE INDEX, and that is a deliberate reversal of the first
// design. Putting them in the snapshot would cost about 38 MB on 2.4 million
// records, need a new snapshot format with two migrations behind it, and — the
// part that settles it — go stale exactly like everything else, because the USN
// journal does not carry file size. There would be no way to refresh them short
// of doing this pass anyway.
//
// So it is done on demand, when the reclaim screen opens: a few seconds, always
// accurate, and nothing resident while the user is only searching.
struct FileFact {
    uint32_t slot = 0;      // position in the FileIndex
    uint64_t size = 0;      // bytes on disk
    int64_t  mtime = 0;     // FILETIME, UTC
};

// Children grouped by parent, in one array with an offset per record.
//
// Counting sort rather than a comparison sort: parent indices are already
// dense integers in [0, size), so two linear passes beat 2.4 million
// comparisons and the result is the same.
//
// Public because the classifier walks the same tree. It is the only way to ask
// "what is under this folder" without a second trip to the disk, and the disk
// pass costs seconds while this costs about 20 MB and two linear passes.
struct ChildIndex {
    std::vector<uint32_t> offset;   // size + 1 boundaries
    std::vector<uint32_t> child;
};

ChildIndex buildChildIndex(const FileIndex& index);

struct ScanStats {
    uint64_t examined = 0;      // records considered
    uint64_t measured = 0;      // files we got an answer for
    uint64_t directories = 0;
    uint64_t unreadable = 0;    // gone, locked, or denied
    uint64_t cloudOnly = 0;     // OneDrive placeholders: reading downloads them
    uint64_t reparsePoints = 0; // symlinks and junctions: counted where they point
    uint64_t totalBytes = 0;

    double   elapsedSeconds = 0.0;
    unsigned threadsUsed = 0;
};

// Measures every live file in the index.
//
// Skips what must be skipped rather than what is convenient:
//
//  - CLOUD PLACEHOLDERS. A OneDrive file that is "online only" occupies almost
//    nothing locally, and touching its contents would download it. Reporting it
//    as reclaimable space would be wrong twice over.
//  - REPARSE POINTS. A junction is another name for a directory that is already
//    counted somewhere else; following it inflates every total and can loop.
//  - DIRECTORIES, which have no size of their own.
//
// This does NOT use selectCandidates(): that excludes node_modules, build and
// the package caches, which are precisely the folders this feature exists to
// find. Indexing and reclaiming want opposite lists.
// Walks the DIRECTORIES and reads their contents in batches.
//
// A per-file GetFileAttributesExW costs a full path resolution each time — 263
// microseconds on the development machine, which is the antivirus filter stack
// and not the disk. FindFirstFileEx pays that once per directory and then reads
// the rest out of a buffer the file system already filled: 473,000 openings
// instead of 1,870,000.
//
// Measured on 375 GB and 1.87 million files: 5.9 seconds against 35.9, a factor
// of six. It needs a child index to attach each enumerated name back to its
// record, which costs two linear passes and about 20 MB while it runs.
std::vector<FileFact> collectFileFacts(const FileIndex& index, ScanStats* stats,
                                       unsigned threadCount = 0);

// The obvious implementation: one stat per file.
//
// Kept because the comparison is the entire justification for the one above,
// and because a claim of "six times faster" that cannot be re-run is not a
// measurement, it is a memory. --reclaimscan times both.
std::vector<FileFact> collectFileFactsPerFile(const FileIndex& index, ScanStats* stats,
                                              unsigned threadCount = 0);

}  // namespace filo
