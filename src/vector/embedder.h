#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace filo {

// Computes embeddings: turns a piece of text into a vector of numbers that
// stands for its MEANING.
//
// This is the jump from lexical search to semantic search. Searching for
// "penale", word matching only finds documents that spell it that way; two
// nearby vectors instead say that "clausola risarcitoria" is about the same
// thing, without sharing a single letter.
//
// The model is multilingual and small — under 120 million parameters — and runs
// in milliseconds. A large model would buy nothing here: it does not have to
// reason, only to place a sentence in the right spot.
class Embedder {
public:
    Embedder() = default;
    ~Embedder();

    Embedder(const Embedder&) = delete;
    Embedder& operator=(const Embedder&) = delete;

    struct Options {
        uint32_t contextTokens = 512;
        // How many chunks to process in one pass. Grouping does not change the
        // result: it changes the SHAPE of the matrix multiplications, and more
        // sequences together should use the vector units and the cache better.
        //
        // Should. Measured on this workload it does not: the batch path costs
        // about 78 ms per sequence against 24 for a single call, and the best
        // group size buys 1.11x. Left at 1, and left here rather than deleted
        // because the next model may behave differently — but change it on a
        // measurement, not on the theory above.
        uint32_t batchSequences = 1;
        int threads = 0;        // 0 = automatic
        // How many model layers to run on the graphics card. 0 = CPU only. With
        // no usable card llama.cpp falls back to the CPU on its own: this is an
        // acceleration, not a requirement.
        int gpuLayers = 99;
        // Prefixes are MODEL-SPECIFIC, not a general convention. The E5 family
        // demands them ("passage: " / "query: "); Granite and others explicitly
        // say not to use them. Adding them where they do not belong makes every
        // text start with the same tokens, and on a model that reads its vector
        // from the first token that flattens every score.
        std::string passagePrefix;
        std::string queryPrefix;
    };

    // Where the time inside a single embed() call goes, and what it was asked
    // to embed.
    //
    // Timing the call from outside says only "inference is 100% of the run",
    // which is true and useless. The question that decides what to optimize is
    // whether the cost sits in tokenization, in the encode itself, or in the
    // per-call setup — and whether it scales with sequence length or is flat.
    struct Stats {
        struct Bucket {
            uint64_t calls = 0;
            double   msEncode = 0;
        };

        double msGuard = 0;      // the untokenizable-input check
        double msTokenize = 0;
        double msBatch = 0;      // building the llama_batch
        double msEncode = 0;     // llama_encode itself
        double msReadOut = 0;    // fetching and normalizing the vector

        uint64_t calls = 0;
        uint64_t failed = 0;
        uint64_t rejected = 0;    // refused by the run-length guard
        uint64_t truncated = 0;   // longer than the context window
        uint64_t tokensTotal = 0;
        uint32_t tokensMax = 0;

        // 1-64, 65-128, 129-256, 257-512, 513+
        Bucket buckets[5];
        Bucket& bucket(int32_t tokens) {
            if (tokens <= 64) return buckets[0];
            if (tokens <= 128) return buckets[1];
            if (tokens <= 256) return buckets[2];
            if (tokens <= 512) return buckets[3];
            return buckets[4];
        }
        static const char* bucketName(int i) {
            static const char* names[5] = {"1-64", "65-128", "129-256", "257-512", "513+"};
            return names[i];
        }
    };

    const Stats& stats() const { return stats_; }
    void resetStats() { stats_ = Stats{}; }

    bool load(const std::wstring& modelPath, const Options& options, std::wstring* error);
    bool ready() const { return context_ != nullptr; }
    uint32_t dimensions() const { return dimensions_; }
    uint32_t batchSequences() const { return options_.batchSequences; }

    // Embeds several texts in one pass. `out` must have room for
    // texts.size() * dimensions() values.
    bool embedPassages(const std::vector<std::string>& texts, float* out);

    // The E5 family wants a prefix that tells the stored text apart from the
    // question: "passage: " for the first, "query: " for the second. Leaving
    // it out makes the results noticeably worse, and it is the most common
    // mistake people make with these models.
    bool embedPassage(const std::string& text, float* out);
    bool embedQuery(const std::string& text, float* out);

    // How many tokens fit in one pass. Longer text is truncated.
    uint32_t contextTokens() const { return contextTokens_; }

private:
    bool embed(const std::string& text, float* out);

    void* model_ = nullptr;
    void* context_ = nullptr;
    uint32_t dimensions_ = 0;
    uint32_t contextTokens_ = 512;
    Options options_;
    Stats stats_;
};

}  // namespace filo
