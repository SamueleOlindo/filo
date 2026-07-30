#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "reclaim/scan.h"

namespace filo {

// What the Recycle Bin can actually do on a given drive.
//
// FOF_ALLOWUNDO is a REQUEST, not a guarantee. Windows silently deletes
// permanently in two cases: when the bin is disabled for the volume, and when
// the file is larger than the bin's capacity. Both are invisible from the
// return code, so both are checked before anything is asked for.
struct RecycleBinInfo {
    bool     usable = false;    // false when NukeOnDelete is set: deletes are permanent
    // Whether maxBytes came from Windows or from an assumption. Windows records
    // a per-volume capacity only once something has changed it, so the common
    // case is that it is NOT recorded and there is nothing to read. That used to
    // switch the size check off completely, which is the opposite of what an
    // unknown limit should do.
    bool     capacityKnown = false;
    uint64_t maxBytes = 0;      // measured, or a conservative estimate
    uint64_t usedBytes = 0;
    uint64_t freeBytes = 0;     // what a deletion can still fit into
};

RecycleBinInfo queryRecycleBin(wchar_t drive);

// One file to move, with what we measured about it.
//
// None of these fields is decoration. Every one is checked again at the moment
// of deletion, from a handle to the open file rather than from the path, and
// any mismatch means the file is left alone. On a machine that cannot read the
// USN journal the index is stale by definition, so "the file we measured" and
// "the file at this path now" are two different claims and have to be treated
// as such.
struct BinCandidate {
    std::wstring path;
    uint64_t size = 0;
    int64_t  mtime = 0;

    // The NTFS file reference number, and the volume it belongs to. Size and
    // time say the file was not written to; these say it is the same file
    // OBJECT and not a different one that arrived at the same path.
    uint64_t frn = 0;             // 0 = unknown, skip the check
    uint32_t volumeSerial = 0;    // 0 = unknown, skip the check

    // Which index slot this came from, so the caller can strike the file from
    // the index once it is really gone. Filo moved it: nothing else has to
    // notice, and no rescan is needed to find out.
    uint32_t slot = 0xFFFFFFFFu;
};

struct BinResult {
    uint64_t binned = 0;
    uint64_t bytesFreed = 0;

    uint64_t missing = 0;       // gone since it was measured
    uint64_t changed = 0;       // size, timestamp or identity no longer match
    uint64_t wouldBePermanent = 0;  // larger than the bin can hold
    uint64_t locked = 0;        // another process is writing to it
    uint64_t failed = 0;

    // The slots of the files that actually reached the bin. Not the ones that
    // were asked for: the ones the shell confirmed, one item at a time. This is
    // what makes it safe to strike them from the index.
    std::vector<uint32_t> recycledSlots;

    std::wstring logPath;
    std::wstring error;
};

// Moves files to the Recycle Bin. Never deletes.
//
// Refuses outright if the bin is disabled on that drive, because there
// FOF_ALLOWUNDO means nothing and every "move" would be a permanent deletion.
// Skips any file that has changed since it was measured, and any file too large
// for the bin to hold — that one would be destroyed rather than recycled, and a
// background process must never accept that on the user's behalf.
//
// When the bin's capacity cannot be read — the common case, because Windows
// only records one after somebody changes it — a low estimate is used rather
// than the check being skipped. If even the volume size cannot be read, nothing
// is accepted at all. An unknown limit refuses; it does not wave things
// through.
//
// Writes a log of what went where. The bin is the undo, but only for someone
// who knows what to look for.
BinResult moveToRecycleBin(wchar_t drive, const std::vector<BinCandidate>& candidates,
                           bool dryRun);

}  // namespace filo
