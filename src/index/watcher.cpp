#include "index/watcher.h"

#include <windows.h>

#include <thread>

namespace filo {
namespace {

// Per root. Big enough that an ordinary burst fits, small enough that watching
// a handful of folders costs kilobytes rather than megabytes.
//
// The buffer is where this goes wrong if it goes wrong: when it fills, Windows
// reports ERROR_NOTIFY_ENUM_DIR and throws the contents away rather than
// truncating them, so what is lost is unknown. That is reported as an overflow
// instead of being papered over, because "some changes were lost, we do not know
// which" is a different situation from "no changes happened".
constexpr DWORD kBufferBytes = 64 * 1024;

constexpr DWORD kInterestingChanges =
    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
    FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;

}  // namespace

// One watched folder, with the overlapped state its read needs to stay alive
// between calls.
struct FolderWatcher::Root {
    std::wstring path;
    HANDLE       directory = INVALID_HANDLE_VALUE;
    OVERLAPPED   overlapped{};
    // Alignment matters: the structures Windows writes here contain DWORDs, and
    // a misaligned buffer is undefined behaviour on the read of them.
    alignas(sizeof(DWORD)) unsigned char buffer[kBufferBytes] = {};
    bool pending = false;
};

FolderWatcher::~FolderWatcher() { stop(); }

size_t FolderWatcher::start(const std::vector<std::wstring>& roots, Sink sink) {
    stop();
    sink_ = std::move(sink);
    overflowed_ = false;

    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) return 0;

    for (const std::wstring& path : roots) {
        // FILE_FLAG_BACKUP_SEMANTICS is what makes it legal to open a DIRECTORY
        // with CreateFile at all; without it this fails on every root.
        //
        // FILE_SHARE_DELETE matters too: watching somebody's profile must never
        // be the reason they cannot rename a folder inside it.
        const HANDLE directory = CreateFileW(
            path.c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (directory == INVALID_HANDLE_VALUE) continue;

        auto* root = new Root();
        root->path = path;
        root->directory = directory;
        root->overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!root->overlapped.hEvent) {
            CloseHandle(directory);
            delete root;
            continue;
        }
        roots_.push_back(root);
    }

    if (roots_.empty()) {
        CloseHandle(static_cast<HANDLE>(stopEvent_));
        stopEvent_ = nullptr;
        return 0;
    }

    auto* worker = new std::thread([this] { runLoop(); });
    thread_ = worker;
    return roots_.size();
}

void FolderWatcher::stop() {
    if (stopEvent_) SetEvent(static_cast<HANDLE>(stopEvent_));

    if (thread_) {
        auto* worker = static_cast<std::thread*>(thread_);
        // Every outstanding read has to be cancelled before the handle closes,
        // or the kernel is left writing into a buffer that has been freed.
        for (Root* root : roots_) {
            if (root->pending) CancelIoEx(root->directory, &root->overlapped);
        }
        if (worker->joinable()) worker->join();
        delete worker;
        thread_ = nullptr;
    }

    for (Root* root : roots_) {
        if (root->overlapped.hEvent) CloseHandle(root->overlapped.hEvent);
        if (root->directory != INVALID_HANDLE_VALUE) CloseHandle(root->directory);
        delete root;
    }
    roots_.clear();

    if (stopEvent_) {
        CloseHandle(static_cast<HANDLE>(stopEvent_));
        stopEvent_ = nullptr;
    }
}

void FolderWatcher::runLoop() {
    const HANDLE stop = static_cast<HANDLE>(stopEvent_);

    // stopEvent first, then one per root: WaitForMultipleObjects returns the
    // LOWEST signalled index, so putting stop at zero means a stop is never
    // starved by a folder that keeps changing.
    std::vector<HANDLE> waits;
    waits.push_back(stop);
    for (Root* root : roots_) waits.push_back(root->overlapped.hEvent);

    auto arm = [&](Root* root) {
        ResetEvent(root->overlapped.hEvent);
        root->pending = ReadDirectoryChangesW(root->directory, root->buffer, kBufferBytes,
                                              /*watchSubtree=*/TRUE, kInterestingChanges,
                                              nullptr, &root->overlapped, nullptr) != FALSE;
        return root->pending;
    };

    for (Root* root : roots_) arm(root);

    std::vector<Event> batch;
    for (;;) {
        const DWORD signalled = WaitForMultipleObjects(static_cast<DWORD>(waits.size()),
                                                       waits.data(), FALSE, INFINITE);
        if (signalled == WAIT_OBJECT_0 || signalled == WAIT_FAILED) return;

        const size_t which = signalled - WAIT_OBJECT_0 - 1;
        if (which >= roots_.size()) return;
        Root* root = roots_[which];
        root->pending = false;

        DWORD written = 0;
        if (!GetOverlappedResult(root->directory, &root->overlapped, &written, FALSE)) {
            const DWORD error = GetLastError();
            if (error == ERROR_OPERATION_ABORTED) return;   // stop() cancelled it
            // Anything else: the buffer overflowed, or the folder went away.
            // Re-arm and carry on, having admitted the gap.
            overflowed_ = true;
            if (!arm(root)) return;
            continue;
        }

        // A zero-length result IS the overflow signal: Windows discarded the
        // contents rather than handing back half of them, so what was lost
        // cannot be enumerated. Saying so is the whole point of recording it.
        if (written == 0) {
            overflowed_ = true;
            if (!arm(root)) return;
            continue;
        }

        batch.clear();
        const unsigned char* cursor = root->buffer;
        for (;;) {
            const auto* notify = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(cursor);

            Event event;
            switch (notify->Action) {
                case FILE_ACTION_ADDED:            event.what = Change::Added; break;
                case FILE_ACTION_REMOVED:          event.what = Change::Removed; break;
                case FILE_ACTION_MODIFIED:         event.what = Change::Modified; break;
                case FILE_ACTION_RENAMED_OLD_NAME: event.what = Change::RenamedFrom; break;
                case FILE_ACTION_RENAMED_NEW_NAME: event.what = Change::RenamedTo; break;
                default:                           event.what = Change::Modified; break;
            }

            // FileName is NOT null-terminated and FileNameLength is in BYTES.
            // Reading it as a string would run off the end into the next record.
            const size_t characters = notify->FileNameLength / sizeof(wchar_t);
            event.path = root->path;
            if (!event.path.empty() && event.path.back() != L'\\') event.path += L'\\';
            event.path.append(notify->FileName, characters);
            batch.push_back(std::move(event));

            if (notify->NextEntryOffset == 0) break;
            cursor += notify->NextEntryOffset;
        }

        // Re-armed BEFORE the sink runs. The sink takes the index lock and can
        // block for a moment; leaving the read disarmed for that moment is
        // exactly when a burst would overflow the buffer we are not reading.
        if (!arm(root)) return;
        if (sink_ && !batch.empty()) sink_(batch);
    }
}

}  // namespace filo
