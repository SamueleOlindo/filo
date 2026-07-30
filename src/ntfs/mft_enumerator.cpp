#include "ntfs/mft_enumerator.h"

#include <windows.h>
#include <winioctl.h>

#include <chrono>
#include <vector>

#include "ntfs/volume.h"

namespace filo {
namespace {

// Transfer buffer. The bigger it is, the fewer calls into the driver are
// needed: 1 MB holds a few thousand records per round.
constexpr DWORD kBufferSize = 1 << 20;

// Starting estimates, so the memory is reserved in one shot instead of growing
// the vectors one reallocation at a time.
constexpr size_t kExpectedRecords = 1u << 21;     // ~2.1 million
constexpr size_t kExpectedNameChars = 64u << 20;  // ~64M characters

}  // namespace

MftEnumerator::MftEnumerator(wchar_t driveLetter) : drive_(driveLetter) {}

MftEnumerator::~MftEnumerator() {
    if (volume_) CloseHandle(static_cast<HANDLE>(volume_));
}

bool MftEnumerator::enumerate(FileIndex* index, std::wstring* error) {
    const auto start = std::chrono::steady_clock::now();

    if (!volume_) {
        volume_ = openVolume(drive_, error);
        if (!volume_) {
            *error += L". Restart as administrator to (re)build the index.";
            return false;
        }
    }
    HANDLE handle = static_cast<HANDLE>(volume_);

    // Read the journal position BEFORE the scan: everything that changes
    // during the scan lands in the journal and will be picked up on the next
    // start. Reading it afterwards would instead leave a hole.
    uint64_t journalId = 0;
    uint64_t nextUsn = 0;
    const bool haveJournal = queryJournal(handle, &journalId, &nextUsn);

    *index = FileIndex();
    index->setDrive(drive_);
    index->reserveRecords(kExpectedRecords, kExpectedNameChars);

    // Request parameters. StartFileReferenceNumber acts as the cursor: on each
    // round the driver tells us where to resume from.
    //
    // HighUsn = MAXLONGLONG also takes in the records with USN 0, that is the
    // files never modified since the journal has existed. Without this, a disk
    // whose journal was just created would come out almost empty.
    MFT_ENUM_DATA_V0 request{};
    request.StartFileReferenceNumber = 0;
    request.LowUsn = 0;
    request.HighUsn = MAXLONGLONG;

    std::vector<char> buffer(kBufferSize);

    for (;;) {
        DWORD bytesReturned = 0;
        const BOOL ok = DeviceIoControl(
            handle,
            FSCTL_ENUM_USN_DATA,
            &request, sizeof(request),
            buffer.data(), static_cast<DWORD>(buffer.size()),
            &bytesReturned,
            nullptr);

        if (!ok) {
            const DWORD code = GetLastError();
            if (code == ERROR_HANDLE_EOF) break;  // end of the MFT: normal exit
            *error = L"FSCTL_ENUM_USN_DATA failed: " + formatWinError(code);
            return false;
        }

        // The driver returns: [8 bytes: next FRN][record][record]...
        if (bytesReturned <= sizeof(USN)) break;
        stats_.bytesRead += bytesReturned;

        request.StartFileReferenceNumber =
            *reinterpret_cast<const DWORDLONG*>(buffer.data());

        const char* cursor = buffer.data() + sizeof(USN);
        const char* const end = buffer.data() + bytesReturned;

        while (cursor + sizeof(USN_RECORD_V2) <= end) {
            const auto* record = reinterpret_cast<const USN_RECORD_V2*>(cursor);
            if (record->RecordLength == 0) break;  // endless-loop guard

            // On NTFS the records are V2. ReFS uses V3, with a 128-bit FRN:
            // we skip it rather than read it wrong.
            if (record->MajorVersion == 2) {
                index->append(
                    record->FileReferenceNumber,
                    record->ParentFileReferenceNumber,
                    record->FileAttributes,
                    reinterpret_cast<const wchar_t*>(cursor + record->FileNameOffset),
                    record->FileNameLength / sizeof(wchar_t));
                ++stats_.recordCount;
            }

            cursor += record->RecordLength;
        }
    }

    index->finalize();
    if (haveJournal) index->setJournalPosition(journalId, nextUsn);

    stats_.elapsedSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return true;
}

}  // namespace filo
