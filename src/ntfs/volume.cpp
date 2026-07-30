#include "ntfs/volume.h"

#include <windows.h>
#include <winioctl.h>

namespace filo {

std::wstring formatWinError(unsigned long code) {
    LPWSTR buffer = nullptr;
    const DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);

    std::wstring result = len && buffer ? std::wstring(buffer, len)
                                       : L"unknown error";
    if (buffer) LocalFree(buffer);

    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) {
        result.pop_back();
    }
    return result + L" (code " + std::to_wstring(code) + L")";
}

void* openVolume(wchar_t drive, std::wstring* error) {
    // "\\.\C:" opens the VOLUME as if it were a file, stepping over the
    // filesystem. We are not opening a file inside C:, we are opening C:
    // itself.
    std::wstring devicePath = L"\\\\.\\";
    devicePath += drive;
    devicePath += L':';

    HANDLE handle = CreateFileW(
        devicePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,  // the volume is in use: mandatory
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_ACCESS_DENIED) {
            *error = L"access denied to the volume (administrator rights needed)";
        } else {
            *error = L"cannot open the volume: " + formatWinError(code);
        }
        return nullptr;
    }
    return handle;
}

uint32_t volumeSerial(wchar_t drive) {
    wchar_t root[4] = {drive, L':', L'\\', 0};
    DWORD serial = 0;
    if (!GetVolumeInformationW(root, nullptr, 0, &serial, nullptr, nullptr, nullptr, 0)) {
        return 0;
    }
    return serial;
}

bool queryJournal(void* volume, uint64_t* journalId, uint64_t* nextUsn) {
    HANDLE handle = static_cast<HANDLE>(volume);

    // Oversized buffer: depending on the Windows version the driver answers
    // with a V0, V1 or V2 structure. The fields we need sit at the start and
    // are identical in all of them.
    alignas(8) char buffer[256] = {};
    DWORD returned = 0;

    if (!DeviceIoControl(handle, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, buffer,
                         sizeof(buffer), &returned, nullptr)) {
        if (GetLastError() != ERROR_JOURNAL_NOT_ACTIVE) return false;

        // The journal does not exist on this volume: we create it. From here
        // on NTFS will record every change, and that is what makes the
        // incremental update possible instead of a rescan.
        CREATE_USN_JOURNAL_DATA create{};
        create.MaximumSize = 32ull << 20;   // 32 MB of history
        create.AllocationDelta = 4ull << 20;
        if (!DeviceIoControl(handle, FSCTL_CREATE_USN_JOURNAL, &create, sizeof(create),
                             nullptr, 0, &returned, nullptr)) {
            return false;
        }
        if (!DeviceIoControl(handle, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, buffer,
                             sizeof(buffer), &returned, nullptr)) {
            return false;
        }
    }

    if (returned < sizeof(USN_JOURNAL_DATA_V0)) return false;

    const auto* journal = reinterpret_cast<const USN_JOURNAL_DATA_V0*>(buffer);
    *journalId = journal->UsnJournalID;
    *nextUsn = static_cast<uint64_t>(journal->NextUsn);
    return true;
}

}  // namespace filo
