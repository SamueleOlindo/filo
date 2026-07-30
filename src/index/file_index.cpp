#include "index/file_index.h"

#include <windows.h>

#include <algorithm>
#include <numeric>

namespace filo {
namespace {

// Version 3 stores a length per record instead of shared name boundaries, so a
// record can be renamed without moving its neighbours.
constexpr char kMagic[8] = {'F', 'I', 'L', 'O', 'I', 'D', 'X', '3'};
constexpr uint32_t kVersion = 3;

#pragma pack(push, 1)
struct SnapshotHeader {
    char     magic[8];
    uint32_t version;
    uint32_t drive;
    uint64_t recordCount;
    uint64_t nameCharCount;
    uint64_t sortedCount;
    uint64_t deletedCount;
    uint32_t rootIndex;
    uint32_t orderCount;   // != 0 only in the rare case of an unordered MFT
    uint64_t journalId;
    uint64_t nextUsn;
};
#pragma pack(pop)

std::wstring winError(const wchar_t* what, DWORD code) {
    return std::wstring(what) + L" (error " + std::to_wstring(code) + L")";
}

// ReadFile/WriteFile may serve fewer bytes than asked for: loop until the block
// is complete.
bool writeExact(HANDLE file, const void* data, size_t bytes) {
    const char* cursor = static_cast<const char*>(data);
    while (bytes > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes, 1u << 30));
        DWORD written = 0;
        if (!WriteFile(file, cursor, chunk, &written, nullptr) || written == 0) return false;
        cursor += written;
        bytes -= written;
    }
    return true;
}

bool readExact(HANDLE file, void* data, size_t bytes) {
    char* cursor = static_cast<char*>(data);
    while (bytes > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes, 1u << 30));
        DWORD read = 0;
        if (!ReadFile(file, cursor, chunk, &read, nullptr) || read == 0) return false;
        cursor += read;
        bytes -= read;
    }
    return true;
}

template <typename T>
bool writeVector(HANDLE file, const std::vector<T>& data) {
    return data.empty() || writeExact(file, data.data(), data.size() * sizeof(T));
}

template <typename T>
bool readVector(HANDLE file, std::vector<T>& data, size_t count) {
    data.resize(count);
    return count == 0 || readExact(file, data.data(), count * sizeof(T));
}

}  // namespace

namespace {

// NTFS stops at 255 characters. Clamping instead of trusting the caller keeps
// a malformed journal record from writing a length the name array cannot hold.
constexpr size_t kMaxNameChars = 255;

}  // namespace

FileIndex::FileIndex() = default;

void FileIndex::reserveRecords(size_t records, size_t nameChars) {
    frn_.reserve(records);
    parentIndex_.reserve(records);
    attributes_.reserve(records);
    parentFrnTemp_.reserve(records);
    nameOffset_.reserve(records);
    nameLength_.reserve(records);
    namePool_.reserve(nameChars);
}

void FileIndex::append(uint64_t frn, uint64_t parentFrn, uint32_t attributes,
                       const wchar_t* name, size_t nameLength) {
    nameLength = std::min(nameLength, kMaxNameChars);
    nameOffset_.push_back(static_cast<uint32_t>(namePool_.size()));
    nameLength_.push_back(static_cast<uint16_t>(nameLength));
    namePool_.insert(namePool_.end(), name, name + nameLength);

    frn_.push_back(frn);
    parentFrnTemp_.push_back(parentFrn);
    attributes_.push_back(attributes & ~kDeletedAttribute);
    parentIndex_.push_back(kNoParent);
}

void FileIndex::rebuildFrnOrder() {
    const size_t count = frn_.size();

    // MFT enumeration advances by ascending record number, so the low 48 bits
    // of the FRNs normally arrive already sorted and can be binary-searched
    // without building any table. We verify that assumption rather than trust
    // it: after an incremental update it nearly always stops holding.
    bool sorted = true;
    for (size_t i = 1; i < count; ++i) {
        if ((frn_[i] & kMftRecordMask) <= (frn_[i - 1] & kMftRecordMask)) {
            sorted = false;
            break;
        }
    }

    if (sorted) {
        frnOrder_.clear();
    } else {
        frnOrder_.resize(count);
        std::iota(frnOrder_.begin(), frnOrder_.end(), 0u);
        std::sort(frnOrder_.begin(), frnOrder_.end(), [this](uint32_t a, uint32_t b) {
            return (frn_[a] & kMftRecordMask) < (frn_[b] & kMftRecordMask);
        });
    }

    sortedCount_ = count;
    deltaFrnToIndex_.clear();
}

void FileIndex::finalize() {
    rebuildFrnOrder();

    const size_t count = frn_.size();
    for (size_t i = 0; i < count; ++i) {
        const uint64_t parentFrn = parentFrnTemp_[i];
        if (parentFrn == frn_[i]) {
            rootIndex_ = static_cast<uint32_t>(i);  // the root is its own parent
            parentIndex_[i] = kNoParent;
            continue;
        }
        parentIndex_[i] = indexOfFrn(parentFrn);
    }

    // The parent FRNs are no longer needed: that information now lives in the
    // indices.
    std::vector<uint64_t>().swap(parentFrnTemp_);
}

uint32_t FileIndex::appendLive(uint64_t frn, uint64_t parentFrn, uint32_t attributes,
                               const wchar_t* name, size_t nameLength) {
    // The parent has to be resolved BEFORE touching the arrays, or a search
    // could cross the record halfway through its insertion.
    const uint32_t parentIdx = (parentFrn == frn) ? kNoParent : indexOfFrn(parentFrn);
    const uint32_t index = static_cast<uint32_t>(frn_.size());

    nameLength = std::min(nameLength, kMaxNameChars);
    nameOffset_.push_back(static_cast<uint32_t>(namePool_.size()));
    nameLength_.push_back(static_cast<uint16_t>(nameLength));
    namePool_.insert(namePool_.end(), name, name + nameLength);
    frn_.push_back(frn);
    attributes_.push_back(attributes & ~kDeletedAttribute);
    parentIndex_.push_back(parentIdx);

    deltaFrnToIndex_[frn] = index;
    return index;
}

void FileIndex::updateEntry(uint32_t index, uint64_t parentFrn, uint32_t attributes,
                            const wchar_t* name, size_t nameLength) {
    if (index >= frn_.size()) return;

    // Resolve the parent before touching anything: a search running on another
    // thread must never see the record halfway through the update.
    const uint32_t parentIdx =
        (parentFrn == frn_[index]) ? kNoParent : indexOfFrn(parentFrn);

    nameLength = std::min(nameLength, kMaxNameChars);
    const auto at = static_cast<uint32_t>(namePool_.size());
    namePool_.insert(namePool_.end(), name, name + nameLength);

    // Offset first, then length. If a reader catches the pair mid-write it sees
    // the new offset with the old length, which is a short or long read inside
    // a buffer that already holds the new name — not a read past its end.
    nameOffset_[index] = at;
    nameLength_[index] = static_cast<uint16_t>(nameLength);
    attributes_[index] = attributes & ~kDeletedAttribute;
    parentIndex_[index] = parentIdx;
}

void FileIndex::markDeleted(uint32_t index) {
    if (index >= attributes_.size()) return;
    if (attributes_[index] & kDeletedAttribute) return;
    attributes_[index] |= kDeletedAttribute;
    ++deletedCount_;
}

uint32_t FileIndex::indexOfFrn(uint64_t frn) const {
    const uint64_t target = frn & kMftRecordMask;

    // Journal arrivals first: there are few of them and they sit in a map, keyed
    // on the FULL FRN.
    if (!deltaFrnToIndex_.empty()) {
        const auto it = deltaFrnToIndex_.find(frn);
        if (it != deltaFrnToIndex_.end()) return it->second;
    }

    // In the sorted region the binary search finds the first MFT slot with that
    // number. Sorting by record number is the only possible choice (it is the
    // order the MFT is enumerated in), but that number is NOT an identity: NTFS
    // reassigns it to a different file when the slot frees up, incrementing the
    // sequence number in the top 16 bits.
    size_t position = 0;
    const size_t limit = frnOrder_.empty() ? sortedCount_ : frnOrder_.size();

    if (frnOrder_.empty()) {
        const auto begin = frn_.begin();
        const auto it = std::lower_bound(begin, begin + static_cast<ptrdiff_t>(sortedCount_),
                                         target, [](uint64_t value, uint64_t needle) {
                                             return (value & kMftRecordMask) < needle;
                                         });
        position = static_cast<size_t>(it - begin);
    } else {
        const auto it = std::lower_bound(frnOrder_.begin(), frnOrder_.end(), target,
                                         [this](uint32_t slot, uint64_t needle) {
                                             return (frn_[slot] & kMftRecordMask) < needle;
                                         });
        position = static_cast<size_t>(it - frnOrder_.begin());
    }

    // Walk the handful of records sharing that slot and tell them apart on the
    // full FRN. They can coexist because a deleted file leaves its tombstone
    // where another file has since moved in.
    uint32_t buried = kNoParent;
    for (size_t k = position; k < limit; ++k) {
        const uint32_t slot = frnOrder_.empty() ? static_cast<uint32_t>(k) : frnOrder_[k];
        if ((frn_[slot] & kMftRecordMask) != target) break;
        if (frn_[slot] != frn) continue;      // same slot, different file
        if (!isDeleted(slot)) return slot;    // at equal FRN, the live one wins
        buried = slot;
    }
    return buried;
}

void FileIndex::compact() {
    if (deletedCount_ == 0) return;

    const size_t count = frn_.size();
    const size_t survivors = count - deletedCount_;

    std::vector<uint32_t> remap(count, kNoParent);

    std::vector<wchar_t>  newPool;
    std::vector<uint32_t> newOffset;
    std::vector<uint16_t> newLength;
    std::vector<uint64_t> newFrn;
    std::vector<uint32_t> newParent;
    std::vector<uint32_t> newAttributes;

    newPool.reserve(namePool_.size());
    newOffset.reserve(survivors);
    newLength.reserve(survivors);
    newFrn.reserve(survivors);
    newParent.reserve(survivors);
    newAttributes.reserve(survivors);

    for (size_t i = 0; i < count; ++i) {
        if (isDeleted(i)) continue;
        remap[i] = static_cast<uint32_t>(newFrn.size());

        // Also reclaims the pool space left behind by renames: the old name
        // bytes have no record pointing at them any more, so they simply do
        // not get copied.
        const std::wstring_view entryName = name(i);
        newOffset.push_back(static_cast<uint32_t>(newPool.size()));
        newLength.push_back(static_cast<uint16_t>(entryName.size()));
        newPool.insert(newPool.end(), entryName.begin(), entryName.end());
        newFrn.push_back(frn_[i]);
        newParent.push_back(parentIndex_[i]);  // still old indices
        newAttributes.push_back(attributes_[i]);
    }

    // The parents pointed at the old positions and have to be translated. A
    // buried parent (deleted folder) becomes "no parent".
    for (uint32_t& parent : newParent) {
        parent = (parent == kNoParent) ? kNoParent : remap[parent];
    }
    rootIndex_ = (rootIndex_ != kNoParent && rootIndex_ < count) ? remap[rootIndex_]
                                                                : kNoParent;

    namePool_.swap(newPool);
    nameOffset_.swap(newOffset);
    nameLength_.swap(newLength);
    frn_.swap(newFrn);
    parentIndex_.swap(newParent);
    attributes_.swap(newAttributes);

    deletedCount_ = 0;
    rebuildFrnOrder();
}

std::wstring FileIndex::fullPath(size_t i) const {
    if (i >= size()) return {};

    // Walk up parent by parent. This is pure array indexing: no hash map, no
    // hash computation, no unpredictable jumps.
    std::wstring_view parts[256];
    int depth = 0;

    uint32_t current = static_cast<uint32_t>(i);
    while (depth < 256) {
        if (current == rootIndex_) break;  // the root's name is "." : skip it
        parts[depth++] = name(current);

        const uint32_t parent = parentIndex_[current];
        if (parent == kNoParent) break;  // parent outside the index
        current = parent;
    }

    size_t total = 2;  // "C:"
    for (int d = 0; d < depth; ++d) total += parts[d].size() + 1;

    std::wstring path;
    path.reserve(total);
    path += drive_;
    path += L':';
    for (int d = depth - 1; d >= 0; --d) {
        path += L'\\';
        path.append(parts[d]);
    }
    return path;
}

void FileIndex::setJournalPosition(uint64_t journalId, uint64_t nextUsn) {
    journalId_ = journalId;
    nextUsn_ = nextUsn;
}

size_t FileIndex::memoryBytes() const {
    return namePool_.capacity() * sizeof(wchar_t) +
           nameOffset_.capacity() * sizeof(uint32_t) +
           nameLength_.capacity() * sizeof(uint16_t) +
           frn_.capacity() * sizeof(uint64_t) +
           parentIndex_.capacity() * sizeof(uint32_t) +
           attributes_.capacity() * sizeof(uint32_t) +
           frnOrder_.capacity() * sizeof(uint32_t) +
           deltaFrnToIndex_.size() * (sizeof(uint64_t) + sizeof(uint32_t) + 16);
}

bool FileIndex::save(const std::wstring& path, std::wstring* error) const {
    // Write to a temporary file and then replace: if the process dies mid-write
    // the previous snapshot stays valid, rather than becoming a truncated file
    // the next start would load.
    const std::wstring temporary = path + L".tmp";

    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *error = winError(L"cannot create the snapshot", GetLastError());
        return false;
    }

    SnapshotHeader header{};
    std::copy(std::begin(kMagic), std::end(kMagic), header.magic);
    header.version = kVersion;
    header.drive = static_cast<uint32_t>(drive_);
    header.recordCount = size();
    header.nameCharCount = namePool_.size();
    header.sortedCount = sortedCount_;
    header.deletedCount = deletedCount_;
    header.rootIndex = rootIndex_;
    header.orderCount = static_cast<uint32_t>(frnOrder_.size());
    header.journalId = journalId_;
    header.nextUsn = nextUsn_;

    const bool ok =
        writeExact(file, &header, sizeof(header)) &&
        writeVector(file, namePool_) &&
        writeVector(file, nameOffset_) &&
        writeVector(file, nameLength_) &&
        writeVector(file, frn_) &&
        writeVector(file, parentIndex_) &&
        writeVector(file, attributes_) &&
        writeVector(file, frnOrder_);

    CloseHandle(file);

    if (!ok) {
        DeleteFileW(temporary.c_str());
        *error = L"writing the snapshot failed";
        return false;
    }

    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        const DWORD code = GetLastError();
        DeleteFileW(temporary.c_str());
        *error = winError(L"cannot replace the snapshot", code);
        return false;
    }
    return true;
}

bool FileIndex::load(const std::wstring& path, std::wstring* error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *error = L"no snapshot present";
        return false;
    }

    SnapshotHeader header{};
    if (!readExact(file, &header, sizeof(header))) {
        CloseHandle(file);
        *error = L"snapshot truncated";
        return false;
    }

    // Only the trailing digit of the magic moves between versions, so the
    // prefix is what identifies the file as ours at all.
    const bool ours = std::equal(std::begin(kMagic), std::begin(kMagic) + 7, header.magic);
    if (!ours || (header.version != kVersion && header.version != 2)) {
        CloseHandle(file);
        *error = L"snapshot format not recognized";
        return false;
    }

    const size_t count = static_cast<size_t>(header.recordCount);
    bool ok = readVector(file, namePool_, static_cast<size_t>(header.nameCharCount));

    if (ok && header.version == 2) {
        // Version 2 stored count+1 shared boundaries instead of a length per
        // record. Deriving the lengths costs one pass and saves the user a full
        // rescan of the volume — which needs administrator rights, and would
        // otherwise turn "the program was updated" into "the program no longer
        // works until you find an elevated prompt".
        std::vector<uint32_t> boundaries;
        ok = readVector(file, boundaries, count + 1);
        if (ok) {
            nameOffset_.resize(count);
            nameLength_.resize(count);
            for (size_t i = 0; i < count; ++i) {
                nameOffset_[i] = boundaries[i];
                nameLength_[i] = static_cast<uint16_t>(
                    std::min<uint32_t>(boundaries[i + 1] - boundaries[i], kMaxNameChars));
            }
        }
    } else if (ok) {
        ok = readVector(file, nameOffset_, count) && readVector(file, nameLength_, count);
    }

    ok = ok && readVector(file, frn_, count) &&
         readVector(file, parentIndex_, count) &&
         readVector(file, attributes_, count) &&
         readVector(file, frnOrder_, header.orderCount);

    CloseHandle(file);

    if (!ok) {
        *error = L"snapshot truncated or corrupt";
        return false;
    }

    drive_ = static_cast<wchar_t>(header.drive);
    rootIndex_ = header.rootIndex;
    sortedCount_ = static_cast<size_t>(header.sortedCount);
    deletedCount_ = static_cast<size_t>(header.deletedCount);
    journalId_ = header.journalId;
    nextUsn_ = header.nextUsn;
    parentFrnTemp_.clear();
    deltaFrnToIndex_.clear();
    return true;
}

}  // namespace filo
