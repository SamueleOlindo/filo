#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace filo {

inline constexpr uint32_t kNoParent = 0xFFFFFFFFu;

// The top 16 bits of a File Reference Number are the "sequence number", which
// NTFS increments when it reuses a freed entry. The low 48 bits are the record
// number in the MFT, and that is what gets enumerated in ascending order.
inline constexpr uint64_t kMftRecordMask = 0x0000FFFFFFFFFFFFull;

// Tombstone. Windows uses the low bits of the FILE_ATTRIBUTE_* mask; bit 31 is
// free, and we take it to mark dead records without spending extra memory or
// touching the hot search loop.
inline constexpr uint32_t kDeletedAttribute = 0x80000000u;

// The file index, laid out as a structure of arrays.
//
// The intuitive shape would be a vector of structs, one element per file. But
// search reads ONLY the names: with a vector of structs the CPU would drag FRN,
// parent and attributes into cache for every record, wasting most of the memory
// bandwidth on data it never even looks at.
//
// Here every field lives in its own contiguous array. The search loop touches
// two arrays, both read perfectly sequentially, and the names live in a single
// pool: no std::wstring objects, no per-file allocation.
class FileIndex {
public:
    FileIndex();

    void setDrive(wchar_t drive) { drive_ = drive; }
    wchar_t drive() const { return drive_; }

    size_t size() const { return frn_.size(); }
    bool empty() const { return frn_.empty(); }
    size_t deletedCount() const { return deletedCount_; }

    // --- building from a full scan -------------------------------------------
    void reserveRecords(size_t records, size_t nameChars);
    void append(uint64_t frn, uint64_t parentFrn, uint32_t attributes,
                const wchar_t* name, size_t nameLength);
    // Translates parents from FRN to index and frees the temporary structures.
    void finalize();

    // --- incremental update --------------------------------------------------
    // Adds a record to an index that is already built. New arrivals land at the
    // end, hence outside the sorted region: they are found through a small side
    // map rather than through the binary search.
    uint32_t appendLive(uint64_t frn, uint64_t parentFrn, uint32_t attributes,
                        const wchar_t* name, size_t nameLength);
    // Replaces the name, parent and attributes of an existing record IN PLACE.
    //
    // This is what a rename or a move produces, and the slot has to survive it.
    // The old path buried the record and appended a new one, which is correct
    // for a file and quietly wrong for a FOLDER: every child holds the index of
    // its parent, those indices kept pointing at the tombstone, and fullPath()
    // walked them — so renaming a folder left every file underneath reporting
    // its old path until the next full rescan.
    //
    // The new name is appended to the pool rather than written over the old
    // one, because it rarely has the same length. That leaks the old bytes
    // until compact() runs, which is the price of keeping slots stable.
    void updateEntry(uint32_t index, uint64_t parentFrn, uint32_t attributes,
                     const wchar_t* name, size_t nameLength);

    void markDeleted(uint32_t index);
    // Rebuilds the FRN lookup structure after a burst of changes. Must be
    // called before save().
    void rebuildFrnOrder();
    // Actually removes the tombstones and compacts the arrays. Tombstones are
    // not wrong, but as they pile up they waste memory and lengthen the chains
    // of records sharing one MFT slot.
    void compact();

    // --- reading -------------------------------------------------------------
    std::wstring_view name(size_t i) const {
        return {namePool_.data() + nameOffset_[i], nameLength_[i]};
    }
    uint32_t parent(size_t i) const { return parentIndex_[i]; }
    uint32_t attributes(size_t i) const { return attributes_[i] & ~kDeletedAttribute; }
    uint64_t frn(size_t i) const { return frn_[i]; }
    bool isDeleted(size_t i) const { return (attributes_[i] & kDeletedAttribute) != 0; }

    std::wstring fullPath(size_t i) const;

    // Raw access to the arrays: the hot search loop uses them directly, without
    // going through function calls.
    const wchar_t* namePool() const { return namePool_.data(); }
    const uint32_t* nameOffsets() const { return nameOffset_.data(); }
    const uint16_t* nameLengths() const { return nameLength_.data(); }
    const uint32_t* attributesData() const { return attributes_.data(); }

    // Index of the record with that FRN, or kNoParent.
    uint32_t indexOfFrn(uint64_t frn) const;

    // --- persistence ---------------------------------------------------------
    // Journal position at the time of the snapshot, for the incremental
    // catch-up on the next start.
    void setJournalPosition(uint64_t journalId, uint64_t nextUsn);
    uint64_t journalId() const { return journalId_; }
    uint64_t nextUsn() const { return nextUsn_; }

    bool save(const std::wstring& path, std::wstring* error) const;
    bool load(const std::wstring& path, std::wstring* error);

    size_t memoryBytes() const;

private:
    std::vector<wchar_t>  namePool_;     // every name, concatenated
    // Start and length per record, rather than shared boundaries. Boundaries
    // are two bytes smaller per record but they chain every name to its
    // neighbours: changing one means moving all the rest. NTFS caps a name at
    // 255 characters, so the length fits a uint16 with room to spare.
    std::vector<uint32_t> nameOffset_;
    std::vector<uint16_t> nameLength_;
    std::vector<uint64_t> frn_;
    std::vector<uint32_t> parentIndex_;
    std::vector<uint32_t> attributes_;

    // Lives only between append() and finalize(): parents arrive as FRNs and
    // have to become indices, but that conversion needs the complete index.
    std::vector<uint64_t> parentFrnTemp_;

    // Fallback for an MFT that does not arrive sorted: a permutation ordered by
    // record number. Under normal conditions it stays empty.
    std::vector<uint32_t> frnOrder_;

    // The first sortedCount_ records are covered by the binary search. Those
    // past it arrived from the journal after the last reorder and live here.
    size_t sortedCount_ = 0;
    std::unordered_map<uint64_t, uint32_t> deltaFrnToIndex_;

    size_t   deletedCount_ = 0;
    uint32_t rootIndex_ = kNoParent;
    uint64_t journalId_ = 0;
    uint64_t nextUsn_ = 0;
    wchar_t  drive_ = L'C';
};

}  // namespace filo
