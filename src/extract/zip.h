#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace filo {

struct ZipEntry {
    std::string name;
    uint64_t compressedSize = 0;
    uint64_t uncompressedSize = 0;
    uint64_t localHeaderOffset = 0;
    uint16_t method = 0;   // 0 = stored, 8 = deflate
};

// Read-only ZIP reader, written for HOSTILE input.
//
// DOCX, XLSX, PPTX, ODT and EPUB are all ZIP archives. We do not use a library
// because the available ones drag in compression, encryption and writing —
// things we will never use — and because all that is needed here is reading a
// few files by name, with strict checks.
//
// Every field coming out of the archive is treated as a CLAIM, not a fact: the
// declared size can lie, offsets can point outside, and a 50 KB file can
// promise 4 GB of content.
class ZipReader {
public:
    bool open(const char* data, size_t size);

    const std::vector<ZipEntry>& entries() const { return entries_; }
    const ZipEntry* find(const std::string& name) const;

    // Extracts one entry. `out` is overwritten. Returns false if the entry is
    // corrupt, past the limits, or compressed with a method we do not handle.
    bool extract(const ZipEntry& entry, std::string* out) const;
    bool extractByName(const std::string& name, std::string* out) const;

private:
    bool readCentralDirectory(uint64_t offset, uint64_t count);

    const char* data_ = nullptr;
    size_t size_ = 0;
    std::vector<ZipEntry> entries_;
};

}  // namespace filo
