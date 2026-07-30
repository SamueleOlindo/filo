#include "reclaim/recycle.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl.h>

#include <algorithm>

namespace filo {
namespace {

// The shell cannot address a path longer than this, and there is no \\?\ escape
// that works here. Better to skip the file and say so than to hand the shell
// something it will silently mishandle.
constexpr size_t kMaxUsablePath = MAX_PATH - 1;

std::wstring volumeGuidOf(wchar_t drive) {
    wchar_t mount[4] = {drive, L':', L'\\', L'\0'};
    wchar_t guid[64] = {};
    if (!GetVolumeNameForVolumeMountPointW(mount, guid, 64)) return {};

    // "\\?\Volume{...}\" -> "{...}"
    std::wstring text = guid;
    const size_t open = text.find(L'{');
    const size_t close = text.find(L'}');
    if (open == std::wstring::npos || close == std::wstring::npos) return {};
    return text.substr(open, close - open + 1);
}

std::wstring logPathFor() {
    wchar_t base[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    std::wstring directory = (length > 0 && length < MAX_PATH) ? base : L".";
    directory += L"\\filo";
    CreateDirectoryW(directory.c_str(), nullptr);
    return directory + L"\\recycled.log";
}

void appendLine(HANDLE file, const std::wstring& line) {
    if (file == INVALID_HANDLE_VALUE) return;
    const std::wstring text = line + L"\r\n";
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0,
                                          nullptr, nullptr);
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        utf8.data(), bytes, nullptr, nullptr);
    DWORD written = 0;
    WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

enum class Check { Ok, Missing, Changed, Locked };

// Is the file at this path still the one we measured — and can we keep it that
// way until it is gone?
//
// The old version asked GetFileAttributesEx about a PATH and compared size and
// time. Both halves were weak. A path is not a file: between the question and
// the deletion, anything could arrive at that name, and the shell would have
// deleted whatever it found. And size and time alone cannot tell one file from
// another that happens to match.
//
// This opens the file and answers from the HANDLE, then leaves it open. The
// share mode is the point: FILE_SHARE_DELETE so the shell can still move it to
// the bin, and no FILE_SHARE_WRITE, so from this moment nobody can change the
// bytes we just checked. The window between "verified" and "deleted" stops
// being a window.
//
// What it still does not do is re-read the contents. For a duplicate, the claim
// is that this file is identical to one being kept, and a hash would prove it —
// but proving it means reading gigabytes again for every deletion, and any
// ordinary modification already moves the timestamp. What is left uncovered is
// someone rewriting a file and restoring its size and timestamp deliberately.
// That is an attack, not an accident, and it is not what this guards against.
Check verifyAndHold(const BinCandidate& candidate, HANDLE* held) {
    *held = CreateFileW(candidate.path.c_str(), GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_NO_RECALL, nullptr);
    if (*held == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return Check::Missing;
        }
        // Most often a sharing violation: somebody has it open for writing. Not
        // an error on our side, and a good reason to leave it alone.
        return Check::Locked;
    }

    BY_HANDLE_FILE_INFORMATION facts{};
    if (!GetFileInformationByHandle(*held, &facts)) {
        CloseHandle(*held);
        *held = INVALID_HANDLE_VALUE;
        return Check::Changed;
    }

    const uint64_t size =
        (static_cast<uint64_t>(facts.nFileSizeHigh) << 32) | facts.nFileSizeLow;
    const int64_t mtime =
        (static_cast<int64_t>(facts.ftLastWriteTime.dwHighDateTime) << 32) |
        facts.ftLastWriteTime.dwLowDateTime;
    const uint64_t frn =
        (static_cast<uint64_t>(facts.nFileIndexHigh) << 32) | facts.nFileIndexLow;

    bool same = size == candidate.size && mtime == candidate.mtime;
    // The scan does not always know these; when it does, they have to agree.
    // The low 48 bits are the MFT record and the top 16 the sequence number,
    // and NTFS reuses records — so this is compared whole, never masked.
    if (same && candidate.frn != 0 && candidate.frn != frn) same = false;
    if (same && candidate.volumeSerial != 0 &&
        candidate.volumeSerial != facts.dwVolumeSerialNumber) {
        same = false;
    }

    if (!same) {
        CloseHandle(*held);
        *held = INVALID_HANDLE_VALUE;
        return Check::Changed;
    }
    return Check::Ok;
}

// Moves one item to the Recycle Bin and says whether it got there.
//
// One IFileOperation per file, deliberately. Handed a batch, the shell reports
// a single status for the lot, and "some of these moved" is not something Filo
// can act on: striking a file from the index requires knowing that THAT file
// moved. One item per operation makes GetAnyOperationsAborted an answer about
// one file. The cost is a COM round trip each, which is nothing against the
// disk work, and a whole directory still counts as one item.
//
// IFileOperation rather than SHFileOperation because Microsoft recommends it
// and because it is the one that can be asked about a single item. The flags
// carry the same meaning as before, including FOF_WANTNUKEWARNING: the shell
// stops and asks before it destroys anything that will not fit the bin, which
// is the guarantee that survives a wrong capacity estimate.
bool recycleOne(const std::wstring& path) {
    Microsoft::WRL::ComPtr<IFileOperation> operation;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&operation)))) {
        return false;
    }
    if (FAILED(operation->SetOperationFlags(
            FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING |
            FOF_NOERRORUI | FOF_SILENT | FOFX_RECYCLEONDELETE))) {
        return false;
    }

    Microsoft::WRL::ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        return false;
    }
    if (FAILED(operation->DeleteItem(item.Get(), nullptr))) return false;
    if (FAILED(operation->PerformOperations())) return false;

    // PerformOperations returns S_OK for an operation that ran, not for one
    // that succeeded. This is the part that says whether the file moved.
    BOOL aborted = FALSE;
    if (FAILED(operation->GetAnyOperationsAborted(&aborted))) return false;
    return aborted == FALSE;
}

// What to assume the bin holds when Windows has not written it down.
//
// This is a GUESS, and it is deliberately a low one. Guessing high means
// accepting a file that will not fit, and a file that does not fit is destroyed
// rather than recycled. Guessing low means refusing a file that would have gone
// in, which costs the user a click and nothing else.
//
// The real guarantee is not this number. It is FOF_WANTNUKEWARNING at the call
// site, which makes Windows itself stop and ask before it destroys anything.
// This estimate only keeps the common cases from ever reaching that prompt.
void estimateCapacity(wchar_t drive, RecycleBinInfo* info) {
    const wchar_t root[4] = {drive, L':', L'\\', L'\0'};
    ULARGE_INTEGER total{};
    if (!GetDiskFreeSpaceExW(root, nullptr, &total, nullptr)) return;

    info->maxBytes = total.QuadPart / 20;   // five per cent
    info->capacityKnown = false;
}

}  // namespace

RecycleBinInfo queryRecycleBin(wchar_t drive) {
    RecycleBinInfo info;

    const wchar_t root[4] = {drive, L':', L'\\', L'\0'};
    SHQUERYRBINFO query{};
    query.cbSize = sizeof(query);
    if (SUCCEEDED(SHQueryRecycleBinW(root, &query))) {
        info.usedBytes = static_cast<uint64_t>(query.i64Size);
    }

    const std::wstring guid = volumeGuidOf(drive);
    if (guid.empty()) return info;

    const std::wstring key =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\BitBucket\\Volume\\" +
        guid;

    HKEY handle = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_QUERY_VALUE, &handle) !=
        ERROR_SUCCESS) {
        // No per-volume settings recorded: Windows is on its defaults, so the
        // bin exists but its capacity is not written down anywhere we can read.
        info.usable = true;
        estimateCapacity(drive, &info);
        return info;
    }

    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;

    // NukeOnDelete: the bin is switched off for this volume, and FOF_ALLOWUNDO
    // becomes a permanent delete with no warning and no error.
    if (RegQueryValueExW(handle, L"NukeOnDelete", nullptr, &type,
                         reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS &&
        type == REG_DWORD) {
        info.usable = (value == 0);
    } else {
        info.usable = true;
    }

    value = 0;
    size = sizeof(value);
    if (RegQueryValueExW(handle, L"MaxCapacity", nullptr, &type,
                         reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS &&
        type == REG_DWORD) {
        info.maxBytes = static_cast<uint64_t>(value) * 1024ull * 1024ull;
        info.capacityKnown = true;
    }
    RegCloseKey(handle);

    if (!info.capacityKnown) estimateCapacity(drive, &info);
    if (info.maxBytes > info.usedBytes) info.freeBytes = info.maxBytes - info.usedBytes;
    return info;
}

BinResult moveToRecycleBin(wchar_t drive, const std::vector<BinCandidate>& candidates,
                           bool dryRun) {
    BinResult result;
    if (candidates.empty()) return result;

    const RecycleBinInfo bin = queryRecycleBin(drive);
    if (!bin.usable) {
        result.error =
            L"the Recycle Bin is disabled on this drive, so deleting would be permanent";
        return result;
    }

    // COM, for IFileOperation. Apartment-threaded because the shell is, and
    // this runs on the reclaim worker rather than the UI thread. If the thread
    // already initialised it, RPC_E_CHANGED_MODE says so and nothing needs
    // doing — but then this function must not uninitialise it either.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool ownsCom = SUCCEEDED(com);
    if (!ownsCom && com != RPC_E_CHANGED_MODE) {
        result.error = L"the shell could not be reached to move anything";
        return result;
    }

    result.logPath = logPathFor();
    HANDLE log = INVALID_HANDLE_VALUE;
    uint64_t roomLeft = bin.freeBytes;

    for (const BinCandidate& candidate : candidates) {
        if (candidate.path.size() > kMaxUsablePath) {
            ++result.failed;
            continue;
        }

        // Verified from a handle, and the handle stays open until the file is
        // gone. Everything checked here is still true at the moment of the
        // deletion, because nothing else can write to it in between.
        HANDLE held = INVALID_HANDLE_VALUE;
        switch (verifyAndHold(candidate, &held)) {
            case Check::Missing: ++result.missing; continue;
            case Check::Locked:  ++result.locked;  continue;
            // The index is stale by construction on a machine that cannot read
            // the USN journal, so this is expected and not an error.
            case Check::Changed: ++result.changed; continue;
            case Check::Ok: break;
        }

        // Too large for the bin means Windows destroys it instead of recycling
        // it. A background process must not accept that on the user's behalf.
        //
        // This used to read "if (bin.maxBytes != 0)", so an unreadable capacity
        // skipped the check entirely — and an unreadable capacity is the NORMAL
        // case, because Windows only writes one down once someone changes it.
        // Unknown now means a conservative estimate, never a free pass.
        if (candidate.size > roomLeft) {
            CloseHandle(held);
            ++result.wouldBePermanent;
            continue;
        }

        if (dryRun) {
            CloseHandle(held);
            roomLeft -= candidate.size;
            result.bytesFreed += candidate.size;
            ++result.binned;
            continue;
        }

        const bool moved = recycleOne(candidate.path);
        CloseHandle(held);

        if (!moved) {
            ++result.failed;
            continue;
        }

        roomLeft -= candidate.size;
        result.bytesFreed += candidate.size;
        ++result.binned;
        // Only now, and only for this file. The caller strikes these from the
        // index, so anything that did not actually move must not be in here.
        if (candidate.slot != 0xFFFFFFFFu) result.recycledSlots.push_back(candidate.slot);

        if (log == INVALID_HANDLE_VALUE) {
            log = CreateFileW(result.logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            SYSTEMTIME now{};
            GetLocalTime(&now);
            wchar_t stamp[64];
            swprintf_s(stamp, L"%04u-%02u-%02u %02u:%02u:%02u  recycled:", now.wYear,
                       now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
            appendLine(log, stamp);
        }
        wchar_t line[32];
        swprintf_s(line, L"  %10llu  ", static_cast<unsigned long long>(candidate.size));
        appendLine(log, std::wstring(line) + candidate.path);
    }

    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (ownsCom) CoUninitialize();
    return result;
}

}  // namespace filo
