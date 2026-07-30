#include "ntfs/usn_reader.h"

#include <windows.h>
#include <winioctl.h>

#include <chrono>
#include <string_view>
#include <vector>

#include "ntfs/volume.h"

namespace filo {
namespace {

constexpr DWORD kBufferSize = 1 << 20;

// A journal entry describes a file by FRN, name and parent folder. Applying it
// means bringing the index into agreement with that description.
struct JournalEntry {
    uint64_t frn;
    uint64_t parentFrn;
    uint32_t attributes;
    const wchar_t* name;
    size_t nameLength;
};

// Does the record already exist and describe exactly the same thing?
bool matchesExisting(const FileIndex& index, uint32_t slot, const JournalEntry& entry) {
    if (index.frn(slot) != entry.frn) return false;  // MFT slot reused by another file
    if (index.name(slot) != std::wstring_view(entry.name, entry.nameLength)) return false;
    return index.parent(slot) == index.indexOfFrn(entry.parentFrn);
}

}  // namespace

UsnReader::UsnReader(wchar_t driveLetter) : drive_(driveLetter) {}

UsnReader::~UsnReader() {
    if (volume_) CloseHandle(static_cast<HANDLE>(volume_));
}

bool UsnReader::catchUp(FileIndex* index, CatchupStats* stats, std::wstring* error) {
    const auto start = std::chrono::steady_clock::now();

    if (index->journalId() == 0) {
        // The snapshot was created without a journal: there is no resume point,
        // so a full scan is required.
        stats->needsFullScan = true;
        return true;
    }

    if (!volume_) {
        volume_ = openVolume(drive_, error);
        if (!volume_) return false;
    }
    HANDLE handle = static_cast<HANDLE>(volume_);

    uint64_t journalId = 0;
    uint64_t currentNextUsn = 0;
    if (!queryJournal(handle, &journalId, &currentNextUsn)) {
        *error = L"cannot query the journal: " + formatWinError(GetLastError());
        return false;
    }

    // If the identifier changed, the journal was destroyed and recreated
    // (a format, a disable, a chkdsk): the old numbers mean nothing any more,
    // and resuming from them would produce an inconsistent index.
    if (journalId != index->journalId()) {
        stats->needsFullScan = true;
        return true;
    }

    READ_USN_JOURNAL_DATA_V0 request{};
    request.StartUsn = static_cast<USN>(index->nextUsn());
    request.ReasonMask = 0xFFFFFFFF;
    request.ReturnOnlyOnClose = FALSE;
    request.Timeout = 0;
    request.BytesToWaitFor = 0;   // do not block: read what is there and return
    request.UsnJournalID = journalId;

    std::vector<char> buffer(kBufferSize);

    for (;;) {
        DWORD returned = 0;
        if (!DeviceIoControl(handle, FSCTL_READ_USN_JOURNAL, &request, sizeof(request),
                             buffer.data(), static_cast<DWORD>(buffer.size()),
                             &returned, nullptr)) {
            const DWORD code = GetLastError();
            if (code == ERROR_HANDLE_EOF) break;
            if (code == ERROR_JOURNAL_ENTRY_DELETED) {
                // The journal has a size ceiling and discards its oldest
                // entries: if our resume point fell off the end, the delta
                // cannot be reconstructed.
                stats->needsFullScan = true;
                return true;
            }
            *error = L"reading the journal failed: " + formatWinError(code);
            return false;
        }

        if (returned < sizeof(USN)) break;

        // Same as for the enumeration: the first 8 bytes are the resume point.
        const USN nextUsn = *reinterpret_cast<const USN*>(buffer.data());
        if (returned == sizeof(USN)) {
            request.StartUsn = nextUsn;  // no entries: we are caught up
            break;
        }

        const char* cursor = buffer.data() + sizeof(USN);
        const char* const end = buffer.data() + returned;

        while (cursor + sizeof(USN_RECORD_V2) <= end) {
            const auto* record = reinterpret_cast<const USN_RECORD_V2*>(cursor);
            if (record->RecordLength == 0) break;
            if (record->MajorVersion != 2) {
                cursor += record->RecordLength;
                continue;
            }

            ++stats->recordsRead;

            const JournalEntry entry{
                record->FileReferenceNumber,
                record->ParentFileReferenceNumber,
                record->FileAttributes,
                reinterpret_cast<const wchar_t*>(cursor + record->FileNameOffset),
                record->FileNameLength / sizeof(wchar_t)};

            const uint32_t slot = index->indexOfFrn(entry.frn);

            if (record->Reason & USN_REASON_FILE_DELETE) {
                if (slot != kNoParent && !index->isDeleted(slot)) {
                    index->markDeleted(slot);
                    ++stats->deleted;
                }
            } else if (slot == kNoParent || index->isDeleted(slot)) {
                // Never seen, or buried and now resurrected: a new entry.
                index->appendLive(entry.frn, entry.parentFrn, entry.attributes,
                                  entry.name, entry.nameLength);
                ++stats->created;
            } else if (!matchesExisting(*index, slot, entry)) {
                // Renamed or moved. Updated IN PLACE, keeping the slot.
                //
                // This used to bury the old record and append a new one, which
                // is harmless for a file and wrong for a FOLDER: children hold
                // the index of their parent, and those indices went on pointing
                // at the tombstone with the old name. Renaming a folder left
                // every file beneath it reporting the old path — and reporting
                // it confidently — until the next full rescan.
                index->updateEntry(slot, entry.parentFrn, entry.attributes, entry.name,
                                   entry.nameLength);
                ++stats->updated;
            }

            cursor += record->RecordLength;
        }

        request.StartUsn = nextUsn;
    }

    index->setJournalPosition(journalId, static_cast<uint64_t>(request.StartUsn));

    stats->elapsedSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return true;
}

}  // namespace filo
