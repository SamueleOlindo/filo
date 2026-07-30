#include "search/query_model.h"

#include <windows.h>

#include <algorithm>
#include <mutex>
#include <vector>

#include "llama.h"

#if FILO_HAVE_VULKAN
#include "platform/vulkan_probe.h"
#endif

namespace filo {
namespace {

std::once_flag backendOnce;

// The reply, in full.
//
// Every byte of it is fixed except the subject and the two choices, and the
// choices come from closed lists. The model is not being asked to write JSON —
// it is being asked to fill three blanks in a sentence it cannot alter. That
// removes the entire category of "the model answered in prose", "the model
// wrapped it in a code fence", "the model invented a date format", without a
// single line of defensive parsing.
constexpr char kGrammar[] = R"gbnf(
root ::= "{\"subject\":\"" subject "\",\"format\":\"" format "\",\"when\":\"" when "\"}"
subject ::= [^"\\]*
format ::= "any" | "pdf" | "doc" | "sheet" | "slides" | "image" | "video" | "audio" | "code" | "text" | "archive" | "ebook"
when ::= "any" | "today" | "yesterday" | "week" | "month" | "lastmonth" | "year" | "lastyear" | "recent"
)gbnf";

constexpr char kSystemPrompt[] =
    "You turn a file-search question into three fields.\n"
    "subject: the words to search for, with any mention of file format, folder "
    "or date REMOVED. Keep the user's own language. Never add words.\n"
    "format: the file format the user asked for, or \"any\" if they did not.\n"
    "when: the time the user asked for, or \"any\" if they did not.\n"
    "If you are unsure, answer \"any\". Answering \"any\" is always safe.";

// A couple of worked examples, in both languages the user actually types in.
// Without them a 3B model tends to answer the question rather than classify it.
constexpr char kExamples[] =
    "Question: quanto ho pagato di tasse l'anno scorso\n"
    "{\"subject\":\"tasse pagate\",\"format\":\"any\",\"when\":\"lastyear\"}\n"
    "Question: where is that spreadsheet with the invoices\n"
    "{\"subject\":\"invoices\",\"format\":\"sheet\",\"when\":\"any\"}\n"
    "Question: la mail che mi ha mandato marco\n"
    "{\"subject\":\"marco\",\"format\":\"any\",\"when\":\"any\"}\n";

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

std::wstring toWide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), size);
    return out;
}

// Pulls a field out of the fixed reply. The grammar guarantees the shape, so
// this is extraction and not parsing.
std::string field(const std::string& reply, const char* key) {
    const std::string opening = std::string("\"") + key + "\":\"";
    const size_t at = reply.find(opening);
    if (at == std::string::npos) return {};
    const size_t begin = at + opening.size();
    const size_t end = reply.find('"', begin);
    if (end == std::string::npos) return {};
    return reply.substr(begin, end - begin);
}

const char* extensionsFor(const std::string& format) {
    if (format == "pdf") return "pdf";
    if (format == "doc") return "doc docx odt rtf";
    if (format == "sheet") return "xls xlsx ods csv";
    if (format == "slides") return "ppt pptx odp";
    if (format == "image") return "jpg jpeg png gif webp heic bmp";
    if (format == "video") return "mp4 mkv avi mov wmv webm";
    if (format == "audio") return "mp3 flac wav m4a ogg";
    if (format == "text") return "txt md";
    if (format == "archive") return "zip rar 7z tar gz";
    if (format == "ebook") return "epub mobi azw3";
    return nullptr;
}

}  // namespace

QueryModel::~QueryModel() {
    if (context_) llama_free(static_cast<llama_context*>(context_));
    if (model_) llama_model_free(static_cast<llama_model*>(model_));
}

bool QueryModel::ensureLoaded(const std::wstring& modelPath, std::wstring* error) {
    std::call_once(loadOnce_, [&] {
        std::lock_guard<std::mutex> lock(busy_);
        loaded_ = load(modelPath, &loadError_);
    });
    if (!loaded_ && error) *error = loadError_;
    return loaded_;
}

bool QueryModel::load(const std::wstring& modelPath, std::wstring* error) {
    // Silenced before the backend starts: ggml prints its Vulkan banner during
    // registration, so doing this afterwards is too late.
    std::call_once(backendOnce, [] {
        if (GetEnvironmentVariableW(L"FILO_LLAMA_LOG", nullptr, 0) == 0) {
            llama_log_set([](ggml_log_level, const char*, void*) {}, nullptr);
        }
        llama_backend_init();
    });

    llama_model_params modelParams = llama_model_default_params();
    modelParams.n_gpu_layers = 99;
    if (GetEnvironmentVariableW(L"FILO_FORCE_CPU", nullptr, 0) > 0) {
        modelParams.n_gpu_layers = 0;
    }
#if FILO_HAVE_VULKAN
    if (!vulkanAvailable()) modelParams.n_gpu_layers = 0;
#endif

    model_ = llama_model_load_from_file(toUtf8(modelPath).c_str(), modelParams);
    if (!model_) {
        *error = L"query model cannot be loaded: " + modelPath;
        return false;
    }

    llama_context_params contextParams = llama_context_default_params();
    contextParams.n_ctx = 1024;      // prompt plus a very short reply
    contextParams.n_batch = 1024;
    contextParams.n_ubatch = 512;
    context_ = llama_init_from_model(static_cast<llama_model*>(model_), contextParams);
    if (!context_) {
        llama_model_free(static_cast<llama_model*>(model_));
        model_ = nullptr;
        *error = L"query model context cannot be created";
        return false;
    }
    return true;
}

bool QueryModel::worthAsking(const QueryPlan& plan) {
    // Already understood by the cheap pass: the model could only disagree with
    // something that is not in doubt.
    if (plan.hasFilters()) return false;

    // Fewer than three words is a keyword search, not a question. "contratto"
    // means find the word; there is nothing to interpret.
    int words = 1;
    for (const wchar_t c : plan.text) {
        if (c == L' ') ++words;
    }
    if (words < 3) return false;
    if (plan.text.size() > 200) return false;   // not a question either
    return true;
}

bool QueryModel::refine(const std::wstring& query, int64_t now, QueryPlan* plan) {
    if (!context_) return false;

    // Never waits. If the model is mid-sentence explaining a file on the other
    // screen, this query goes through the other four layers and is none the
    // worse for it — whereas a search that stalls two seconds is a search the
    // user notices.
    std::unique_lock<std::mutex> lock(busy_, std::try_to_lock);
    if (!lock.owns_lock()) return false;

    try {
        return refineUnguarded(query, now, plan);
    } catch (const std::exception& failure) {
        lastReply_ = std::string("exception: ") + failure.what();
        return false;
    } catch (...) {
        lastReply_ = "exception";
        return false;
    }
}

bool QueryModel::generate(const std::string& prompt, const char* grammarText,
                          int maxTokens, std::string* out) {
    auto* context = static_cast<llama_context*>(context_);
    auto* model = static_cast<llama_model*>(model_);
    const llama_vocab* vocab = llama_model_get_vocab(model);

    std::vector<llama_token> tokens(2048);
    int32_t count = 0;
    try {
        count = llama_tokenize(vocab, prompt.data(), static_cast<int32_t>(prompt.size()),
                               tokens.data(), static_cast<int32_t>(tokens.size()),
                               /*add_special=*/false, /*parse_special=*/true);
    } catch (...) {
        return false;
    }
    if (count <= 0) return false;
    tokens.resize(count);

    llama_memory_clear(llama_get_memory(context), true);

    llama_sampler* chain = llama_sampler_chain_init(llama_sampler_chain_default_params());
    if (!chain) return false;
    if (grammarText) {
        llama_sampler* grammar = llama_sampler_init_grammar(vocab, grammarText, "root");
        if (!grammar) {
            llama_sampler_free(chain);
            return false;
        }
        llama_sampler_chain_add(chain, grammar);
    }
    // Greedy. Both callers want the model's best answer, not a varied one: a
    // classification has a right choice, and an explanation that changes every
    // time it is asked would look broken.
    llama_sampler_chain_add(chain, llama_sampler_init_greedy());

    bool ok = false;
    std::string reply;
    if (llama_decode(context, llama_batch_get_one(tokens.data(), count)) == 0) {
        char piece[256];
        for (int step = 0; step < maxTokens; ++step) {
            // No llama_sampler_accept here: llama_sampler_sample already ends
            // with one. Calling it again advances the grammar twice per token,
            // which empties its stack a token or two in and aborts.
            const llama_token next = llama_sampler_sample(chain, context, -1);
            if (llama_vocab_is_eog(vocab, next)) { ok = true; break; }

            const int32_t written =
                llama_token_to_piece(vocab, next, piece, sizeof(piece), 0, false);
            if (written > 0) reply.append(piece, static_cast<size_t>(written));

            llama_token one = next;
            if (llama_decode(context, llama_batch_get_one(&one, 1)) != 0) break;
            if (grammarText && reply.find('}') != std::string::npos) { ok = true; break; }
        }
        if (!grammarText) ok = !reply.empty();   // ran out of tokens, still an answer
    }
    llama_sampler_free(chain);

    lastReply_ = reply;
    *out = reply;
    return ok && !reply.empty();
}

bool QueryModel::refineUnguarded(const std::wstring& query, int64_t now, QueryPlan* plan) {
    std::string prompt = "<|begin_of_text|><|start_header_id|>system<|end_header_id|>\n\n";
    prompt += kSystemPrompt;
    prompt += "\n\n";
    prompt += kExamples;
    prompt += "<|eot_id|><|start_header_id|>user<|end_header_id|>\n\nQuestion: ";
    prompt += toUtf8(query);
    prompt += "<|eot_id|><|start_header_id|>assistant<|end_header_id|>\n\n";

    std::string reply;
    if (!generate(prompt, kGrammar, 96, &reply)) return false;

    // --- fold the answer into the plan --------------------------------------
    //
    // Only ever ADDS. If the model says "any", or says something the mapping
    // does not know, the plan keeps what it had. There is no path here that
    // makes a query more restrictive than the user wrote it.
    // Everything lands on a COPY first, and the copy is only taken if the whole
    // answer survives validation.
    //
    // Applying the format and the date before checking the subject meant a
    // rejected subject still left the model's guesses in place — and a subject
    // wrong enough to reject is evidence that the model misread the question,
    // which makes its format and its date exactly as suspect. Half an answer
    // from a model that misunderstood is worse than none.
    QueryPlan candidate = *plan;

    const std::string format = field(reply, "format");
    if (const char* extensions = extensionsFor(format)) {
        std::wstring token;
        for (const char* c = extensions;; ++c) {
            if (*c == ' ' || *c == '\0') {
                if (!token.empty() && std::find(candidate.extensions.begin(),
                                                candidate.extensions.end(),
                                                token) == candidate.extensions.end()) {
                    candidate.extensions.push_back(token);
                }
                token.clear();
                if (*c == '\0') break;
            } else {
                token.push_back(static_cast<wchar_t>(*c));
            }
        }
    }

    // The time words go back through the deterministic parser rather than being
    // converted here. One implementation of "last month", and it is the one
    // already covered by tests.
    const std::string when = field(reply, "when");
    if (when != "any" && !when.empty()) {
        static const struct { const char* name; const wchar_t* phrase; } kWhen[] = {
            {"today", L"oggi"},        {"yesterday", L"ieri"},
            {"week", L"settimana scorsa"}, {"month", L"questo mese"},
            {"lastmonth", L"mese scorso"}, {"year", L"questo anno"},
            {"lastyear", L"anno scorso"},  {"recent", L"recenti"},
        };
        for (const auto& entry : kWhen) {
            if (when != entry.name) continue;
            const QueryPlan dates = planQuery(entry.phrase, now);
            if (candidate.after == 0) candidate.after = dates.after;
            if (candidate.before == 0) candidate.before = dates.before;
            break;
        }
    }

    // --- the subject, accepted only if it kept enough of the question --------
    //
    // Two ways for this to go wrong, and both were observed. Adding words means
    // searching for things nobody asked about. Cutting too much loses the
    // question: "quanto pago se interrompo prima" came back as "pago", which is
    // about paying and no longer about stopping early.
    //
    // So: every word must come from the original, and a question of four words
    // or more may not collapse to one. Rejecting discards the WHOLE candidate —
    // format and date included — and leaves the plan exactly as the
    // deterministic pass built it, which already works.
    const std::wstring subject = toWide(field(reply, "subject"));
    if (subject.empty()) return true;

    auto wordsOf = [](const std::wstring& text) {
        std::vector<std::wstring> words;
        std::wstring current;
        for (const wchar_t c : text) {
            if (c == L' ') {
                if (!current.empty()) words.push_back(current);
                current.clear();
            } else {
                current.push_back(static_cast<wchar_t>(::towlower(c)));
            }
        }
        if (!current.empty()) words.push_back(current);
        return words;
    };

    const std::vector<std::wstring> asked = wordsOf(query);
    const std::vector<std::wstring> answered = wordsOf(subject);
    if (answered.empty()) return true;
    if (asked.size() >= 4 && answered.size() < 2) return true;

    for (const std::wstring& word : answered) {
        if (std::find(asked.begin(), asked.end(), word) == asked.end()) return true;
    }

    // Everything held. Take the answer whole.
    candidate.text = subject;
    *plan = candidate;
    return true;
}

// --- explaining a file ------------------------------------------------------

namespace {

// Prose, but never more than one line of it. A model left to itself answers a
// question like this with a bulleted list and a closing offer to help further,
// and the interface has room for a sentence.
constexpr char kDescribeGrammar[] = R"gbnf(
root ::= [^\n]+
)gbnf";

// Note what is NOT asked for: whether the file is safe to delete.
//
// The first version of this prompt asked for exactly that, and the model
// obliged — it called a 4 GB game asset archive "a save file" and said an
// Android emulator disk could go without losing anything. Both wrong, both
// stated with the same confidence as the answers that were right.
//
// Safe-to-delete is the classifier's job, and the classifier can show its
// working. Asking an unchecked model the same question puts a second opinion
// next to it with nothing behind it. So this asks only what the file IS and
// what put it there — still a guess, but a guess about identity, sitting beside
// a verdict Filo can defend rather than competing with it.
constexpr char kDescribeSystem[] =
    "You say what a file on a Windows PC is, in ONE short sentence a "
    "non-technical person understands.\n"
    "Say what it is and what program put it there. Never say whether it is safe "
    "to delete — that is not your job and you will be wrong.\n"
    "If the path does not tell you, say \"I don't recognise this one\" and "
    "nothing more. Never name a product you are not sure about, and never "
    "guess at what a file contains.\n"
    "One sentence. No lists, no preamble, no offer to help further.";

// The examples are real conversation turns, not a block of text in the system
// prompt. Written as text the model imitated their FORMAT — every answer came
// back as "File: C:\...\thing.obj. This is..." — because a transcript pasted
// into a system prompt is just a pattern to continue. As turns it is a dialogue
// to take part in, and the echo disappears.
//
// The fourth pair is the important one. A path can name a program without
// saying what the file does, and a model handed a well-known game's title in
// the path will happily invent the rest — it called an asset archive "a save
// game file created by the game's Forge mode", a mode that game does not have.
// This teaches the shape of an answer that stops at what the path actually
// says.
struct Example {
    const char* path;
    const char* answer;
};
constexpr Example kDescribeExamples[] = {
    {"C:\\Users\\you\\project\\node_modules\\.bin\\tsc  (12 MB)",
     "That is the TypeScript compiler, installed into your project by npm."},
    {"C:\\Users\\you\\AppData\\Local\\Docker\\wsl\\disk\\docker_data.vhdx  (24.0 GB)",
     "That is the virtual disk Docker keeps your images and containers in."},
    {"C:\\Users\\you\\Documents\\zqx-4417.bin  (3.0 GB)",
     "I don't recognise this one."},
    {"D:\\Steam\\steamapps\\common\\Some Game\\data\\pak04.vpk  (7.2 GB)",
     "That is one of the data files for the game Some Game; what is inside it, "
     "I can't tell from the name."},
};

std::string humanSize(uint64_t bytes) {
    char text[32];
    if (bytes >= (1ull << 30)) {
        std::snprintf(text, sizeof(text), "%.1f GB", bytes / 1073741824.0);
    } else {
        std::snprintf(text, sizeof(text), "%.0f MB", bytes / 1048576.0);
    }
    return text;
}

}  // namespace

bool QueryModel::describe(const std::wstring& path, uint64_t size, std::wstring* out) {
    if (!context_) return false;

    // Blocking, unlike refine(). Somebody clicked and is watching a spinner.
    std::lock_guard<std::mutex> lock(busy_);

    std::string prompt = "<|begin_of_text|><|start_header_id|>system<|end_header_id|>\n\n";
    prompt += kDescribeSystem;
    prompt += "<|eot_id|>";
    for (const Example& example : kDescribeExamples) {
        prompt += "<|start_header_id|>user<|end_header_id|>\n\n";
        prompt += example.path;
        prompt += "<|eot_id|><|start_header_id|>assistant<|end_header_id|>\n\n";
        prompt += example.answer;
        prompt += "<|eot_id|>";
    }
    prompt += "<|start_header_id|>user<|end_header_id|>\n\n";
    prompt += toUtf8(path);
    prompt += "  (" + humanSize(size) + ")";
    prompt += "<|eot_id|><|start_header_id|>assistant<|end_header_id|>\n\n";

    std::string reply;
    try {
        if (!generate(prompt, kDescribeGrammar, 90, &reply)) return false;
    } catch (...) {
        return false;
    }

    // Trim whatever whitespace the template left around it.
    while (!reply.empty() && (reply.front() == ' ' || reply.front() == '\n')) {
        reply.erase(reply.begin());
    }
    while (!reply.empty() && (reply.back() == ' ' || reply.back() == '\n')) reply.pop_back();
    if (reply.empty()) return false;

    *out = toWide(reply);
    return true;
}

}  // namespace filo
