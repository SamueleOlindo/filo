#include "extract/zip.h"

#include <libdeflate.h>

#include <algorithm>
#include <cstring>

namespace filo {
namespace {

constexpr uint32_t kSigLocal        = 0x04034b50;
constexpr uint32_t kSigCentral      = 0x02014b50;
constexpr uint32_t kSigEocd         = 0x06054b50;
constexpr uint32_t kSigZip64Eocd    = 0x06064b50;
constexpr uint32_t kSigZip64Locator = 0x07064b50;

// Defenses against "zip bombs": a 50 KB archive can legally declare 4 GB of
// content. On an 8 GB laptop one such file would be enough to kill the
// application, and the checks must fire BEFORE anything is allocated.
constexpr uint64_t kMaxPartBytes = 64ull << 20;
constexpr uint64_t kMaxRatio = 200;
constexpr size_t kMaxEntries = 20000;

uint16_t readU16(const char* p) {
    const auto* b = reinterpret_cast<const unsigned char*>(p);
    return static_cast<uint16_t>(b[0] | (b[1] << 8));
}
uint32_t readU32(const char* p) {
    const auto* b = reinterpret_cast<const unsigned char*>(p);
    return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
           (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
}
uint64_t readU64(const char* p) {
    return static_cast<uint64_t>(readU32(p)) | (static_cast<uint64_t>(readU32(p + 4)) << 32);
}

// The ZIP64 extra field (id 0x0001) holds the 64-bit values, but ONLY for the
// fields that read 0xFFFFFFFF in the main record. The spec says so in plain
// words, and getting it wrong means reading the next field and taking it for
// the right one.
void applyZip64Extra(const char* extra, size_t extraLength, ZipEntry* entry,
                     bool needUncompressed, bool needCompressed, bool needOffset) {
    size_t at = 0;
    while (at + 4 <= extraLength) {
        const uint16_t id = readU16(extra + at);
        const uint16_t length = readU16(extra + at + 2);
        if (at + 4 + length > extraLength) return;

        if (id == 0x0001) {
            const char* field = extra + at + 4;
            size_t remaining = length;
            if (needUncompressed && remaining >= 8) {
                entry->uncompressedSize = readU64(field);
                field += 8;
                remaining -= 8;
            }
            if (needCompressed && remaining >= 8) {
                entry->compressedSize = readU64(field);
                field += 8;
                remaining -= 8;
            }
            if (needOffset && remaining >= 8) {
                entry->localHeaderOffset = readU64(field);
            }
            return;
        }
        at += 4 + length;
    }
}

}  // namespace

bool ZipReader::open(const char* data, size_t size) {
    data_ = data;
    size_ = size;
    entries_.clear();
    if (!data || size < 22) return false;

    // The end record sits at the bottom, but a comment of up to 64 KB can
    // follow it: it has to be searched for backwards.
    const size_t searchSpan = std::min<size_t>(size, 66000);
    size_t eocd = std::string::npos;
    for (size_t back = 22; back <= searchSpan; ++back) {
        const size_t at = size - back;
        if (readU32(data + at) == kSigEocd) { eocd = at; break; }
    }
    if (eocd == std::string::npos) return false;

    uint64_t entryCount = readU16(data + eocd + 10);
    uint64_t centralOffset = readU32(data + eocd + 16);

    // If the fields are saturated there is a ZIP64 version of the record: the
    // locator sits immediately before the end record.
    if (entryCount == 0xFFFF || centralOffset == 0xFFFFFFFF) {
        if (eocd >= 20 && readU32(data + eocd - 20) == kSigZip64Locator) {
            const uint64_t zip64At = readU64(data + eocd - 20 + 8);
            if (zip64At + 56 <= size && readU32(data + zip64At) == kSigZip64Eocd) {
                entryCount = readU64(data + zip64At + 32);
                centralOffset = readU64(data + zip64At + 48);
            }
        }
    }

    if (centralOffset >= size || entryCount == 0) return false;
    return readCentralDirectory(centralOffset, entryCount);
}

bool ZipReader::readCentralDirectory(uint64_t offset, uint64_t count) {
    entries_.reserve(static_cast<size_t>(std::min<uint64_t>(count, 1024)));

    size_t at = static_cast<size_t>(offset);
    for (uint64_t i = 0; i < count && entries_.size() < kMaxEntries; ++i) {
        if (at + 46 > size_ || readU32(data_ + at) != kSigCentral) break;

        ZipEntry entry;
        entry.method = readU16(data_ + at + 10);
        entry.compressedSize = readU32(data_ + at + 20);
        entry.uncompressedSize = readU32(data_ + at + 24);

        const uint16_t nameLength = readU16(data_ + at + 28);
        const uint16_t extraLength = readU16(data_ + at + 30);
        const uint16_t commentLength = readU16(data_ + at + 32);
        entry.localHeaderOffset = readU32(data_ + at + 42);

        const size_t nameAt = at + 46;
        if (nameAt + nameLength + extraLength + commentLength > size_) break;

        entry.name.assign(data_ + nameAt, nameLength);
        applyZip64Extra(data_ + nameAt + nameLength, extraLength, &entry,
                        entry.uncompressedSize == 0xFFFFFFFF,
                        entry.compressedSize == 0xFFFFFFFF,
                        entry.localHeaderOffset == 0xFFFFFFFF);

        if (entry.localHeaderOffset < size_ && !entry.name.empty() &&
            entry.name.back() != '/') {
            entries_.push_back(std::move(entry));
        }
        at = nameAt + nameLength + extraLength + commentLength;
    }
    return !entries_.empty();
}

const ZipEntry* ZipReader::find(const std::string& name) const {
    for (const ZipEntry& entry : entries_) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

bool ZipReader::extract(const ZipEntry& entry, std::string* out) const {
    out->clear();
    if (entry.method != 0 && entry.method != 8) return false;   // stored and deflate only

    // The limits are checked BEFORE any memory is reserved: that is the point.
    if (entry.uncompressedSize > kMaxPartBytes) return false;
    if (entry.compressedSize > 0 &&
        entry.uncompressedSize / entry.compressedSize > kMaxRatio &&
        entry.uncompressedSize > (1ull << 20)) {
        return false;
    }

    const size_t local = static_cast<size_t>(entry.localHeaderOffset);
    if (local + 30 > size_ || readU32(data_ + local) != kSigLocal) return false;

    // Name and extra in the local header can have lengths DIFFERENT from the
    // ones in the central directory: they must be re-read here, not reused.
    const uint16_t localNameLength = readU16(data_ + local + 26);
    const uint16_t localExtraLength = readU16(data_ + local + 28);
    const size_t payload = local + 30 + localNameLength + localExtraLength;
    if (payload > size_) return false;

    const size_t available = size_ - payload;
    const size_t compressed = static_cast<size_t>(
        std::min<uint64_t>(entry.compressedSize, available));

    if (entry.method == 0) {
        const size_t length = static_cast<size_t>(
            std::min<uint64_t>(entry.uncompressedSize ? entry.uncompressedSize : compressed,
                               available));
        out->assign(data_ + payload, length);
        return true;
    }

    if (compressed == 0) return false;

    libdeflate_decompressor* decompressor = libdeflate_alloc_decompressor();
    if (!decompressor) return false;

    // The declared size is a HINT for sizing the buffer, not a requirement:
    // pass actual_out_nbytes_ret and an upper bound is enough, and libdeflate
    // reports how much it really wrote. If the buffer is too small it says so,
    // and we can retry bigger.
    size_t capacity = entry.uncompressedSize > 0
                          ? static_cast<size_t>(entry.uncompressedSize)
                          : std::min<size_t>(compressed * 8 + 4096, kMaxPartBytes);

    bool ok = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (capacity > kMaxPartBytes) capacity = kMaxPartBytes;
        out->resize(capacity);
        size_t produced = 0;
        const libdeflate_result result = libdeflate_deflate_decompress(
            decompressor, data_ + payload, compressed, out->data(), capacity, &produced);

        if (result == LIBDEFLATE_SUCCESS) {
            out->resize(produced);
            ok = true;
            break;
        }
        if (result != LIBDEFLATE_INSUFFICIENT_SPACE || capacity >= kMaxPartBytes) break;
        capacity *= 4;
    }

    libdeflate_free_decompressor(decompressor);
    if (!ok) out->clear();
    return ok;
}

bool ZipReader::extractByName(const std::string& name, std::string* out) const {
    const ZipEntry* entry = find(name);
    return entry && extract(*entry, out);
}

}  // namespace filo
