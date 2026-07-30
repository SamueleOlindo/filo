#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace filo {

// Watches folders for changes, for machines where the journal is out of reach.
//
// WHY THIS EXISTS
//
// Reading the MFT and the USN journal needs administrator rights. On an ordinary
// account the first run cannot build an index at all, and once one exists it
// goes stale with nothing to correct it: every file created, deleted or renamed
// by another program stays wrong until somebody runs Filo elevated again. That
// is the difference between a tool a person can use and a tool a person has to
// be an administrator to use.
//
// ReadDirectoryChangesW needs no rights beyond being able to read the folder. It
// cannot do what the journal does — it does not see the whole volume, it does
// not survive the process, and it drops everything if changes arrive faster than
// they are collected — but it does see the folders somebody actually searches.
//
// WHAT IT DELIBERATELY DOES NOT DO
//
// It does not touch the index. It reports paths, and the caller decides what
// they mean, because the caller is the one holding the lock that makes a change
// to the index safe. Keeping those two apart is what stops this from becoming a
// second, quieter writer to the index.
class FolderWatcher {
public:
    // What happened to a path. Renames arrive as a pair, so both halves are
    // needed to move an entry rather than lose one and invent another.
    enum class Change { Added, Removed, Modified, RenamedFrom, RenamedTo };

    struct Event {
        Change       what = Change::Modified;
        std::wstring path;
    };

    // Called from the watcher's own thread with everything collected since the
    // last call. Batched rather than one at a time: a build writes thousands of
    // files in a second, and taking the index lock for each one would make the
    // interface stutter for the sake of paths that are about to change again.
    using Sink = std::function<void(const std::vector<Event>&)>;

    FolderWatcher() = default;
    ~FolderWatcher();

    FolderWatcher(const FolderWatcher&) = delete;
    FolderWatcher& operator=(const FolderWatcher&) = delete;

    // Starts watching. Roots that cannot be opened are skipped, not fatal: a
    // folder that has gone away is not a reason to stop watching the others.
    // Returns how many roots are actually being watched.
    size_t start(const std::vector<std::wstring>& roots, Sink sink);
    void stop();

    // Whether the buffer overflowed at some point, meaning changes were lost.
    //
    // Worth surfacing rather than hiding: it means the index is incomplete in a
    // way watching alone will not repair, and the honest answer is to say the
    // index needs rebuilding rather than to quietly return wrong results.
    bool overflowed() const { return overflowed_; }

private:
    struct Root;

    std::vector<Root*> roots_;
    void* stopEvent_ = nullptr;
    void* thread_ = nullptr;
    Sink  sink_;
    bool  overflowed_ = false;

    void runLoop();
};

}  // namespace filo
