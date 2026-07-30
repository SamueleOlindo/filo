#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "index/file_index.h"
#include "reclaim/scan.h"

namespace filo {

// Files that share a size, before anything has been read.
//
// Stage one of the funnel, and free: phase 0 already measured every file. What
// it produces is a work list, ordered by what it could possibly be worth.
struct SizeGroup {
    uint64_t size = 0;
    std::vector<uint32_t> slots;

    // Most that could be freed if every copy but one turned out identical.
    uint64_t potential() const {
        return slots.size() < 2 ? 0 : size * (slots.size() - 1);
    }
};

// Groups the measured files by size. No disk access.
//
// `index` is needed to check where each file lives: copies inside a package
// store, a git object database or node_modules are excluded, because there a
// duplicate is the design and deleting one breaks the thing that made it.
std::vector<SizeGroup> groupBySize(const FileIndex& index,
                                   const std::vector<FileFact>& facts,
                                   uint64_t minimumSize);

struct DuplicateOptions {
    // Below this, a duplicate is not worth opening the file for.
    //
    // The cost of this whole feature is PER FILE, not per byte: on an SSD the
    // fingerprint pass managed 18 MB/s, which is the file-open path and the
    // antivirus filter stack, not the disk. Reading 128 KB from a 5 KB file
    // costs almost exactly what reading it from a 5 GB file costs, while
    // freeing a millionth as much.
    uint64_t minimumSize = 1u << 20;   // 1 MB

    // Stop after opening this many files, 0 for no limit. Groups are visited in
    // descending order of what they could free, so a budget spends itself on
    // the largest wins first.
    size_t fileBudget = 0;

    // Called from the worker threads as the pass advances. A minute of silence
    // reads as a hang, and this pass takes about that long.
    std::function<void(const wchar_t* phase, uint64_t done, uint64_t total)> onProgress;
};

// A set of files with identical contents. The first slot is the one to keep.
struct DuplicateGroup {
    uint64_t bytesEach = 0;
    uint64_t reclaimable = 0;      // bytesEach * (members - 1)
    std::vector<uint32_t> slots;   // slots[0] is the keeper
};

struct DuplicateStats {
    uint64_t considered = 0;        // files above the floor
    uint64_t sharingASize = 0;      // files whose size is not unique
    uint64_t fingerprinted = 0;
    uint64_t fingerprintBytes = 0;  // read to fingerprint
    uint64_t fullyHashed = 0;
    uint64_t fullHashBytes = 0;     // read to confirm
    uint64_t hardlinks = 0;         // same FRN: deleting one frees nothing
    uint64_t unreadable = 0;

    uint64_t groups = 0;
    uint64_t reclaimableBytes = 0;

    double sizeSeconds = 0.0;
    double fingerprintSeconds = 0.0;
    double fullHashSeconds = 0.0;
};

// Finds files with identical contents, cheaply.
//
// THE FUNNEL
//
// Reading 375 GB to compare everything is not an option, and it is not
// necessary. Three stages, each one only paying for what the previous could not
// rule out:
//
//  1. SIZE. Free — phase 0 already measured it — and most sizes are unique, so
//     this discards the overwhelming majority without touching the disk.
//  2. FINGERPRINT. The first and last 64 KB plus the size, hashed. A fixed
//     128 KB per file regardless of how large the file is.
//  3. FULL HASH. Only where fingerprints agree. Two different files that share
//     a size AND both ends are rare but real — disk images and database files
//     do it — and this is about to delete something, so "rare" is not a
//     standard worth working to.
//
// HARDLINKS
//
// Two names for one MFT record share a File Reference Number. They have
// identical contents by definition and deleting one frees nothing at all, so
// they are counted and excluded rather than offered as a saving that will not
// materialize.
std::vector<DuplicateGroup> findExactDuplicates(const FileIndex& index,
                                                const std::vector<FileFact>& facts,
                                                const DuplicateOptions& options,
                                                DuplicateStats* stats,
                                                unsigned threadCount = 0);

// Which copy to keep, given a set of paths.
//
// Preference order: not in Downloads or Temp, then the shallowest path, then
// the oldest. The reasoning is that a file survives in the place it was filed
// and accumulates copies in the places it passed through — and that the oldest
// copy is the one other things are most likely to point at.
uint32_t chooseKeeper(const FileIndex& index, const std::vector<FileFact>& facts,
                      const std::vector<uint32_t>& members);

}  // namespace filo
