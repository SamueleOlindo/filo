#pragma once

#include <cstdint>
#include <string>

namespace filo {

// Readable system message for a Win32 error code.
std::wstring formatWinError(unsigned long code);

// Opens "\\.\X:", that is the volume itself rather than a file inside it.
// Returns nullptr on failure, filling `error`.
// Requires elevation: that is a Windows security decision, not something we can
// work around.
void* openVolume(wchar_t drive, std::wstring* error);

// Current position of the NTFS change journal. If the journal does not exist
// yet it is created, so that incremental updates become possible from the next
// start onwards.
bool queryJournal(void* volume, uint64_t* journalId, uint64_t* nextUsn);

// Volume serial number. It is the first half of the document key: (volume, FRN)
// identifies a file even when several disks are indexed, and survives the drive
// letter, which can change.
uint32_t volumeSerial(wchar_t drive);

}  // namespace filo
