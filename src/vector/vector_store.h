#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace filo {

// Store for semantic vectors, one per text chunk.
//
// WHY NOT AN APPROXIMATE INDEX (vec1, IVFADC, HNSW)
//
// The background reading recommended an approximate index, on the strength of
// one sqlite-vec measurement: 75 ms for a full scan of 100,000 vectors at 384
// dimensions. But that number is scalar code on a single thread.
//
// Run the numbers for OUR case: 220,000 chunks x 384 dimensions is 84 million
// multiplications per query. Across twelve threads, with the vectors quantized
// to one byte and laid out contiguously, that is a few milliseconds. An
// approximate index would cost a training step, a periodic rebuild, precision
// drift as the index changes, and a pre-1.0 dependency — to solve a problem we
// do not have at this size.
//
// Same call as layer 1: a flat array beats a clever structure for as long as
// the volume allows it. At a million chunks the full scan still lands under
// 50 ms; past that the approximate route stays open and this interface does
// not change.
//
// QUANTIZATION
//
// Vectors arrive normalized, so every component sits between -1 and 1 and maps
// onto a signed byte with no per-vector scale factor. It costs a little
// precision and saves three quarters of the memory: 84 MB instead of 338,
// which is the difference between fitting in cache and not.
class VectorStore {
public:
    struct Neighbor {
        uint32_t chunkId = 0;
        float    score = 0.0f;   // cosine similarity, -1 to 1
    };

    // What the vectors on disk MEAN. A store built with a different model, a
    // different chunker or a different number of dimensions holds vectors that
    // are not comparable with the ones we would compute now — mixing them
    // silently would produce a search that is subtly, unfixably wrong. So the
    // identity travels in the file header and a mismatch discards the file.
    struct Identity {
        uint64_t modelId = 0;          // hash of model file name + size
        uint32_t dimensions = 0;
        uint32_t chunkerVersion = 0;
        // Bumped every time the content index is rebuilt from scratch. Vectors
        // are keyed by chunk id, and a rebuild renumbers every chunk, so
        // without this the old vectors would still load and point at rows that
        // now belong to different documents.
        uint32_t contentGeneration = 0;
    };

    void configure(uint32_t dimensions);
    void reserve(size_t count);
    void clear();

    // `vector` must already be normalized (length 1).
    void add(uint32_t chunkId, const float* vector);

    // Adds a chunk whose text is IDENTICAL to one already computed: copies the
    // vector instead of running inference again. Not an approximation — same
    // input bytes, same output vector.
    void addExisting(uint32_t chunkId, uint32_t existingPosition);

    size_t size() const { return chunkIds_.size(); }
    uint32_t dimensions() const { return dimensions_; }
    size_t memoryBytes() const;

    // The chunk ids currently held, in insertion order. The indexer uses this
    // to skip work it has already done after an interrupted run.
    const std::vector<uint32_t>& chunkIds() const { return chunkIds_; }

    // The `topK` most similar chunks. `query` must be normalized like the rest.
    std::vector<Neighbor> search(const float* query, size_t topK,
                                 unsigned threadCount = 0) const;

    // Writes every record, replacing whatever was there.
    bool save(const std::wstring& path, const Identity& identity,
              std::wstring* error) const;

    // Writes only the records added since the last flush or load, then updates
    // the count in the header.
    //
    // The previous version rewrote the whole file every ten seconds. Over a run
    // long enough to need checkpoints that is quadratic: a five-hour pass would
    // write tens of gigabytes to say something a few kilobytes of new records
    // already said. Appending makes a checkpoint cost what it actually is.
    bool flush(const std::wstring& path, const Identity& identity,
               std::wstring* error);

    // Loads a store, but only if `identity` matches what the file was built
    // with. `mismatch` distinguishes "no usable file, start over" from a real
    // I/O failure, so the caller can rebuild quietly instead of giving up.
    bool load(const std::wstring& path, const Identity& identity,
              bool* mismatch, std::wstring* error);

    // Loads a store WITHOUT checking its identity, reporting what it found.
    //
    // For maintenance that only removes entries by chunk id. That work needs no
    // opinion about which model produced the vectors — it needs the ids — and
    // requiring the identity would mean loading a 115 MB model just to delete
    // some rows. The identity comes back so it can be written out unchanged.
    bool loadForMaintenance(const std::wstring& path, Identity* identity,
                            std::wstring* error);

    // Drops every entry whose chunk id is in `chunkIds`. Returns how many went.
    //
    // Needed because chunk_id is an INTEGER PRIMARY KEY without AUTOINCREMENT:
    // SQLite hands out max(rowid)+1, so deleting the highest ids lets them be
    // reused by unrelated chunks later. A vector left behind would then be
    // silently attached to a different document — the same trap as an NTFS
    // sequence number, one level up.
    size_t removeChunks(const std::unordered_set<uint32_t>& chunkIds);

private:
    bool readFile(const std::wstring& path, const Identity* expected, Identity* found,
                  bool* mismatch, std::wstring* error);

    uint32_t dimensions_ = 0;
    std::vector<int8_t> data_;        // size() * dimensions_, contiguous
    std::vector<uint32_t> chunkIds_;

    // How many records are already on disk. Everything past this is pending.
    size_t persisted_ = 0;
};

// Scales a vector to length 1. Cosine similarity between normalized vectors is
// a plain dot product, which is what makes the full scan affordable.
void normalizeVector(float* vector, uint32_t dimensions);

// Identifies a model by name and size. Not a content hash: reading 115 MB at
// every startup to notice a file nobody touched is not worth it, and a model
// swapped underneath the same name and size is not a case that happens by
// accident.
uint64_t modelIdentity(const std::wstring& modelPath);

}  // namespace filo
