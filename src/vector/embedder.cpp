#include "vector/embedder.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>

#include "llama.h"

#include "vector/vector_store.h"

#if FILO_HAVE_VULKAN
#include "platform/vulkan_probe.h"
#endif

namespace filo {
namespace {

std::once_flag backendOnce;

std::string toUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0,
                                         nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), size, nullptr, nullptr);
    return out;
}

// The longest run of characters with no separator that we will hand to the
// tokenizer.
//
// This is not a stylistic limit, it is a crash guard. Granite's pretokenizer
// regex is not one of the patterns llama.cpp splits by hand, so it falls back
// to std::regex, and the MSVC implementation recurses per character on
// `\p{Lu}*\p{Ll}+`. A few thousand characters with no break — base64 payloads,
// minified JavaScript, hex dumps — exhaust the stack, and llama.cpp turns that
// into an exception thrown straight through its C API.
//
// Losing this content costs nothing: a run like that carries no meaning worth
// embedding, and a vector built from it sits near everything without being
// close to anything.
// Every alternative in that regex is a quantified run — `\p{Lu}*\p{Ll}+`,
// `[^\s\p{L}\p{N}]+`, `\s+` — and each one recurses per character. So the
// limit has to apply to a run of ANY single class, not just to words: two
// thousand spaces or a wall of '=' separators blow the same stack a base64
// payload does.
constexpr size_t kMaxLetterRun = 1024;   // CJK prose has no spaces; leave it room
constexpr size_t kMaxOtherRun = 256;     // whitespace or punctuation this long is never prose

enum class CharClass { Letter, Space, Other };

CharClass classify(unsigned char c) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
        return CharClass::Space;
    }
    if (c >= 0x80) return CharClass::Letter;   // assume non-ASCII is script
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return CharClass::Letter;
    }
    return CharClass::Other;
}

// Rejects text the tokenizer cannot survive. Continuation bytes fold into the
// character they belong to, so a dense CJK passage is measured in characters
// rather than bytes and is not discarded for lacking spaces.
//
// Losing this content costs nothing: a run like that carries no meaning worth
// embedding, and a vector built from it sits near everything without being
// close to anything.
bool isTokenizable(const std::string& text) {
    CharClass current = CharClass::Space;
    size_t run = 0;
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        if ((c & 0xC0) == 0x80) continue;   // continuation byte, same character

        const CharClass here = classify(c);
        if (here != current) {
            current = here;
            run = 0;
        }
        const size_t limit = here == CharClass::Letter ? kMaxLetterRun : kMaxOtherRun;
        if (++run > limit) return false;
    }
    return true;
}

// No text should ever need more tokens than this. A cap keeps a hostile or
// broken input from turning a returned requirement into a huge allocation.
constexpr size_t kTokenCeiling = 1u << 20;

// Tokenizes `text` into `tokens`, growing the buffer as the model demands, and
// reports how many tokens the text really needs.
//
// llama_tokenize returns the token count, or the NEGATIVE of the required count
// when the buffer is too small — and in that case it writes NOTHING: the copy
// loop sits after the size check. That contract is easy to get wrong, and the
// wrong version was live here.
//
// The previous code retried with the SAME insufficient capacity, threw away the
// second result, and returned the buffer size as if it had succeeded. Callers
// then embedded `contextTokens` zero-initialized entries — so every chunk over
// the window produced the same meaningless vector, and produced it at the
// slowest possible length. At the time chunks were cut to 2048 BYTES against a
// 512-token window, so 92% of them were over it: this was not a rare edge, it
// was most of the corpus. Chunker version 3 counts tokens and aims under the
// window, which is what makes going over it rare rather than normal — the guard
// stays because "rare" is not "never".
//
// llama_tokenize is a C entry point with C++ behind it, and it throws. Anything
// escaping here would cross a C boundary into std::terminate, which is how one
// malformed chunk took down a five-hour indexing run.
int32_t safeTokenize(const llama_vocab* vocab, const std::string& text,
                     std::vector<llama_token>* tokens, size_t windowTokens,
                     bool* truncated) {
    *truncated = false;
    try {
        int32_t count = llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()),
                                       tokens->data(), static_cast<int32_t>(tokens->size()),
                                       /*add_special=*/true, /*parse_special=*/false);

        if (count < 0) {
            // INT32_MIN has no positive counterpart, and llama.cpp returns it
            // for a tokenization too large to describe at all. Negating it is
            // undefined behaviour, so it is rejected before the arithmetic.
            if (count == std::numeric_limits<int32_t>::min()) return -1;

            const auto required = static_cast<size_t>(-static_cast<int64_t>(count));
            if (required == 0 || required > kTokenCeiling) return -1;

            tokens->assign(required, 0);
            count = llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()),
                                   tokens->data(), static_cast<int32_t>(tokens->size()),
                                   true, false);
            // A buffer sized from the model's own answer must now fit. If it
            // still does not, something is inconsistent and the safe move is to
            // skip this text rather than embed whatever the buffer holds.
            if (count <= 0) return -1;
        }

        // Only NOW is truncation applied, to real tokens: the leading piece of
        // an over-long chunk still says what the chunk is about, whereas a
        // buffer of zeros says nothing at all.
        if (static_cast<size_t>(count) > windowTokens) {
            *truncated = true;
            count = static_cast<int32_t>(windowTokens);
        }
        return count;
    } catch (...) {
        return -1;
    }
}

}  // namespace

Embedder::~Embedder() {
    if (context_) llama_free(static_cast<llama_context*>(context_));
    if (model_) llama_model_free(static_cast<llama_model*>(model_));
}

bool Embedder::load(const std::wstring& modelPath, const Options& options,
                    std::wstring* error) {
    options_ = options;
    contextTokens_ = options.contextTokens;
    std::call_once(backendOnce, [] {
        // SILENCED BEFORE THE BACKEND STARTS, not after.
        //
        // It was the other way round, and ggml prints its Vulkan banner during
        // backend registration — so every launch spat two lines about the
        // graphics card onto the terminal before anything could stop it.
        if (GetEnvironmentVariableW(L"FILO_LLAMA_LOG", nullptr, 0) == 0) {
            llama_log_set([](ggml_log_level, const char*, void*) {}, nullptr);
        }
        // Loading a model prints dozens more diagnostic lines. FILO_LLAMA_LOG
        // brings all of it back, because silencing WITHOUT leaving a way to
        // turn it on means that the day a model fails to load, nobody knows
        // why.
        llama_backend_init();
    });

    llama_model_params modelParams = llama_model_default_params();
    modelParams.n_gpu_layers = options_.gpuLayers;
    // Safety switch: if the card or the driver misbehaves, the user can fall
    // back to the CPU without reinstalling anything.
    if (GetEnvironmentVariableW(L"FILO_FORCE_CPU", nullptr, 0) > 0) {
        modelParams.n_gpu_layers = 0;
    }
#if FILO_HAVE_VULKAN
    // No Vulkan loader on this machine: asking for GPU layers would only send
    // ggml looking for a backend that cannot exist. The CPU is 12% slower here
    // and always present, which is the better half of that trade.
    if (!vulkanAvailable()) modelParams.n_gpu_layers = 0;
#endif

    const std::string path = toUtf8(modelPath);
    model_ = llama_model_load_from_file(path.c_str(), modelParams);
    if (!model_) {
        *error = L"model cannot be loaded: " + modelPath;
        return false;
    }

    const uint32_t sequences = std::max(1u, options_.batchSequences);

    llama_context_params contextParams = llama_context_default_params();
    contextParams.embeddings = true;
    // The space has to cover ALL the sequences in the group, not just one.
    contextParams.n_ctx = contextTokens_ * sequences;
    contextParams.n_batch = contextTokens_ * sequences;
    contextParams.n_ubatch = contextTokens_ * sequences;
    contextParams.n_seq_max = sequences;
    // UNSPECIFIED, not MEAN: this lets llama.cpp read from the model file how
    // it wants to be pooled. Forcing it from outside means guessing, and
    // guessing wrong produces no error at all — only worse vectors. E5 models
    // average every token, Granite and many others read the first: those are
    // the model's choices, not ours.
    contextParams.pooling_type = LLAMA_POOLING_TYPE_UNSPECIFIED;
    // Diagnostic: different models pool the token states differently (mean,
    // first token, last). Getting it wrong produces no errors, only worse
    // vectors — so it has to be testable.
    wchar_t poolingChoice[16] = {};
    if (GetEnvironmentVariableW(L"FILO_POOLING", poolingChoice, 16) > 0) {
        const std::wstring choice = poolingChoice;
        if (choice == L"cls")  contextParams.pooling_type = LLAMA_POOLING_TYPE_CLS;
        if (choice == L"last") contextParams.pooling_type = LLAMA_POOLING_TYPE_LAST;
        if (choice == L"none") contextParams.pooling_type = LLAMA_POOLING_TYPE_NONE;
    }
    contextParams.n_threads = options_.threads > 0
                                  ? options_.threads
                                  : static_cast<int32_t>(std::max(
                                        1u, std::thread::hardware_concurrency() / 2));
    contextParams.n_threads_batch = contextParams.n_threads;

    context_ = llama_init_from_model(static_cast<llama_model*>(model_), contextParams);
    if (!context_) {
        llama_model_free(static_cast<llama_model*>(model_));
        model_ = nullptr;
        *error = L"inference context cannot be created";
        return false;
    }

    dimensions_ = static_cast<uint32_t>(
        llama_model_n_embd(static_cast<llama_model*>(model_)));
    return dimensions_ > 0;
}

bool Embedder::embed(const std::string& text, float* out) {
    if (!context_ || text.empty()) return false;

    const auto clock = [] { return std::chrono::steady_clock::now(); };
    const auto elapsedMs = [](auto from, auto to) {
        return std::chrono::duration<double, std::milli>(to - from).count();
    };

    const auto tGuard = clock();
    if (!isTokenizable(text)) {
        ++stats_.rejected;
        return false;
    }

    auto* context = static_cast<llama_context*>(context_);
    auto* model = static_cast<llama_model*>(model_);
    const llama_vocab* vocab = llama_model_get_vocab(model);

    std::vector<llama_token> tokens(contextTokens_);
    bool truncated = false;
    const auto tTokenizeStart = clock();
    const int32_t count = safeTokenize(vocab, text, &tokens, contextTokens_, &truncated);
    const auto tTokenizeEnd = clock();

    stats_.msGuard += elapsedMs(tGuard, tTokenizeStart);
    stats_.msTokenize += elapsedMs(tTokenizeStart, tTokenizeEnd);
    if (count <= 0) {
        ++stats_.failed;
        return false;
    }
    if (truncated) ++stats_.truncated;

    llama_memory_clear(llama_get_memory(context), true);

    llama_batch batch = llama_batch_init(count, 0, 1);
    for (int32_t i = 0; i < count; ++i) {
        batch.token[i] = tokens[i];
        batch.pos[i] = i;
        batch.n_seq_id[i] = 1;
        batch.seq_id[i][0] = 0;
        batch.logits[i] = 0;
    }
    batch.n_tokens = count;
    const auto tBatch = clock();

    const bool ok = llama_encode(context, batch) == 0;
    const auto tEncode = clock();

    if (ok) {
        const float* embedding = llama_get_embeddings_seq(context, 0);
        if (embedding) {
            std::memcpy(out, embedding, dimensions_ * sizeof(float));
            // Normalizing once here makes comparison a plain dot product, which
            // is what makes the full scan affordable.
            normalizeVector(out, dimensions_);
        } else {
            llama_batch_free(batch);
            ++stats_.failed;
            return false;
        }
    }
    llama_batch_free(batch);

    const auto tDone = clock();
    stats_.msBatch += elapsedMs(tTokenizeEnd, tBatch);
    stats_.msEncode += elapsedMs(tBatch, tEncode);
    stats_.msReadOut += elapsedMs(tEncode, tDone);
    ++stats_.calls;
    stats_.tokensTotal += static_cast<uint64_t>(count);
    stats_.tokensMax = std::max(stats_.tokensMax, static_cast<uint32_t>(count));

    // Encode time bucketed by sequence length. If a differing token count is
    // what stops ggml reusing its compute graph, the cost per bucket is where
    // it shows.
    stats_.bucket(count).calls += 1;
    stats_.bucket(count).msEncode += elapsedMs(tBatch, tEncode);
    return ok;
}

bool Embedder::embedPassages(const std::vector<std::string>& texts, float* out) {
    if (!context_ || texts.empty()) return false;
    if (texts.size() == 1) return embedPassage(texts[0], out);

    auto* context = static_cast<llama_context*>(context_);
    auto* model = static_cast<llama_model*>(model_);
    const llama_vocab* vocab = llama_model_get_vocab(model);

    // Tokenize every text, each with its own sequence id: that is how the model
    // keeps them apart inside a single pass, with none influencing another.
    std::vector<std::vector<llama_token>> allTokens;
    allTokens.reserve(texts.size());
    int32_t totalTokens = 0;

    for (const std::string& text : texts) {
        const std::string prefixed =
            options_.passagePrefix.empty() ? text : options_.passagePrefix + text;
        if (!isTokenizable(prefixed)) return false;
        std::vector<llama_token> tokens(contextTokens_);
        bool truncated = false;
        const int32_t count =
            safeTokenize(vocab, prefixed, &tokens, contextTokens_, &truncated);
        if (count <= 0) return false;
        tokens.resize(count);
        totalTokens += count;
        allTokens.push_back(std::move(tokens));
    }

    llama_memory_clear(llama_get_memory(context), true);

    llama_batch batch = llama_batch_init(totalTokens, 0,
                                         static_cast<int32_t>(texts.size()));
    int32_t at = 0;
    for (size_t sequence = 0; sequence < allTokens.size(); ++sequence) {
        for (size_t i = 0; i < allTokens[sequence].size(); ++i) {
            batch.token[at] = allTokens[sequence][i];
            batch.pos[at] = static_cast<llama_pos>(i);
            batch.n_seq_id[at] = 1;
            batch.seq_id[at][0] = static_cast<llama_seq_id>(sequence);
            batch.logits[at] = 0;
            ++at;
        }
    }
    batch.n_tokens = totalTokens;

    const bool ok = llama_encode(context, batch) == 0;
    if (ok) {
        for (size_t sequence = 0; sequence < allTokens.size(); ++sequence) {
            const float* embedding =
                llama_get_embeddings_seq(context, static_cast<llama_seq_id>(sequence));
            float* destination = out + sequence * dimensions_;
            if (!embedding) {
                llama_batch_free(batch);
                return false;
            }
            std::memcpy(destination, embedding, dimensions_ * sizeof(float));
            normalizeVector(destination, dimensions_);
        }
    }
    llama_batch_free(batch);
    return ok;
}

bool Embedder::embedPassage(const std::string& text, float* out) {
    return embed(options_.passagePrefix.empty() ? text : options_.passagePrefix + text, out);
}

bool Embedder::embedQuery(const std::string& text, float* out) {
    return embed(options_.queryPrefix.empty() ? text : options_.queryPrefix + text, out);
}

}  // namespace filo
