#include "vector/vector_store.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>

namespace filo {
namespace {

constexpr char kMagic[8] = {'F', 'I', 'L', 'O', 'V', 'E', 'C', '2'};

#pragma pack(push, 1)
struct VectorHeader {
    char     magic[8];
    uint32_t version;
    uint32_t dimensions;
    uint64_t count;
    uint64_t modelId;
    uint32_t chunkerVersion;
    uint32_t contentGeneration;
};
#pragma pack(pop)

// Records on disk are INTERLEAVED — id followed by its vector — while memory
// keeps them split. The split layout is what lets the scan run over one
// contiguous run of bytes; the interleaved one is what makes a checkpoint a
// plain append instead of a rewrite. Converting between them costs one pass at
// load and nothing at all during the run.
size_t recordBytes(uint32_t dimensions) { return sizeof(uint32_t) + dimensions; }

// Quantization factor: normalized vectors live in [-1, 1], and 127 is the
// largest value a signed byte can hold.
constexpr float kQuantScale = 127.0f;

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

}  // namespace

void normalizeVector(float* vector, uint32_t dimensions) {
    double sum = 0.0;
    for (uint32_t i = 0; i < dimensions; ++i) sum += static_cast<double>(vector[i]) * vector[i];
    if (sum <= 0.0) return;
    const float inverse = static_cast<float>(1.0 / std::sqrt(sum));
    for (uint32_t i = 0; i < dimensions; ++i) vector[i] *= inverse;
}

void VectorStore::configure(uint32_t dimensions) {
    dimensions_ = dimensions;
    clear();
}

void VectorStore::clear() {
    data_.clear();
    chunkIds_.clear();
    persisted_ = 0;
}

void VectorStore::reserve(size_t count) {
    chunkIds_.reserve(count);
    data_.reserve(count * dimensions_);
}

void VectorStore::add(uint32_t chunkId, const float* vector) {
    chunkIds_.push_back(chunkId);
    const size_t at = data_.size();
    data_.resize(at + dimensions_);
    for (uint32_t i = 0; i < dimensions_; ++i) {
        const float scaled = vector[i] * kQuantScale;
        const int rounded = static_cast<int>(scaled < 0 ? scaled - 0.5f : scaled + 0.5f);
        data_[at + i] = static_cast<int8_t>(std::clamp(rounded, -127, 127));
    }
}

void VectorStore::addExisting(uint32_t chunkId, uint32_t existingPosition) {
    if (static_cast<size_t>(existingPosition) >= chunkIds_.size()) return;
    const size_t source = static_cast<size_t>(existingPosition) * dimensions_;
    chunkIds_.push_back(chunkId);
    const size_t at = data_.size();
    data_.resize(at + dimensions_);
    std::memcpy(data_.data() + at, data_.data() + source, dimensions_);
}

size_t VectorStore::memoryBytes() const {
    return data_.capacity() + chunkIds_.capacity() * sizeof(uint32_t);
}

std::vector<VectorStore::Neighbor> VectorStore::search(const float* query, size_t topK,
                                                       unsigned threadCount) const {
    std::vector<Neighbor> results;
    const size_t count = chunkIds_.size();
    if (count == 0 || dimensions_ == 0 || topK == 0) return results;

    // The query gets quantized too, so the dot product stays integer-only and
    // the compiler can vectorize it.
    std::vector<int8_t> quantized(dimensions_);
    for (uint32_t i = 0; i < dimensions_; ++i) {
        const float scaled = query[i] * kQuantScale;
        const int rounded = static_cast<int>(scaled < 0 ? scaled - 0.5f : scaled + 0.5f);
        quantized[i] = static_cast<int8_t>(std::clamp(rounded, -127, 127));
    }

    if (threadCount == 0) threadCount = std::max(1u, std::thread::hardware_concurrency());
    threadCount = static_cast<unsigned>(std::min<size_t>(threadCount, (count + 4095) / 4096));
    threadCount = std::max(1u, threadCount);

    // Every thread keeps its own partial ranking: no shared writes in the hot
    // loop.
    std::vector<std::vector<Neighbor>> partial(threadCount);
    std::vector<std::thread> workers;
    workers.reserve(threadCount - 1);

    const size_t slice = (count + threadCount - 1) / threadCount;
    const int8_t* const base = data_.data();
    const int8_t* const needle = quantized.data();
    const uint32_t dims = dimensions_;

    auto scan = [&](unsigned slot) {
        const size_t begin = slot * slice;
        const size_t end = std::min(begin + slice, count);
        auto& best = partial[slot];
        best.reserve(topK + 1);

        float worst = -2.0f;
        for (size_t i = begin; i < end; ++i) {
            const int8_t* candidate = base + i * dims;
            // Integer dot product. The compiler vectorizes it on its own:
            // the loop is contiguous, the length is known, and there are no
            // branches in it.
            int32_t sum = 0;
            for (uint32_t d = 0; d < dims; ++d) {
                sum += static_cast<int32_t>(candidate[d]) * needle[d];
            }
            const float score = static_cast<float>(sum) / (kQuantScale * kQuantScale);

            if (best.size() < topK) {
                best.push_back({chunkIds_[i], score});
                if (best.size() == topK) {
                    std::sort(best.begin(), best.end(),
                              [](const Neighbor& a, const Neighbor& b) { return a.score > b.score; });
                    worst = best.back().score;
                }
            } else if (score > worst) {
                best.back() = {chunkIds_[i], score};
                // Insertion reorder: the ranking is nearly sorted already, so
                // this costs a few swaps instead of a full sort.
                for (size_t k = best.size() - 1; k > 0 && best[k].score > best[k - 1].score; --k) {
                    std::swap(best[k], best[k - 1]);
                }
                worst = best.back().score;
            }
        }
        if (best.size() < topK) {
            std::sort(best.begin(), best.end(),
                      [](const Neighbor& a, const Neighbor& b) { return a.score > b.score; });
        }
    };

    for (unsigned slot = 1; slot < threadCount; ++slot) workers.emplace_back(scan, slot);
    scan(0);
    for (std::thread& worker : workers) worker.join();

    for (const auto& slice2 : partial) results.insert(results.end(), slice2.begin(), slice2.end());
    std::sort(results.begin(), results.end(),
              [](const Neighbor& a, const Neighbor& b) { return a.score > b.score; });
    if (results.size() > topK) results.resize(topK);
    return results;
}

namespace {

VectorHeader makeHeader(const VectorStore::Identity& identity, size_t count) {
    VectorHeader header{};
    std::copy(std::begin(kMagic), std::end(kMagic), header.magic);
    header.version = 2;
    header.dimensions = identity.dimensions;
    header.count = count;
    header.modelId = identity.modelId;
    header.chunkerVersion = identity.chunkerVersion;
    header.contentGeneration = identity.contentGeneration;
    return header;
}

// Writes records [from, to) interleaved into an already-positioned handle.
bool writeRange(HANDLE file, const std::vector<uint32_t>& ids,
                const std::vector<int8_t>& data, uint32_t dimensions, size_t from,
                size_t to) {
    if (from >= to) return true;

    // One buffered pass instead of two WriteFile calls per record: at 190,000
    // records the syscall count is what would dominate, not the bytes.
    std::vector<char> buffer;
    buffer.reserve((to - from) * recordBytes(dimensions));
    for (size_t i = from; i < to; ++i) {
        const char* id = reinterpret_cast<const char*>(&ids[i]);
        buffer.insert(buffer.end(), id, id + sizeof(uint32_t));
        const char* vector = reinterpret_cast<const char*>(data.data() + i * dimensions);
        buffer.insert(buffer.end(), vector, vector + dimensions);
    }
    return writeExact(file, buffer.data(), buffer.size());
}

}  // namespace

bool VectorStore::save(const std::wstring& path, const Identity& identity,
                       std::wstring* error) const {
    const std::wstring temporary = path + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *error = L"cannot create the vector file";
        return false;
    }

    const VectorHeader header = makeHeader(identity, chunkIds_.size());
    const bool ok =
        writeExact(file, &header, sizeof(header)) &&
        writeRange(file, chunkIds_, data_, dimensions_, 0, chunkIds_.size());
    CloseHandle(file);

    if (!ok) {
        DeleteFileW(temporary.c_str());
        *error = L"writing the vector file failed";
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temporary.c_str());
        *error = L"replacing the vector file failed";
        return false;
    }
    return true;
}

bool VectorStore::flush(const std::wstring& path, const Identity& identity,
                        std::wstring* error) {
    if (persisted_ > chunkIds_.size()) persisted_ = 0;   // store was cleared

    // Nothing new since the last checkpoint. Still cheap to be sure the file
    // exists, but no reason to touch it.
    if (persisted_ == chunkIds_.size() && persisted_ > 0) return true;

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    // No file yet, or one we cannot append to safely: write the whole thing.
    // The append path below is an optimization, never a correctness
    // requirement, so every doubt resolves to the full write.
    bool appendable = file != INVALID_HANDLE_VALUE;
    VectorHeader header{};
    if (appendable) {
        appendable = readExact(file, &header, sizeof(header)) &&
                     std::equal(std::begin(kMagic), std::end(kMagic), header.magic) &&
                     header.version == 2 && header.dimensions == dimensions_ &&
                     header.modelId == identity.modelId &&
                     header.chunkerVersion == identity.chunkerVersion &&
                     header.contentGeneration == identity.contentGeneration &&
                     header.count == persisted_;
    }
    if (appendable) {
        // A file longer than its header claims means a previous append died
        // partway. Truncating back to the recorded count throws away the torn
        // tail rather than reading it back as a valid record later.
        LARGE_INTEGER end{};
        end.QuadPart = static_cast<LONGLONG>(sizeof(VectorHeader) +
                                             persisted_ * recordBytes(dimensions_));
        appendable = SetFilePointerEx(file, end, nullptr, FILE_BEGIN) &&
                     SetEndOfFile(file) &&
                     writeRange(file, chunkIds_, data_, dimensions_, persisted_,
                                chunkIds_.size());
        if (appendable) {
            header.count = chunkIds_.size();
            LARGE_INTEGER start{};
            appendable = SetFilePointerEx(file, start, nullptr, FILE_BEGIN) &&
                         writeExact(file, &header, sizeof(header));
        }
    }
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);

    if (!appendable && !save(path, identity, error)) return false;
    persisted_ = chunkIds_.size();
    return true;
}

// Reads the whole file into the arrays. `expected` is checked when non-null;
// maintenance passes null because removing entries by id needs no opinion about
// which model produced them.
bool VectorStore::readFile(const std::wstring& path, const Identity* expected,
                           Identity* found, bool* mismatch, std::wstring* error) {
    *mismatch = false;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *mismatch = true;
        *error = L"no vector file yet";
        return false;
    }

    VectorHeader header{};
    if (!readExact(file, &header, sizeof(header)) ||
        !std::equal(std::begin(kMagic), std::end(kMagic), header.magic) ||
        header.version != 2) {
        CloseHandle(file);
        *mismatch = true;
        *error = L"vector file not recognized";
        return false;
    }

    // Vectors from another model, or from text split a different way, are not
    // comparable with what we would compute now. Refusing them is the whole
    // point of carrying the identity: the alternative is a search that works
    // just well enough that nobody notices it is wrong.
    if (expected && (header.modelId != expected->modelId ||
                     header.chunkerVersion != expected->chunkerVersion ||
                     header.contentGeneration != expected->contentGeneration ||
                     (expected->dimensions != 0 &&
                      header.dimensions != expected->dimensions))) {
        CloseHandle(file);
        *mismatch = true;
        *error = L"vector file was built with a different model or chunker";
        return false;
    }

    if (found) {
        found->modelId = header.modelId;
        found->dimensions = header.dimensions;
        found->chunkerVersion = header.chunkerVersion;
        found->contentGeneration = header.contentGeneration;
    }

    dimensions_ = header.dimensions;
    const size_t count = static_cast<size_t>(header.count);

    // Guard against a header that claims more than the file holds before
    // resizing: `count` comes off disk, and a torn file could ask for a
    // hundred gigabytes.
    LARGE_INTEGER fileSize{};
    if (dimensions_ == 0 || !GetFileSizeEx(file, &fileSize) ||
        static_cast<uint64_t>(fileSize.QuadPart) <
            sizeof(VectorHeader) + static_cast<uint64_t>(count) * recordBytes(dimensions_)) {
        CloseHandle(file);
        *mismatch = true;
        *error = L"vector file is truncated";
        return false;
    }

    std::vector<char> buffer(count * recordBytes(dimensions_));
    const bool ok = buffer.empty() || readExact(file, buffer.data(), buffer.size());
    CloseHandle(file);
    if (!ok) {
        clear();
        *mismatch = true;
        *error = L"vector file is truncated";
        return false;
    }

    chunkIds_.resize(count);
    data_.resize(count * dimensions_);
    for (size_t i = 0; i < count; ++i) {
        const char* record = buffer.data() + i * recordBytes(dimensions_);
        std::memcpy(&chunkIds_[i], record, sizeof(uint32_t));
        std::memcpy(data_.data() + i * dimensions_, record + sizeof(uint32_t), dimensions_);
    }
    persisted_ = count;
    return true;
}

bool VectorStore::load(const std::wstring& path, const Identity& identity,
                       bool* mismatch, std::wstring* error) {
    return readFile(path, &identity, nullptr, mismatch, error);
}

bool VectorStore::loadForMaintenance(const std::wstring& path, Identity* identity,
                                     std::wstring* error) {
    bool mismatch = false;
    return readFile(path, nullptr, identity, &mismatch, error);
}

size_t VectorStore::removeChunks(const std::unordered_set<uint32_t>& chunkIds) {
    if (chunkIds.empty() || chunkIds_.empty()) return 0;

    // Compacted in one forward pass, writing survivors over the gaps. The
    // vectors move but their order does not change, so nothing depends on where
    // a record used to sit.
    size_t kept = 0;
    for (size_t i = 0; i < chunkIds_.size(); ++i) {
        if (chunkIds.count(chunkIds_[i])) continue;
        if (kept != i) {
            chunkIds_[kept] = chunkIds_[i];
            std::memcpy(data_.data() + kept * dimensions_,
                        data_.data() + i * dimensions_, dimensions_);
        }
        ++kept;
    }

    const size_t removed = chunkIds_.size() - kept;
    chunkIds_.resize(kept);
    data_.resize(kept * dimensions_);

    // The file no longer matches: the next write has to be a full rewrite, not
    // an append, or it would keep the removed records on disk.
    persisted_ = 0;
    return removed;
}

uint64_t modelIdentity(const std::wstring& modelPath) {
    // THE FILE'S HEAD AND ITS SIZE. Not its name.
    //
    // The name was in here, and it was wrong in a way that only showed up in
    // use: copying the model to the folder Filo looks in renamed it, the
    // identity changed, the whole vector store was rejected as "built with a
    // different model", and semantic search switched itself off in silence. A
    // rename is not a model swap.
    //
    // The first megabyte of a GGUF is its header — architecture, tensor names,
    // dimensions — which no two different models share. Together with the exact
    // byte count that is identity enough, and it costs one read of 1 MB at
    // startup rather than the 115 that hashing the whole file would.
    uint64_t hash = 1469598103934665603ull;   // FNV-1a over the size
    WIN32_FILE_ATTRIBUTE_DATA facts{};
    if (GetFileAttributesExW(modelPath.c_str(), GetFileExInfoStandard, &facts)) {
        const uint64_t size =
            (static_cast<uint64_t>(facts.nFileSizeHigh) << 32) | facts.nFileSizeLow;
        hash ^= size;
        hash *= 1099511628211ull;
    }

    HANDLE file = CreateFileW(modelPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return hash;

    std::vector<char> head(1u << 20);
    DWORD got = 0;
    if (ReadFile(file, head.data(), static_cast<DWORD>(head.size()), &got, nullptr)) {
        for (DWORD i = 0; i < got; ++i) {
            hash ^= static_cast<unsigned char>(head[i]);
            hash *= 1099511628211ull;
        }
    }
    CloseHandle(file);
    return hash;
}

}  // namespace filo
