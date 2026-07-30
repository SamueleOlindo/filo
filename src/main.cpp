// filo - milestone 2: a persistent index, updated incrementally.
//
// Usage:
//   filo.exe                      load the C: snapshot and catch up on the journal
//   filo.exe D:                   another volume
//   filo.exe C: fattura           load, catch up, search
//   filo.exe C: fattura --rescan  ignore the snapshot and rescan the MFT

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <string>
#include <thread>
#include <vector>

#include <sqlite3.h>

#include <atomic>
#include <mutex>
#include <unordered_set>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "content/chunker.h"
#include "content/normalize.h"
#include "extract/epub.h"
#include "extract/html.h"
#include "extract/ooxml.h"
#include "extract/rtf.h"
#include "extract/zip.h"
#if FILO_HAVE_PDFIUM
#include "extract/pdf.h"
#endif
#include "content/reader.h"
#include "content/selector.h"
#include "ntfs/volume.h"
#include "store/query.h"
#include "worker/supervisor.h"
#include "worker/worker_main.h"
#include "vector/vector_store.h"
#if FILO_HAVE_LLAMA
#include "search/query_model.h"
#include "vector/embedder.h"
#include "vector/eval_corpus.h"
#endif
#include "index/file_index.h"
#include "store/content_db.h"
#include "ui/webview_window.h"
#include "ntfs/mft_enumerator.h"
#include "ntfs/usn_reader.h"
#include "reclaim/classify.h"
#include "reclaim/duplicates.h"
#include "reclaim/paths.h"
#include "reclaim/recycle.h"
#include "reclaim/scan.h"
#include "search/matcher.h"
#include "search/query_plan.h"
#include "search/searcher.h"

namespace {

// The Windows console does not print wchar_t reliably: we convert to UTF-8 and
// use the narrow APIs, after putting the console in codepage 65001.
std::string toUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
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

// Thousands separator, to make large counts readable.
std::string groupDigits(uint64_t value) {
    std::string digits = std::to_string(value);
    std::string out;
    int count = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (count && count % 3 == 0) out.push_back('.');
        out.push_back(*it);
        ++count;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

// The snapshot lives in %LOCALAPPDATA%\filo\index-C.bin
std::wstring snapshotPath(wchar_t drive) {
    wchar_t base[MAX_PATH] = {};
    const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    std::wstring directory = (len > 0 && len < MAX_PATH) ? base : L".";
    directory += L"\\filo";
    CreateDirectoryW(directory.c_str(), nullptr);

    std::wstring path = directory + L"\\index-";
    path += drive;
    path += L".bin";
    return path;
}

// Startup chatter, printed only when a person is there to read it.
//
// The index load says how many records it found, whether the journal was
// readable and how much memory it took. Useful after typing a command; debris
// after double-clicking an icon, and it lands in whatever terminal the program
// was started from, which may not be ours to write to.
bool g_quiet = false;

void say(const char* format, ...) {
    if (g_quiet) return;
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
}

// Where the models live, without the user having to be told.
//
// Two folders are searched: beside the executable, so a portable copy carries
// its own models and keeps the single-folder promise, then the data directory,
// where anything downloaded after installation would land.
//
// The first version demanded the exact names model.gguf and question.gguf and
// printed "model cannot be loaded" when they were absent, which is a true
// statement and a useless one: the user has a perfectly good model sitting in
// the same folder under its real name. So the names are tried first and then
// ANY .gguf is accepted, sorted by size — an embedding model is a hundred
// megabytes and a generative one is a couple of gigabytes, and nothing else in
// that folder could be confused for either.
struct FoundModels {
    std::wstring embedding;
    std::wstring question;
};

std::vector<std::wstring> modelFolders() {
    std::vector<std::wstring> folders;
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        const std::wstring full = exePath;
        const size_t cut = full.find_last_of(L'\\');
        if (cut != std::wstring::npos) folders.push_back(full.substr(0, cut));
    }
    wchar_t base[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (length > 0 && length < MAX_PATH) folders.push_back(std::wstring(base) + L"\\filo");
    return folders;
}

FoundModels findModels() {
    FoundModels found;

    // Anything at least this big is a model that generates text, not one that
    // only places a sentence in space.
    constexpr uint64_t kGenerativeFloor = 700ull << 20;

    for (const std::wstring& folder : modelFolders()) {
        for (const wchar_t* name : {L"\\model.gguf", L"\\question.gguf"}) {
            const std::wstring path = folder + name;
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            if (name[1] == L'm' && found.embedding.empty()) found.embedding = path;
            if (name[1] == L'q' && found.question.empty()) found.question = path;
        }

        WIN32_FIND_DATAW entry{};
        const HANDLE search = FindFirstFileW((folder + L"\\*.gguf").c_str(), &entry);
        if (search == INVALID_HANDLE_VALUE) continue;
        do {
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const uint64_t size =
                (static_cast<uint64_t>(entry.nFileSizeHigh) << 32) | entry.nFileSizeLow;
            const std::wstring path = folder + L"\\" + entry.cFileName;
            if (size >= kGenerativeFloor) {
                if (found.question.empty()) found.question = path;
            } else if (found.embedding.empty()) {
                found.embedding = path;
            }
        } while (FindNextFileW(search, &entry));
        FindClose(search);
    }
    return found;
}

// The CONTENT index lives beside the name snapshot but in a separate file: two
// structures with different lifetimes and formats.
std::wstring contentDbPath(wchar_t drive) {
    wchar_t base[MAX_PATH] = {};
    const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    std::wstring directory = (len > 0 && len < MAX_PATH) ? base : L".";
    directory += L"\\filo";
    CreateDirectoryW(directory.c_str(), nullptr);

    std::wstring path = directory + L"\\content-";
    path += drive;
    path += L".db";
    return path;
}

double secondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// The "user documents" folders, asked of the system rather than built by hand:
// on Windows the real names on disk can differ from the ones shown, and the
// folders may have been moved somewhere else.
struct UserArea {
    const wchar_t* label;
    std::wstring prefix;
    uint64_t byKind[16] = {};
    uint64_t total = 0;
};

std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &raw))) return {};
    std::wstring path = raw ? raw : L"";
    CoTaskMemFree(raw);
    return filo::toLowerCopy(path);
}

std::vector<UserArea> buildUserAreas() {
    std::vector<UserArea> areas;
    auto add = [&areas](const wchar_t* label, std::wstring prefix) {
        if (!prefix.empty()) areas.push_back(UserArea{label, std::move(prefix)});
    };
    add(L"Documents", knownFolder(FOLDERID_Documents));
    add(L"Downloads", knownFolder(FOLDERID_Downloads));
    add(L"Desktop", knownFolder(FOLDERID_Desktop));
    add(L"Pictures", knownFolder(FOLDERID_Pictures));

    // OneDrive has no stable KNOWNFOLDERID: it arrives as an environment variable.
    wchar_t oneDrive[MAX_PATH] = {};
    const DWORD len = GetEnvironmentVariableW(L"OneDrive", oneDrive, MAX_PATH);
    if (len > 0 && len < MAX_PATH) add(L"OneDrive", filo::toLowerCopy(oneDrive));

    return areas;
}

bool fullScan(wchar_t drive, filo::FileIndex* index) {
    filo::MftEnumerator enumerator(drive);
    std::wstring error;
    if (!enumerator.enumerate(index, &error)) {
        std::printf("ERROR: %s\n", toUtf8(error).c_str());
        return false;
    }
    const auto& stats = enumerator.stats();
    std::printf("MFT scan: %s records in %.2f s (%.1f MB read)\n",
                groupDigits(stats.recordCount).c_str(), stats.elapsedSeconds,
                stats.bytesRead / (1024.0 * 1024.0));
    return true;
}

constexpr size_t kMaxResults = 25;
constexpr int kSearchRuns = 5;

// --- the reader benchmark ---------------------------------------------------
//
// Measures what it really costs to pull the candidate files into memory,
// separating the OPEN from the READ. The distinction is not academic: across
// tens of thousands of small files almost all the time goes into opening and
// closing through the antivirus filters, and in that case adding threads helps
// while optimizing the transfer does not.
int runReadTest(const filo::FileIndex& index, const std::vector<unsigned>& threadCounts) {
    filo::SelectionStats selection;
    const std::vector<uint32_t> candidates = filo::selectCandidates(index, &selection);
    std::printf("candidates: %s files\n\n", groupDigits(candidates.size()).c_str());

    std::mutex sampleMutex;
    std::vector<std::wstring> samples;

    bool first = true;
    for (const unsigned threads : threadCounts) {
        filo::ContentReader::Options options;
        options.threads = threads;

        samples.clear();
        const filo::ReaderStats stats = filo::ContentReader::run(
            index, candidates, options,
            [&](const filo::ReadResult& result) {
                if (result.verdict == filo::GateVerdict::Pass) return;
                if (result.verdict == filo::GateVerdict::Empty) return;
                std::lock_guard<std::mutex> lock(sampleMutex);
                if (samples.size() >= 6) return;
                samples.push_back(std::wstring(filo::verdictName(result.verdict)) +
                                  L"  " + index.fullPath(result.slot));
            });

        const double mbRead = stats.bytesRead / (1024.0 * 1024.0);
        std::printf("%2u threads %6.2f s   %s files/s  %.0f MB/s%s\n", threads,
                    stats.elapsedSeconds,
                    groupDigits(static_cast<uint64_t>(stats.filesSeen /
                                                      stats.elapsedSeconds)).c_str(),
                    mbRead / stats.elapsedSeconds,
                    first ? "   (cold cache)" : "");
        std::printf("           cumulative wait across all threads: open %.1f s, "
                    "read %.1f s -> %.0f%% of the time is opening files\n",
                    stats.openSeconds, stats.readSeconds,
                    100.0 * stats.openSeconds /
                        (stats.openSeconds + stats.readSeconds + 1e-9));

        if (first) {
            std::printf("\n  gate verdicts\n");
            for (size_t v = 0; v < 8; ++v) {
                if (stats.byVerdict[v] == 0) continue;
                std::printf("    %-38s %10s\n",
                            toUtf8(filo::verdictName(static_cast<filo::GateVerdict>(v))).c_str(),
                            groupDigits(stats.byVerdict[v]).c_str());
            }
            std::printf("\n  encodings detected\n");
            for (size_t e = 0; e < 8; ++e) {
                if (stats.byEncoding[e] == 0) continue;
                std::printf("    %-38s %10s\n",
                            toUtf8(filo::encodingName(static_cast<filo::TextEncoding>(e))).c_str(),
                            groupDigits(stats.byEncoding[e]).c_str());
            }
            std::printf("\n  real format (from the magic bytes)\n");
            for (size_t k = 1; k < 16; ++k) {
                if (stats.byActualKind[k] == 0) continue;
                std::printf("    %-38s %10s\n",
                            toUtf8(filo::kindName(static_cast<filo::ContentKind>(k))).c_str(),
                            groupDigits(stats.byActualKind[k]).c_str());
            }
            if (!samples.empty()) {
                std::printf("\n  examples of files stopped\n");
                for (const std::wstring& sample : samples) {
                    std::printf("    %s\n", toUtf8(sample).c_str());
                }
            }
            std::printf("\n");
            first = false;
        }
        std::printf("\n");
    }
    return 0;
}

// --- checking the index container -------------------------------------------
//
// Before wiring the extractors to it we want proof that the container takes the
// load, and that contentless_delete really does the thing the whole incremental
// update rests on: deleting by rowid ALONE, without still having the original
// text.
int runDbTest() {
    wchar_t base[MAX_PATH] = {};
    const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    std::wstring directory = (len > 0 && len < MAX_PATH) ? base : L".";
    directory += L"\\filo";
    CreateDirectoryW(directory.c_str(), nullptr);
    const std::wstring path = directory + L"\\test-content.db";

    for (const wchar_t* suffix : {L"", L"-wal", L"-shm"}) {
        DeleteFileW((path + suffix).c_str());
    }

    filo::ContentDb db;
    std::wstring error;
    if (!db.open(path, false, &error)) {
        std::printf("open failed: %s\n", toUtf8(error).c_str());
        return 1;
    }

    filo::ContentWriter writer(db);
    if (!writer.prepare(&error)) {
        std::printf("prepare failed: %s\n", toUtf8(error).c_str());
        return 1;
    }

    constexpr int kDocuments = 20'000;
    constexpr int kChunksPerDocument = 5;

    // Synthetic text but with the right shape: common words that produce long
    // posting lists, plus one rare marker per document.
    static const char* kWords[] = {
        "contratto", "penale", "fattura", "cliente", "pagamento", "scadenza",
        "importo", "clausola", "risoluzione", "fornitore", "consegna", "garanzia",
    };

    std::printf("writing %s documents x %d chunks...\n",
                groupDigits(kDocuments).c_str(), kChunksPerDocument);

    const auto writeStart = std::chrono::steady_clock::now();
    if (!writer.beginBatch(&error)) {
        std::printf("begin failed: %s\n", toUtf8(error).c_str());
        return 1;
    }

    std::vector<int64_t> docIds;
    docIds.reserve(kDocuments);

    for (int d = 0; d < kDocuments; ++d) {
        std::string text;
        std::vector<filo::ChunkRecord> chunks;
        chunks.reserve(kChunksPerDocument);

        for (int c = 0; c < kChunksPerDocument; ++c) {
            const uint32_t offset = static_cast<uint32_t>(text.size());
            text += "documento ";
            text += std::to_string(d);
            text += " sezione ";
            text += std::to_string(c);
            text += ' ';
            for (int w = 0; w < 40; ++w) {
                text += kWords[(d + c * 7 + w * 3) % 12];
                text += ' ';
            }
            // unique marker: proves the document can be found again
            if (c == 0) {
                text += "zetaunico";
                text += std::to_string(d);
                text += ' ';
            }
            chunks.push_back({offset, static_cast<uint32_t>(text.size() - offset)});
        }

        filo::DocumentRecord document;
        document.volumeId = 1;
        document.frn = 0x0005000000000000ull + static_cast<uint64_t>(d);
        document.size = text.size();
        document.kind = 1;
        document.extractorVersion = 1;

        int64_t docId = 0;
        if (!writer.addDocument(document, text, chunks, &docId, &error)) {
            std::printf("insert failed at doc %d: %s\n", d, toUtf8(error).c_str());
            return 1;
        }
        docIds.push_back(docId);
    }

    if (!writer.commitBatch(&error)) {
        std::printf("commit failed: %s\n", toUtf8(error).c_str());
        return 1;
    }
    const double writeSeconds = secondsSince(writeStart);

    auto countMatches = [&db](const char* term) -> int64_t {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db.handle(),
                               "SELECT count(*) FROM ft WHERE ft MATCH ?", -1,
                               &statement, nullptr) != SQLITE_OK) {
            return -1;
        }
        sqlite3_bind_text(statement, 1, term, -1, SQLITE_STATIC);
        int64_t count = -1;
        if (sqlite3_step(statement) == SQLITE_ROW) count = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        return count;
    };

    const int64_t totalChunks = kDocuments * kChunksPerDocument;
    std::printf("  %s chunks written in %.2f s (%s chunks/s)\n",
                groupDigits(totalChunks).c_str(), writeSeconds,
                groupDigits(static_cast<uint64_t>(totalChunks / writeSeconds)).c_str());
    std::printf("  database: %.1f MB\n\n", db.fileBytes() / (1024.0 * 1024.0));

    const int64_t before = countMatches("contratto");
    std::printf("  \"contratto\" before the delete: %s chunks\n",
                groupDigits(before < 0 ? 0 : before).c_str());

    // Deleting 2,000 documents = 10,000 FTS5 rows by rowid alone.
    const auto deleteStart = std::chrono::steady_clock::now();
    if (!writer.beginBatch(&error)) return 1;
    for (int d = 0; d < 2'000; ++d) {
        if (!writer.deleteDocument(docIds[static_cast<size_t>(d)], &error)) {
            std::printf("delete failed: %s\n", toUtf8(error).c_str());
            return 1;
        }
    }
    if (!writer.commitBatch(&error)) return 1;
    const double deleteSeconds = secondsSince(deleteStart);

    const int64_t after = countMatches("contratto");
    std::printf("  10.000 rows deleted by rowid in %.2f s\n", deleteSeconds);
    std::printf("  \"contratto\" after the delete:  %s chunks\n\n",
                groupDigits(after < 0 ? 0 : after).c_str());

    // The marker of a deleted document must no longer exist; the marker of a
    // live one must. That is the proof that the delete is selective.
    const int64_t deletedMarker = countMatches("zetaunico5");
    const int64_t liveMarker = countMatches("zetaunico19000");
    std::printf("  marker of a deleted doc (expected 0): %lld\n",
                static_cast<long long>(deletedMarker));
    std::printf("  marker of a live doc    (expected 1): %lld\n",
                static_cast<long long>(liveMarker));

    const bool passed = deletedMarker == 0 && liveMarker == 1 && after < before;
    std::printf("\n  RESULT: %s\n", passed ? "OK" : "FAILED");
    return passed ? 0 : 1;
}

// --- diagnostic: extract one file and show the result -----------------------
//
// Answers the question "what did the extractor actually see in this document?",
// which the search results do not tell you.
int runExtractOne(const std::wstring& path) {
    std::vector<char> raw;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        std::printf("cannot open the file\n");
        return 1;
    }
    LARGE_INTEGER size;
    GetFileSizeEx(file, &size);
    raw.resize(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    ReadFile(file, raw.data(), static_cast<DWORD>(raw.size()), &got, nullptr);
    CloseHandle(file);
    raw.resize(got);

    const size_t slash = path.find_last_of(L'\\');
    const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const filo::ContentKind declared = filo::classifyByName(name);
    const filo::ContentKind actual =
        filo::detectByMagic(raw.data(), raw.size(), declared);

    std::printf("file      %s\n", toUtf8(name).c_str());
    std::printf("bytes     %s\n", groupDigits(raw.size()).c_str());
    std::printf("type      %s (declared: %s)\n",
                toUtf8(filo::kindName(actual)).c_str(),
                toUtf8(filo::kindName(declared)).c_str());

    std::string text;
#if FILO_HAVE_PDFIUM
    if (actual == filo::ContentKind::Pdf) {
        std::string extracted;
        int pages = 0, pagesWithText = 0;
        const filo::PdfResult result =
            filo::extractPdf(raw.data(), raw.size(), &extracted, &pages, &pagesWithText);
        static const char* kNames[] = {"ok", "not a pdf", "password protected",
                                       "images only: would need OCR", "failed"};
        std::printf("pages     %d (with text: %d)\n", pages, pagesWithText);
        std::printf("result    %s\n", kNames[static_cast<int>(result)]);
        if (result != filo::PdfResult::Ok) return 1;
        filo::normalizeToUtf8(extracted.data(), extracted.size(),
                              filo::TextEncoding::Utf8, filo::ContentKind::PlainText,
                              &text);
    } else
#endif
    if (actual == filo::ContentKind::Markup || actual == filo::ContentKind::RichText ||
        actual == filo::ContentKind::Ebook) {
        std::string extracted;
        bool ok = false;
        if (actual == filo::ContentKind::Markup) {
            ok = filo::extractHtml(raw.data(), raw.size(), &extracted);
        } else if (actual == filo::ContentKind::RichText) {
            ok = filo::extractRtf(raw.data(), raw.size(), &extracted);
        } else {
            ok = filo::extractEpub(raw.data(), raw.size(), &extracted);
        }
        std::printf("result    %s\n", ok ? "ok" : "no text");
        if (!ok) return 1;
        filo::normalizeToUtf8(extracted.data(), extracted.size(),
                              filo::TextEncoding::Utf8, filo::ContentKind::PlainText,
                              &text);
    } else if (actual == filo::ContentKind::OfficeXml ||
               actual == filo::ContentKind::OpenDocument) {
        filo::ZipReader zip;
        if (zip.open(raw.data(), raw.size())) {
            std::printf("archive   %s entries\n",
                        groupDigits(zip.entries().size()).c_str());
        }
        std::string extracted;
        const filo::OfficeResult result =
            filo::extractOfficeXml(raw.data(), raw.size(), &extracted);
        static const char* kNames[] = {"ok", "not an archive", "password protected",
                                       "unsupported", "empty"};
        std::printf("result    %s\n", kNames[static_cast<int>(result)]);
        if (result != filo::OfficeResult::Ok) return 1;
        filo::normalizeToUtf8(extracted.data(), extracted.size(),
                              filo::TextEncoding::Utf8, filo::ContentKind::PlainText,
                              &text);
    } else {
        const filo::TextEncoding encoding =
            filo::detectEncoding(raw.data(), raw.size());
        std::printf("encoding  %s\n", toUtf8(filo::encodingName(encoding)).c_str());
        filo::normalizeToUtf8(raw.data(), raw.size(), encoding, actual, &text);
    }

    const std::vector<filo::ChunkRecord> chunks = filo::chunkText(text);
    std::printf("text      %s characters in %s chunks\n\n",
                groupDigits(text.size()).c_str(), groupDigits(chunks.size()).c_str());

    const size_t show = std::min<size_t>(text.size(), 1400);
    std::printf("%.*s\n", static_cast<int>(show), text.c_str());
    if (text.size() > show) std::printf("\n[... %s more characters]\n",
                                        groupDigits(text.size() - show).c_str());
    return 0;
}

#if FILO_HAVE_LLAMA
// --- the semantic search check ----------------------------------------------
//
// Checks that the model really says what we expect: sentences that talk about
// the same thing in different words must come out close, sentences with similar
// words but a different meaning must come out far apart. If that relation does
// not hold, the whole of layer 3 is useless.
int runEmbedTest(const std::wstring& modelPath) {
    filo::Embedder embedder;
    std::wstring error;

    // The prefixes apply only to the E5 family and have to be asked for
    // explicitly: putting them on a model that does not want them quietly makes
    // the results worse.
    filo::Embedder::Options options;
    if (GetEnvironmentVariableW(L"FILO_E5_PREFIX", nullptr, 0) > 0) {
        options.passagePrefix = "passage: ";
        options.queryPrefix = "query: ";
    }

    const auto start = std::chrono::steady_clock::now();
    if (!embedder.load(modelPath, options, &error)) {
        std::printf("load failed: %s\n", toUtf8(error).c_str());
        return 1;
    }
    std::printf("model loaded in %.2f s · %u dimensions\n\n",
                secondsSince(start), embedder.dimensions());

    const char* frasi[] = {
        "la penale per il ritardo nella consegna",
        "la clausola risarcitoria in caso di inadempimento",
        "sanzione pecuniaria prevista dal contratto",
        "ricetta della pasta alla norma con melanzane",
        "the penalty clause for late delivery",
    };
    constexpr int kCount = 5;

    const uint32_t dims = embedder.dimensions();
    std::vector<std::vector<float>> vettori(kCount, std::vector<float>(dims));

    const auto embedStart = std::chrono::steady_clock::now();
    for (int i = 0; i < kCount; ++i) {
        if (!embedder.embedPassage(frasi[i], vettori[i].data())) {
            std::printf("embedding failed on sentence %d\n", i);
            return 1;
        }
    }
    const double ms = secondsSince(embedStart) * 1000.0 / kCount;
    std::printf("%.1f ms per sentence\n\n", ms);

    std::printf("similarity with: \"%s\"\n\n", frasi[0]);
    for (int i = 1; i < kCount; ++i) {
        double dot = 0;
        for (uint32_t d = 0; d < dims; ++d) dot += vettori[0][d] * vettori[i][d];
        std::printf("  %.3f   %s\n", dot, frasi[i]);
    }

    // --- the RANKING test, with distractors ---
    //
    // The similarity between two sentences on its own does not say whether a
    // model is usable: what counts is whether, put in front of many texts, it
    // brings the right ones to the top. A model that gives everything 0.96 can
    // still rank well; one that ranks badly is useless even when the numbers
    // look reasonable. That is exactly what gets measured here.
    struct Voce { const char* testo; bool pertinente; };
    static const Voce corpus[] = {
        {"in caso di ritardo si applica una clausola risarcitoria", true},
        {"il fornitore versera' una somma a titolo di indennizzo per l'inadempimento", true},
        {"sanzione pecuniaria prevista in caso di mancato rispetto dei termini", true},
        {"e' dovuto un importo forfettario per ogni giorno di ritardo nella consegna", true},
        {"the supplier shall pay liquidated damages for late delivery", true},
        {"risarcimento del danno derivante da inadempimento contrattuale", true},
        {"soffriggere la cipolla in padella con un filo d'olio", false},
        {"il volo parte dal terminal 3 alle sette del mattino", false},
        {"per installare le dipendenze esegui npm install nella cartella", false},
        {"la partita si e' conclusa con un pareggio all'ultimo minuto", false},
        {"le previsioni indicano pioggia debole sulla costa adriatica", false},
        {"configura il server impostando la porta nel file di ambiente", false},
        {"la garanzia sul prodotto copre i difetti di fabbricazione", false},
        {"il pagamento avviene tramite bonifico entro trenta giorni", false},
    };
    constexpr int kCorpus = static_cast<int>(sizeof(corpus) / sizeof(corpus[0]));
    const char* domanda = "penale per il ritardo";

    std::vector<float> vDomanda(dims);
    if (!embedder.embedQuery(domanda, vDomanda.data())) return 1;

    std::vector<std::pair<double, int>> classifica;
    std::vector<float> vDoc(dims);
    for (int i = 0; i < kCorpus; ++i) {
        if (!embedder.embedPassage(corpus[i].testo, vDoc.data())) return 1;
        double dot = 0;
        for (uint32_t d = 0; d < dims; ++d) dot += vDomanda[d] * vDoc[d];
        classifica.emplace_back(dot, i);
    }
    std::sort(classifica.begin(), classifica.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    int corretti = 0;
    const int pertinentiTotali = 6;
    std::printf("\n\nranking for \"%s\" (6 relevant out of %d)\n\n", domanda, kCorpus);
    for (int i = 0; i < kCorpus; ++i) {
        const Voce& v = corpus[classifica[i].second];
        if (i < pertinentiTotali && v.pertinente) ++corretti;
        std::printf("  %2d. %.3f  %s %.60s\n", i + 1, classifica[i].first,
                    v.pertinente ? "[yes]" : "[no] ", v.testo);
    }
    std::printf("\n  precision in the top %d: %d/%d\n", pertinentiTotali, corretti,
                pertinentiTotali);

    // --- cost on text of REAL length ---
    //
    // The sentences above are ten words long; a real chunk is around 1,400
    // bytes of Italian, a hundred times as much. The cost of a transformer
    // model is not linear in the length, so measuring on short sentences and
    // then multiplying by 220,000 would give a completely wrong estimate.
    //
    // The figure was 2,000 bytes while the chunker cut by bytes. It now cuts to
    // a token budget, so the same 448 tokens come out as fewer bytes of Italian
    // — and as far fewer of anything the tokenizer handles less well.
    std::string blocco;
    while (blocco.size() < 1400) {
        blocco += "Il presente contratto disciplina il rapporto tra le parti e definisce "
                  "termini, modalita' di pagamento, penali per il ritardo e condizioni di "
                  "risoluzione anticipata del rapporto contrattuale in essere. ";
    }
    std::vector<float> vettoreBlocco(dims);

    // One throwaway pass to warm the caches before measuring.
    embedder.embedPassage(blocco, vettoreBlocco.data());

    constexpr int kRuns = 5;
    const auto blockStart = std::chrono::steady_clock::now();
    for (int i = 0; i < kRuns; ++i) embedder.embedPassage(blocco, vettoreBlocco.data());
    const double blockMs = secondsSince(blockStart) * 1000.0 / kRuns;

    std::printf("\nrealistic chunk (%s bytes): %.0f ms\n",
                groupDigits(blocco.size()).c_str(), blockMs);
    const double totalMinutes = blockMs * 220000.0 / 60000.0;
    std::printf("estimate over 220.000 chunks: %.0f minutes (%.1f hours)\n",
                totalMinutes, totalMinutes / 60.0);
    return 0;
}
#endif

#if FILO_HAVE_LLAMA
// --- computing the embeddings over the whole index --------------------------
//
// The text of the chunks is NOT kept (the FTS5 table is contentless), so the
// files have to be read and split again. It is the same pipeline as indexing,
// with the model in place of the write to FTS5.
//
// Two details that are worth hours:
//  - documents are processed IN ORDER OF VALUE, so semantic search is useful on
//    contracts and invoices long before the run has finished;
//  - identical chunks are computed ONCE ONLY. On a real disk they are 13%:
//    backups of the same website, licence headers, boilerplate.
int runEmbedIndex(const filo::FileIndex& index, wchar_t drive,
                  const std::wstring& modelPath, int64_t chunkLimit) {
    filo::ContentDb db;
    std::wstring error;
    if (!db.open(contentDbPath(drive), true, &error)) {
        std::printf("no content index: run --index first\n");
        return 1;
    }

    filo::Embedder embedder;
    if (!embedder.load(modelPath, {}, &error)) {
        std::printf("cannot load the model: %s\n", toUtf8(error).c_str());
        return 1;
    }
    const uint32_t dims = embedder.dimensions();
    std::printf("model: %u dimensions\n\n", dims);

    const std::wstring vectorPath = contentDbPath(drive) + L".vec";

    filo::VectorStore store;
    store.configure(dims);

    // Pick up where an interrupted run left off.
    //
    // Until now the periodic save was writing a file nobody ever read back: a
    // crash five hours in — and one did happen, on a chunk that overflowed the
    // tokenizer's stack — threw away every vector and started from zero. The
    // identity check is what makes resuming safe rather than merely fast: it
    // refuses a file built with another model instead of appending vectors that
    // cannot be compared with the ones already there.
    const filo::VectorStore::Identity identity{
        filo::modelIdentity(modelPath), dims,
        static_cast<uint32_t>(filo::kChunkerVersion),
        static_cast<uint32_t>(db.metaInt("content_gen", 0))};
    std::unordered_set<uint32_t> alreadyEmbedded;
    {
        bool mismatch = false;
        std::wstring loadError;
        if (store.load(vectorPath, identity, &mismatch, &loadError)) {
            alreadyEmbedded.reserve(store.chunkIds().size() * 2);
            for (const uint32_t id : store.chunkIds()) alreadyEmbedded.insert(id);
            std::printf("resuming: %s vectors already computed\n\n",
                        groupDigits(store.chunkIds().size()).c_str());
        } else if (!mismatch) {
            std::printf("existing vector file unusable: %s\n", toUtf8(loadError).c_str());
            return 1;
        } else {
            store.configure(dims);   // load may have left a partial state
        }
    }

    // The same worker pool as indexing: PDFs are the highest-value documents
    // for semantic search and cannot be skipped.
    filo::ExtractorPool pool;
    std::wstring poolError;
    const bool havePool = pool.start(2, &poolError);
    if (!havePool) {
        std::printf("WARNING: workers not started, PDF and Office skipped (%s)\n",
                    toUtf8(poolError).c_str());
    }

    // The order: real documents first, then prose, then code, and last whatever
    // was generated by a machine.
    static const char* kOrder =
        "SELECT doc_id, frn, kind FROM doc ORDER BY "
        "CASE WHEN rank_penalty > 0 THEN 3 "
        "     WHEN kind IN (4,5,6,7,8,9) THEN 0 "
        "     WHEN kind = 2 THEN 2 ELSE 1 END, doc_id";

    sqlite3_stmt* docs = nullptr;
    if (sqlite3_prepare_v2(db.handle(), kOrder, -1, &docs, nullptr) != SQLITE_OK) {
        std::printf("query failed\n");
        return 1;
    }
    sqlite3_stmt* chunkQuery = nullptr;
    sqlite3_prepare_v2(db.handle(),
                       "SELECT chunk_id FROM chunk WHERE doc_id = ? ORDER BY chunk_id",
                       -1, &chunkQuery, nullptr);

    std::unordered_map<uint64_t, uint32_t> visti;   // text hash -> position
    std::vector<float> vettore(dims);
    std::vector<char> raw;

    uint64_t documenti = 0, blocchi = 0, riusati = 0, saltati = 0;
    uint64_t ripresi = 0, illeggibili = 0;

    // Where the time actually goes.
    //
    // The isolated benchmark says 24 ms per chunk and the pipeline delivers 91.
    // Guessing which stage owns the difference is how the last wrong estimate
    // got made; these three counters answer it instead. They cost one clock
    // read per stage, which against a 24 ms inference is free.
    double msLoadingText = 0, msChunking = 0, msInference = 0, msBookkeeping = 0;
    uint64_t bytesEmbedded = 0;

    const auto start = std::chrono::steady_clock::now();
    auto ultimoAvviso = start;
    auto stageMark = start;

    // Milliseconds since the last call, and reset. Sequential code, so a single
    // moving mark is enough to attribute every interval to exactly one stage.
    auto sinceMark = [&stageMark]() {
        const auto now = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - stageMark).count();
        stageMark = now;
        return ms;
    };

    while (sqlite3_step(docs) == SQLITE_ROW) {
        const int64_t docId = sqlite3_column_int64(docs, 0);
        const uint64_t frn = static_cast<uint64_t>(sqlite3_column_int64(docs, 1));
        const auto kind = static_cast<filo::ContentKind>(sqlite3_column_int(docs, 2));

        const uint32_t slot = index.indexOfFrn(frn);
        if (slot == filo::kNoParent || index.isDeleted(slot)) { ++saltati; continue; }

        // The chunk ids come first, from the database, because they are what
        // tells us whether this document still needs any work. Asking before
        // opening the file means a resumed run walks past everything it already
        // did at the speed of a query instead of re-reading and re-extracting
        // it — which, for a disk of PDFs, is the difference between seconds and
        // an hour spent to discover there was nothing to do.
        std::vector<int64_t> chunkIds;
        sqlite3_reset(chunkQuery);
        sqlite3_bind_int64(chunkQuery, 1, docId);
        while (sqlite3_step(chunkQuery) == SQLITE_ROW) {
            chunkIds.push_back(sqlite3_column_int64(chunkQuery, 0));
        }
        if (chunkIds.empty()) { ++saltati; continue; }

        bool complete = true;
        for (const int64_t id : chunkIds) {
            if (!alreadyEmbedded.count(static_cast<uint32_t>(id))) { complete = false; break; }
        }
        if (complete) { ++ripresi; continue; }
        msBookkeeping += sinceMark();

        // The text has to reproduce EXACTLY what indexing produced, or the
        // vectors end up attached to the wrong rows.
        //
        // For PDFs and Office documents that text is already in the database,
        // put there when the file was indexed. Reading it back is a row lookup;
        // the old path re-read the file and sent it through a worker process
        // again, which is most of why the GPU spent two thirds of this pass
        // waiting.
        std::string text;
        if (!filo::readDocumentText(db, docId, &text) || text.empty()) {
            const bool inProcess = kind == filo::ContentKind::PlainText ||
                                   kind == filo::ContentKind::Code;

            const std::wstring path = index.fullPath(slot);
            filo::FileFacts facts;
            if (!filo::readFileFacts(path.c_str(), &facts) ||
                filo::isCloudPlaceholder(facts.attributes) || facts.size == 0 ||
                facts.size > (16ull << 20)) {
                ++saltati;
                continue;
            }

            HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING,
                                      FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_NO_RECALL,
                                      nullptr);
            if (file == INVALID_HANDLE_VALUE) { ++saltati; continue; }
            raw.resize(static_cast<size_t>(facts.size));
            DWORD got = 0;
            ReadFile(file, raw.data(), static_cast<DWORD>(raw.size()), &got, nullptr);
            CloseHandle(file);
            if (got == 0) { ++saltati; continue; }
            raw.resize(got);

            if (inProcess) {
                const filo::TextEncoding encoding =
                    filo::detectEncoding(raw.data(), raw.size());
                if (!filo::normalizeToUtf8(raw.data(), raw.size(), encoding, kind, &text)) {
                    ++saltati;
                    continue;
                }
            } else if (havePool) {
                // Only reached for a document indexed before stored text
                // existed. Same isolated worker as indexing, never a
                // simplified path, or the chunk boundaries would move.
                if (pool.extract(kind, raw.data(), raw.size(), &text) !=
                        filo::WorkerStatus::Ok || text.empty()) {
                    ++saltati;
                    continue;
                }
            } else {
                ++saltati;
                continue;
            }
        }
        msLoadingText += sinceMark();

        const std::vector<filo::ChunkRecord> chunks = filo::chunkText(text);
        msChunking += sinceMark();

        // The chunk ids were read from the database before the file was even
        // opened, so the vector hangs off the SAME row the text index uses. If
        // the counts disagree the file changed after indexing: skipping beats
        // attaching vectors to the wrong rows.
        if (chunkIds.size() != chunks.size()) { ++saltati; continue; }

        for (size_t i = 0; i < chunks.size(); ++i) {
            const auto chunkId = static_cast<uint32_t>(chunkIds[i]);
            if (alreadyEmbedded.count(chunkId)) continue;   // done in an earlier run

            const char* begin = text.data() + chunks[i].byteOffset;
            const uint64_t hash = XXH3_64bits(begin, chunks[i].byteLength);

            const auto seen = visti.find(hash);
            if (seen != visti.end()) {
                // Identical bytes: same vector, by definition.
                store.addExisting(chunkId, seen->second);
                ++riusati;
                ++blocchi;
                continue;
            }

            const std::string blocco(begin, chunks[i].byteLength);

            stageMark = std::chrono::steady_clock::now();
            const bool embedded = embedder.embedPassage(blocco, vettore.data());
            msInference += sinceMark();
            bytesEmbedded += blocco.size();

            if (!embedded) {
                // Text the tokenizer cannot digest — a base64 payload, a
                // minified bundle. One of these used to end the whole run.
                ++illeggibili;
                continue;
            }
            visti.emplace(hash, static_cast<uint32_t>(store.size()));
            store.add(chunkId, vettore.data());
            ++blocchi;
        }
        ++documenti;
        if (chunkLimit > 0 && static_cast<int64_t>(blocchi) >= chunkLimit) {
            std::printf("\n  stopping at the %s chunk limit\n",
                        groupDigits(chunkLimit).c_str());
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - ultimoAvviso).count() > 10.0) {
            ultimoAvviso = now;
            const double elapsed = secondsSince(start);
            const double total = elapsed * 1000.0;
            std::printf("  %s docs · %s chunks (%s reused) · %.0f chunks/s"
                        "  [infer %.0f%% · text %.0f%% · chunk %.0f%% · other %.0f%%]\n",
                        groupDigits(documenti).c_str(), groupDigits(blocchi).c_str(),
                        groupDigits(riusati).c_str(), blocchi / elapsed,
                        100.0 * msInference / total, 100.0 * msLoadingText / total,
                        100.0 * msChunking / total,
                        100.0 * (total - msInference - msLoadingText - msChunking) / total);
            // Without this the progress sits in the buffer and whoever is
            // watching the log sees an empty file for hours.
            std::fflush(stdout);

            // Checkpoint. Appends only what is new, so it costs what it says
            // rather than rewriting the whole store every ten seconds.
            std::wstring flushError;
            if (!store.flush(vectorPath, identity, &flushError)) {
                std::printf("  checkpoint failed: %s\n", toUtf8(flushError).c_str());
            }
            stageMark = std::chrono::steady_clock::now();   // reporting is not a stage
        }
    }
    sqlite3_finalize(docs);
    sqlite3_finalize(chunkQuery);

    if (!store.flush(vectorPath, identity, &error)) {
        std::printf("save failed: %s\n", toUtf8(error).c_str());
        return 1;
    }

    const double seconds = secondsSince(start);
    std::printf("\n  documents      %10s\n", groupDigits(documenti).c_str());
    std::printf("  chunks         %10s\n", groupDigits(blocchi).c_str());
    std::printf("  reused         %10s  (%.1f%%)\n", groupDigits(riusati).c_str(),
                100.0 * riusati / (blocchi ? blocchi : 1));
    if (ripresi) {
        std::printf("  already done   %10s  (skipped without reading)\n",
                    groupDigits(ripresi).c_str());
    }
    if (illeggibili) {
        std::printf("  untokenizable  %10s  (base64, minified bundles)\n",
                    groupDigits(illeggibili).c_str());
    }
    std::printf("  skipped        %10s\n", groupDigits(saltati).c_str());
    std::printf("  time           %10.1f s  (%.0f chunks/s)\n", seconds,
                blocchi / (seconds > 0 ? seconds : 1));
    std::printf("  memory         %10.1f MB\n", store.memoryBytes() / (1024.0 * 1024.0));
    std::printf("  total vectors  %10s\n", groupDigits(store.size()).c_str());

    const double totalMs = seconds * 1000.0;
    const uint64_t inferred = blocchi - riusati;
    std::printf("\n  where the time went\n");
    std::printf("    inference    %8.1f s  (%4.1f%%)  %6.1f ms per chunk embedded\n",
                msInference / 1000.0, 100.0 * msInference / totalMs,
                inferred ? msInference / inferred : 0.0);
    std::printf("    loading text %8.1f s  (%4.1f%%)\n", msLoadingText / 1000.0,
                100.0 * msLoadingText / totalMs);
    std::printf("    chunking     %8.1f s  (%4.1f%%)\n", msChunking / 1000.0,
                100.0 * msChunking / totalMs);
    std::printf("    bookkeeping  %8.1f s  (%4.1f%%)\n", msBookkeeping / 1000.0,
                100.0 * msBookkeeping / totalMs);
    std::printf("    average chunk %7.0f bytes\n",
                inferred ? static_cast<double>(bytesEmbedded) / inferred : 0.0);

    // Inside a single embed() call. "inference is 100%" is true and tells us
    // nothing; this says which part of inference.
    const filo::Embedder::Stats& es = embedder.stats();
    const double perCall = es.calls ? 1.0 / static_cast<double>(es.calls) : 0.0;
    std::printf("\n  inside embed()   %8s calls, %s failed, %s rejected, %s truncated\n",
                groupDigits(es.calls).c_str(), groupDigits(es.failed).c_str(),
                groupDigits(es.rejected).c_str(), groupDigits(es.truncated).c_str());
    std::printf("    guard        %8.1f s  %6.2f ms/call\n", es.msGuard / 1000.0,
                es.msGuard * perCall);
    std::printf("    tokenize     %8.1f s  %6.2f ms/call\n", es.msTokenize / 1000.0,
                es.msTokenize * perCall);
    std::printf("    build batch  %8.1f s  %6.2f ms/call\n", es.msBatch / 1000.0,
                es.msBatch * perCall);
    std::printf("    llama_encode %8.1f s  %6.2f ms/call\n", es.msEncode / 1000.0,
                es.msEncode * perCall);
    std::printf("    read out     %8.1f s  %6.2f ms/call\n", es.msReadOut / 1000.0,
                es.msReadOut * perCall);

    const double accounted = es.msGuard + es.msTokenize + es.msBatch + es.msEncode +
                             es.msReadOut + msLoadingText + msChunking + msBookkeeping;
    std::printf("    UNACCOUNTED  %8.1f s  (%.1f%% of wall time)\n",
                (totalMs - accounted) / 1000.0, 100.0 * (totalMs - accounted) / totalMs);

    std::printf("\n  tokens per chunk: %.0f average, %u longest (window %u)\n",
                es.calls ? static_cast<double>(es.tokensTotal) / es.calls : 0.0,
                es.tokensMax, embedder.contextTokens());
    std::printf("  encode cost by sequence length\n");
    for (int i = 0; i < 5; ++i) {
        const filo::Embedder::Stats::Bucket& b = es.buckets[i];
        if (b.calls == 0) continue;
        std::printf("    %-9s %9s calls  %6.1f ms each\n",
                    filo::Embedder::Stats::bucketName(i), groupDigits(b.calls).c_str(),
                    b.msEncode / static_cast<double>(b.calls));
    }
    return 0;
}

// --- measuring QUALITY with standard metrics --------------------------------
//
// Two measures, both used in the information retrieval literature:
//
//   MRR  (mean reciprocal rank) — 1/position of the FIRST relevant result,
//        averaged over the queries. It says how high the first good answer
//        lands: 1.0 means always first, 0.5 means always second. It is the
//        metric that matches what the user perceives.
//
//   P@5  (precision at 5) — how many of the first five results are really
//        relevant. It says whether the page of results is clean.
//
// A single test on four sentences does not tell a good model from a mediocre
// one: it takes a battery of queries, with distractors from the same domain.
int runModelEval(const std::wstring& modelPath) {
    filo::Embedder embedder;
    filo::Embedder::Options options;
    if (GetEnvironmentVariableW(L"FILO_E5_PREFIX", nullptr, 0) > 0) {
        options.passagePrefix = "passage: ";
        options.queryPrefix = "query: ";
    }
    std::wstring error;
    if (!embedder.load(modelPath, options, &error)) {
        std::printf("load failed: %s\n", toUtf8(error).c_str());
        return 1;
    }
    const uint32_t dims = embedder.dimensions();

    // The passages are computed once and reused for every query.
    std::vector<std::vector<float>> passaggi(filo::kEvalPassageCount,
                                             std::vector<float>(dims));
    for (int i = 0; i < filo::kEvalPassageCount; ++i) {
        if (!embedder.embedPassage(filo::kEvalPassages[i].text, passaggi[i].data())) {
            std::printf("embedding failed on passage %d\n", i);
            return 1;
        }
    }

    double sommaRR = 0;
    double sommaP5 = 0;
    int peggiorRango = 0;
    const char* peggiorDomanda = "";

    std::printf("%-42s %6s %6s\n", "query", "rank", "P@5");
    std::printf("%-42s %6s %6s\n", "------------------------------------------",
                "-----", "-----");

    std::vector<float> vDomanda(dims);
    for (int q = 0; q < filo::kEvalQueryCount; ++q) {
        const filo::EvalQuery& domanda = filo::kEvalQueries[q];
        if (!embedder.embedQuery(domanda.text, vDomanda.data())) return 1;

        std::vector<std::pair<double, int>> classifica;
        classifica.reserve(filo::kEvalPassageCount);
        for (int i = 0; i < filo::kEvalPassageCount; ++i) {
            double dot = 0;
            for (uint32_t d = 0; d < dims; ++d) dot += vDomanda[d] * passaggi[i][d];
            classifica.emplace_back(dot, i);
        }
        std::sort(classifica.begin(), classifica.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });

        int primoPertinente = 0;
        int pertinentiNeiPrimi5 = 0;
        for (int rango = 0; rango < filo::kEvalPassageCount; ++rango) {
            const bool pertinente =
                filo::kEvalPassages[classifica[rango].second].topic == domanda.topic;
            if (pertinente && primoPertinente == 0) primoPertinente = rango + 1;
            if (rango < 5 && pertinente) ++pertinentiNeiPrimi5;
        }

        const double rr = primoPertinente ? 1.0 / primoPertinente : 0.0;
        const double p5 = pertinentiNeiPrimi5 / 5.0;
        sommaRR += rr;
        sommaP5 += p5;
        if (primoPertinente > peggiorRango) {
            peggiorRango = primoPertinente;
            peggiorDomanda = domanda.text;
        }

        std::printf("%-42.42s %6d %5.0f%%\n", domanda.text, primoPertinente, p5 * 100);
    }

    std::printf("\n  MRR  %.3f   (1.0 = the right answer always in first place)\n",
                sommaRR / filo::kEvalQueryCount);
    std::printf("  P@5  %.3f   (share of relevant results in the top five)\n",
                sommaP5 / filo::kEvalQueryCount);
    std::printf("\n  worst case: rank %d on \"%s\"\n", peggiorRango, peggiorDomanda);
    return 0;
}

// --- measuring the speed levers ---------------------------------------------
//
// Two variables, measured on text of realistic length: how many chunks to
// process together and how many threads to use. They are the only two levers
// that cost nothing in behaviour: same vectors, same result.
int runEmbedBench(const std::wstring& modelPath) {
    // 1,400 bytes, which is what 448 tokens of Italian comes to under chunker
    // version 3. It was 2,000 while the chunker cut by bytes; measuring a length
    // the indexer no longer produces would put the levers in the wrong place.
    std::string blocco;
    while (blocco.size() < 1400) {
        blocco += "Il presente contratto disciplina il rapporto tra le parti e definisce "
                  "termini, modalita' di pagamento, penali per il ritardo e condizioni di "
                  "risoluzione anticipata del rapporto contrattuale in essere. ";
    }

    const unsigned core = std::max(1u, std::thread::hardware_concurrency());
    std::printf("test chunk: %s bytes · %u hardware threads\n\n",
                groupDigits(blocco.size()).c_str(), core);
    std::printf("%8s %8s %12s %12s %10s\n", "batch", "threads", "ms/pass",
                "ms/chunk", "speedup");

    double baseline = 0;
    for (const uint32_t gruppo : {1u, 2u, 4u, 8u}) {
        for (const int thread : {static_cast<int>(core / 2), static_cast<int>(core)}) {
            filo::Embedder embedder;
            filo::Embedder::Options options;
            options.batchSequences = gruppo;
            options.threads = thread;
            std::wstring error;
            if (!embedder.load(modelPath, options, &error)) {
                std::printf("%8u %8d   load failed\n", gruppo, thread);
                continue;
            }

            const uint32_t dims = embedder.dimensions();
            std::vector<std::string> testi(gruppo, blocco);
            std::vector<float> vettori(static_cast<size_t>(gruppo) * dims);

            embedder.embedPassages(testi, vettori.data());   // warm-up

            constexpr int kRuns = 3;
            const auto start = std::chrono::steady_clock::now();
            for (int r = 0; r < kRuns; ++r) {
                if (!embedder.embedPassages(testi, vettori.data())) {
                    std::printf("%8u %8d   pass failed\n", gruppo, thread);
                    break;
                }
            }
            const double perPass = secondsSince(start) * 1000.0 / kRuns;
            const double perChunk = perPass / gruppo;
            if (baseline == 0) baseline = perChunk;

            std::printf("%8u %8d %12.0f %12.0f %9.2fx\n", gruppo, thread, perPass,
                        perChunk, baseline / perChunk);
        }
    }

    // --- how the cost scales with the LENGTH of the text ---
    //
    // The only lever left with a potentially large gain, and it has a price:
    // truncating means the tail of the chunk never reaches the vector. The
    // attention mechanism costs quadratically in tokens, the rest linearly: if
    // the quadratic part dominates, halving is worth more than double and the
    // price may be worth paying. If the linear part dominates, it is worth
    // nothing.
    std::printf("\n\n%10s %12s %12s %14s\n", "bytes", "ms/chunk", "vs 2000", "hrs on 61.622");

    filo::Embedder embedder;
    filo::Embedder::Options options;
    options.threads = static_cast<int>(core / 2);
    std::wstring error;
    if (!embedder.load(modelPath, options, &error)) return 1;

    std::vector<float> vettore(embedder.dimensions());
    double pieno = 0;
    for (const size_t lunghezza : {2000u, 1000u, 500u, 250u}) {
        const std::string parziale = blocco.substr(0, lunghezza);
        embedder.embedPassage(parziale, vettore.data());

        constexpr int kRuns = 3;
        const auto start = std::chrono::steady_clock::now();
        for (int r = 0; r < kRuns; ++r) embedder.embedPassage(parziale, vettore.data());
        const double ms = secondsSince(start) * 1000.0 / kRuns;
        if (pieno == 0) pieno = ms;

        std::printf("%10s %12.0f %11.2fx %13.1f\n", groupDigits(lunghezza).c_str(), ms,
                    pieno / ms, ms * 61622.0 / 3600000.0);
    }
    return 0;
}
#endif

// --- measure the disk ------------------------------------------------------
//
// Phase 0 of reclaiming space: how big is everything, and how long does asking
// take. The answer to the second question decides whether this runs when the
// screen opens or has to live in the snapshot, and it should be measured on a
// real volume rather than assumed.
int runReclaimScan(const filo::FileIndex& index) {
    // Both ways, on the same volume, in the same run. The per-file path costs
    // 263 microseconds a call here — the antivirus filter stack, not the disk —
    // and the only way to know whether enumerating directories beats it is to
    // time them side by side.
    filo::ScanStats perFile;
    const std::vector<filo::FileFact> byFile =
        filo::collectFileFactsPerFile(index, &perFile);
    std::printf("  per file:      %8.2f s  %s files  %.1f GB\n", perFile.elapsedSeconds,
                groupDigits(perFile.measured).c_str(),
                perFile.totalBytes / (1024.0 * 1024.0 * 1024.0));

    filo::ScanStats stats;
    const std::vector<filo::FileFact> facts = filo::collectFileFacts(index, &stats);
    std::printf("  per directory: %8.2f s  %s files  %.1f GB   (%.1fx)\n\n",
                stats.elapsedSeconds, groupDigits(stats.measured).c_str(),
                stats.totalBytes / (1024.0 * 1024.0 * 1024.0),
                perFile.elapsedSeconds /
                    (stats.elapsedSeconds > 0 ? stats.elapsedSeconds : 1));

    std::printf("  records examined %12s\n", groupDigits(stats.examined).c_str());
    std::printf("  files measured   %12s\n", groupDigits(stats.measured).c_str());
    std::printf("  directories      %12s\n", groupDigits(stats.directories).c_str());
    std::printf("  cloud only       %12s  (reading would download them)\n",
                groupDigits(stats.cloudOnly).c_str());
    std::printf("  junctions/links  %12s  (counted where they point)\n",
                groupDigits(stats.reparsePoints).c_str());
    std::printf("  gone or denied   %12s\n", groupDigits(stats.unreadable).c_str());
    std::printf("\n  total size       %12.1f GB\n",
                stats.totalBytes / (1024.0 * 1024.0 * 1024.0));
    std::printf("  time             %12.2f s  (%u threads, %.0f files/s)\n",
                stats.elapsedSeconds, stats.threadsUsed,
                stats.measured / (stats.elapsedSeconds > 0 ? stats.elapsedSeconds : 1));
    std::printf("  memory held      %12.1f MB\n",
                facts.size() * sizeof(filo::FileFact) / (1024.0 * 1024.0));

    // The shape of the problem: how much of the disk sits in how few files.
    std::vector<uint64_t> sizes;
    sizes.reserve(facts.size());
    for (const filo::FileFact& fact : facts) sizes.push_back(fact.size);
    std::sort(sizes.begin(), sizes.end(), std::greater<uint64_t>());

    uint64_t running = 0;
    for (const size_t top : {100u, 1'000u, 10'000u}) {
        if (sizes.size() < top) break;
        running = 0;
        for (size_t i = 0; i < top; ++i) running += sizes[i];
        std::printf("  largest %-6s   %12.1f GB  (%.1f%% of the volume)\n",
                    groupDigits(top).c_str(), running / (1024.0 * 1024.0 * 1024.0),
                    100.0 * running / (stats.totalBytes ? stats.totalBytes : 1));
    }
    return 0;
}

// --- where the disk actually went -------------------------------------------
//
// The whole point of the feature, and the thing duplicates are only one branch
// of: take the biggest files, say what each one IS, and let the categories do
// the arguing.
int runReclaimTop(const filo::FileIndex& index) {
    const double gb = 1024.0 * 1024.0 * 1024.0;

    filo::ScanStats scan;
    const std::vector<filo::FileFact> facts = filo::collectFileFacts(index, &scan);
    std::printf("  measured %s files, %.1f GB, in %.1f s\n",
                groupDigits(scan.measured).c_str(), scan.totalBytes / gb,
                scan.elapsedSeconds);

    filo::DuplicateOptions options;
    filo::DuplicateStats duplicateStats;
    const std::vector<filo::DuplicateGroup> duplicates =
        filo::findExactDuplicates(index, facts, options, &duplicateStats);
    std::printf("  %s sets of identical files (%.2f GB)\n\n",
                groupDigits(duplicateStats.groups).c_str(),
                duplicateStats.reclaimableBytes / gb);

    filo::ClassifyStats stats;
    const std::vector<filo::Classified> items =
        filo::classifyBiggest(index, facts, duplicates, 4000, filo::nowFileTime(), &stats);

    std::printf("  the %s biggest files, by what they are\n\n",
                groupDigits(items.size()).c_str());
    for (size_t k = 0; k < static_cast<size_t>(filo::SpaceClass::Count); ++k) {
        const auto kind = static_cast<filo::SpaceClass>(k);
        if (stats.filesByClass[k] == 0) continue;
        std::printf("    %-28s %8.2f GB  %7s files%s\n",
                    toUtf8(filo::spaceClassName(kind)).c_str(), stats.bytesByClass[k] / gb,
                    groupDigits(stats.filesByClass[k]).c_str(),
                    filo::isOfferable(kind) ? "" : "   (shown, never offered)");
    }
    std::printf("\n    %-28s %8.2f GB\n", "can be offered", stats.offerableBytes / gb);

    std::printf("\n  the twenty biggest, individually\n\n");
    for (size_t i = 0; i < items.size() && i < 20; ++i) {
        const filo::Classified& item = items[i];
        std::printf("    %7.2f GB  %-26s %s\n", item.size / gb,
                    toUtf8(filo::spaceClassName(item.kind)).c_str(),
                    toUtf8(index.fullPath(item.slot)).c_str());
        std::printf("               %s\n", toUtf8(item.reason).c_str());
    }
    return 0;
}

// --- find identical files ---------------------------------------------------
int runReclaimDupes(const filo::FileIndex& index, wchar_t drive, bool apply) {
    filo::ScanStats scan;
    const std::vector<filo::FileFact> facts = filo::collectFileFacts(index, &scan);
    std::printf("  measured %s files, %.1f GB, in %.1f s\n\n",
                groupDigits(scan.measured).c_str(),
                scan.totalBytes / (1024.0 * 1024.0 * 1024.0), scan.elapsedSeconds);

    const double gb = 1024.0 * 1024.0 * 1024.0;

    // The shape of the problem, before spending anything on it. Every one of
    // these files would have to be opened, and opening is the whole cost: the
    // fingerprint pass managed 18 MB/s on an SSD, which is the filter stack and
    // not the disk.
    std::printf("  what shares a size, by floor\n");
    for (const uint64_t floor : {uint64_t{4} << 10, uint64_t{64} << 10, uint64_t{1} << 20,
                                 uint64_t{16} << 20, uint64_t{128} << 20}) {
        const std::vector<filo::SizeGroup> sized = filo::groupBySize(index, facts, floor);
        uint64_t files = 0, potential = 0;
        for (const filo::SizeGroup& group : sized) {
            files += group.slots.size();
            potential += group.potential();
        }
        std::printf("    over %6.0f KB  %10s groups  %10s files  up to %7.1f GB\n",
                    floor / 1024.0, groupDigits(sized.size()).c_str(),
                    groupDigits(files).c_str(), potential / gb);
    }
    std::printf("\n");

    filo::DuplicateOptions options;
    filo::DuplicateStats stats;
    const std::vector<filo::DuplicateGroup> groups =
        filo::findExactDuplicates(index, facts, options, &stats);

    std::printf("  the funnel\n");
    std::printf("    considered      %12s files over %.0f KB %6.2f s\n",
                groupDigits(stats.considered).c_str(), options.minimumSize / 1024.0,
                stats.sizeSeconds);
    std::printf("    share a size    %12s                    %6.2f s  read %.2f GB\n",
                groupDigits(stats.sharingASize).c_str(), stats.fingerprintSeconds,
                stats.fingerprintBytes / gb);
    std::printf("    both ends match %12s                    %6.2f s  read %.2f GB\n",
                groupDigits(stats.fullyHashed).c_str(), stats.fullHashSeconds,
                stats.fullHashBytes / gb);
    std::printf("    unreadable      %12s\n", groupDigits(stats.unreadable).c_str());
    std::printf("    hardlinks       %12s  (deleting one frees nothing)\n",
                groupDigits(stats.hardlinks).c_str());

    std::printf("\n  identical groups  %10s\n", groupDigits(stats.groups).c_str());
    std::printf("  reclaimable       %10.2f GB\n", stats.reclaimableBytes / gb);

    std::printf("\n  the ten biggest\n");
    for (size_t i = 0; i < groups.size() && i < 10; ++i) {
        const filo::DuplicateGroup& group = groups[i];
        std::printf("\n    %.2f GB reclaimable · %s copies · %.1f MB each\n",
                    group.reclaimable / gb, groupDigits(group.slots.size()).c_str(),
                    group.bytesEach / (1024.0 * 1024.0));
        for (size_t k = 0; k < group.slots.size() && k < 4; ++k) {
            std::printf("      %s %s\n", k == 0 ? "KEEP  " : "bin   ",
                        toUtf8(index.fullPath(group.slots[k])).c_str());
        }
        if (group.slots.size() > 4) {
            std::printf("      ...   and %s more\n",
                        groupDigits(group.slots.size() - 4).c_str());
        }
    }

    // --- the part that acts -------------------------------------------------
    const filo::RecycleBinInfo bin = filo::queryRecycleBin(drive);
    std::printf("\n  recycle bin: %s", bin.usable ? "available" : "DISABLED on this drive");
    if (bin.maxBytes) {
        std::printf(", %.1f GB of %.1f GB free", bin.freeBytes / gb, bin.maxBytes / gb);
    } else {
        std::printf(", capacity unknown");
    }
    std::printf("\n");

    std::unordered_map<uint32_t, const filo::FileFact*> bySlot;
    for (const filo::FileFact& fact : facts) bySlot[fact.slot] = &fact;

    std::vector<filo::BinCandidate> candidates;
    for (const filo::DuplicateGroup& group : groups) {
        for (size_t k = 1; k < group.slots.size(); ++k) {   // slot 0 is the keeper
            const auto found = bySlot.find(group.slots[k]);
            if (found == bySlot.end()) continue;
            candidates.push_back({index.fullPath(group.slots[k]), found->second->size,
                                  found->second->mtime});
        }
    }

    const filo::BinResult result =
        filo::moveToRecycleBin(drive, candidates, /*dryRun=*/!apply);

    if (!result.error.empty()) {
        std::printf("  REFUSED: %s\n", toUtf8(result.error).c_str());
        return 1;
    }
    std::printf("  %s %s files, %.2f GB\n", apply ? "recycled" : "would recycle",
                groupDigits(result.binned).c_str(), result.bytesFreed / gb);
    if (result.missing) {
        std::printf("    gone since measured   %8s\n", groupDigits(result.missing).c_str());
    }
    if (result.changed) {
        std::printf("    changed since hashed  %8s  (skipped: not the file we compared)\n",
                    groupDigits(result.changed).c_str());
    }
    if (result.wouldBePermanent) {
        std::printf("    too big for the bin   %8s  (skipped: would delete permanently)\n",
                    groupDigits(result.wouldBePermanent).c_str());
    }
    if (result.failed) {
        std::printf("    failed                %8s\n", groupDigits(result.failed).c_str());
    }
    if (!apply) {
        std::printf("\n  nothing was touched. Add --apply to move these to the "
                    "Recycle Bin.\n");
    } else {
        std::printf("  log: %s\n", toUtf8(result.logPath).c_str());
    }
    return 0;
}

// --- remove documents whose files are gone ----------------------------------
//
// Deleted files used to stay in the content index forever. The UI hid them by
// checking the name index at query time, which looks like it works and is not
// the same thing: the FTS rows, the chunks and the stored text all remained,
// growing without bound, and every search paid to score passages belonging to
// files that no longer exist.
//
// The vectors have to go in the same breath. chunk_id is an INTEGER PRIMARY KEY
// without AUTOINCREMENT, so SQLite reissues the highest ids once they are
// freed: a vector left pointing at a purged chunk would later be handed a
// completely unrelated one. That is the NTFS sequence-number trap again, and it
// fails the same silent way.
bool purgeDeletedDocuments(const filo::FileIndex& index, wchar_t drive,
                           filo::ContentDb& db, uint64_t* purgedDocs,
                           uint64_t* purgedVectors, std::wstring* error) {
    *purgedDocs = 0;
    *purgedVectors = 0;

    const uint32_t volumeId = filo::volumeSerial(drive);

    // Which documents are dead, and which chunks go with them.
    std::vector<int64_t> deadDocs;
    std::unordered_set<uint32_t> deadChunks;
    {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db.handle(), "SELECT doc_id, frn FROM doc WHERE volume_id = ?",
                               -1, &statement, nullptr) != SQLITE_OK) {
            *error = L"cannot enumerate documents";
            return false;
        }
        sqlite3_bind_int64(statement, 1, static_cast<int64_t>(volumeId));
        while (sqlite3_step(statement) == SQLITE_ROW) {
            const auto frn = static_cast<uint64_t>(sqlite3_column_int64(statement, 1));
            const uint32_t slot = index.indexOfFrn(frn);
            if (slot != filo::kNoParent && !index.isDeleted(slot)) continue;
            deadDocs.push_back(sqlite3_column_int64(statement, 0));
        }
        sqlite3_finalize(statement);
    }
    if (deadDocs.empty()) return true;

    {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db.handle(), "SELECT chunk_id FROM chunk WHERE doc_id = ?", -1,
                               &statement, nullptr) == SQLITE_OK) {
            for (const int64_t docId : deadDocs) {
                sqlite3_reset(statement);
                sqlite3_bind_int64(statement, 1, docId);
                while (sqlite3_step(statement) == SQLITE_ROW) {
                    deadChunks.insert(
                        static_cast<uint32_t>(sqlite3_column_int64(statement, 0)));
                }
            }
            sqlite3_finalize(statement);
        }
    }

    // Vectors FIRST. If the process dies between the two, an orphaned vector is
    // harmless — its chunk lookup simply fails — whereas a vector still keyed to
    // an id the database has since reissued is wrong and invisible.
    const std::wstring vectorPath = contentDbPath(drive) + L".vec";
    if (GetFileAttributesW(vectorPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        filo::VectorStore store;
        filo::VectorStore::Identity identity;
        std::wstring vectorError;
        if (store.loadForMaintenance(vectorPath, &identity, &vectorError)) {
            const size_t removed = store.removeChunks(deadChunks);
            if (removed > 0 && !store.save(vectorPath, identity, &vectorError)) {
                *error = L"vectors not rewritten: " + vectorError;
                return false;
            }
            *purgedVectors = removed;
        }
    }

    filo::ContentWriter writer(db);
    if (!writer.prepare(error)) return false;
    for (const int64_t docId : deadDocs) {
        if (!writer.deleteDocument(docId, error)) return false;
        ++(*purgedDocs);
    }
    if (!writer.commitBatch(error)) return false;
    db.checkpoint(false);
    return true;
}

int runPurge(const filo::FileIndex& index, wchar_t drive) {
    filo::ContentDb db;
    std::wstring error;
    if (!db.open(contentDbPath(drive), false, &error)) {
        std::printf("cannot open the database: %s\n", toUtf8(error).c_str());
        return 1;
    }

    const auto start = std::chrono::steady_clock::now();
    uint64_t docs = 0, vectors = 0;
    if (!purgeDeletedDocuments(index, drive, db, &docs, &vectors, &error)) {
        std::printf("purge failed: %s\n", toUtf8(error).c_str());
        return 1;
    }

    std::printf("  documents removed %8s\n", groupDigits(docs).c_str());
    std::printf("  vectors removed   %8s\n", groupDigits(vectors).c_str());
    std::printf("  database          %8.1f MB\n", db.fileBytes() / (1024.0 * 1024.0));
    std::printf("  time              %8.1f s\n", secondsSince(start));
    return 0;
}

// --- recompute the rank penalties in place ----------------------------------
//
// The penalty is decided from the path and stored on the document row, so
// changing the rule leaves every existing row wrong. A full reindex would fix
// it and renumber every chunk in the process, which throws away the vector
// store — hours of work to change one column.
//
// This walks the documents, recomputes the penalty from the current path and
// UPDATEs it. Chunk ids are untouched, so the vectors stay valid.
int runRepenalize(const filo::FileIndex& index, wchar_t drive) {
    filo::ContentDb db;
    std::wstring error;
    if (!db.open(contentDbPath(drive), false, &error)) {
        std::printf("cannot open the database: %s\n", toUtf8(error).c_str());
        return 1;
    }

    sqlite3_stmt* select = nullptr;
    if (sqlite3_prepare_v2(db.handle(), "SELECT doc_id, frn, rank_penalty FROM doc", -1,
                           &select, nullptr) != SQLITE_OK) {
        std::printf("query failed\n");
        return 1;
    }
    sqlite3_stmt* update = nullptr;
    sqlite3_prepare_v2(db.handle(), "UPDATE doc SET rank_penalty = ? WHERE doc_id = ?", -1,
                       &update, nullptr);

    db.exec("BEGIN IMMEDIATE", &error);

    uint64_t seen = 0, changed = 0, missing = 0;
    const auto start = std::chrono::steady_clock::now();
    while (sqlite3_step(select) == SQLITE_ROW) {
        ++seen;
        const int64_t docId = sqlite3_column_int64(select, 0);
        const auto frn = static_cast<uint64_t>(sqlite3_column_int64(select, 1));
        const double current = sqlite3_column_double(select, 2);

        const uint32_t slot = index.indexOfFrn(frn);
        if (slot == filo::kNoParent) { ++missing; continue; }

        const double penalty = filo::contentRankPenalty(index.fullPath(slot));
        if (penalty == current) continue;

        sqlite3_reset(update);
        sqlite3_bind_double(update, 1, penalty);
        sqlite3_bind_int64(update, 2, docId);
        if (sqlite3_step(update) == SQLITE_DONE) ++changed;
    }
    db.exec("COMMIT", &error);
    sqlite3_finalize(select);
    sqlite3_finalize(update);
    db.checkpoint(false);

    std::printf("  documents    %10s\n", groupDigits(seen).c_str());
    std::printf("  changed      %10s\n", groupDigits(changed).c_str());
    std::printf("  not in index %10s\n", groupDigits(missing).c_str());
    std::printf("  time         %10.1f s\n", secondsSince(start));
    return 0;
}

// --- self-test for the name index ------------------------------------------
//
// These are the invariants that cost the most to get wrong, because breaking
// them produces confident, plausible, wrong answers rather than a crash: a file
// reported under a path it does not have. The USN journal needs administrator
// rights to read, so the incremental paths cannot be exercised against a real
// volume in an ordinary run — this drives FileIndex directly instead.
namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("  %s  %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) ++failures;
}

void checkPath(const filo::FileIndex& index, uint32_t slot, const wchar_t* expected,
               const char* what) {
    const std::wstring got = index.fullPath(slot);
    const bool same = got == expected;
    std::printf("  %s  %s\n", same ? "ok  " : "FAIL", what);
    if (!same) {
        std::printf("        expected %s\n        got      %s\n",
                    toUtf8(expected).c_str(), toUtf8(got).c_str());
        ++failures;
    }
}

}  // namespace

int runSelfTest() {
    std::printf("FileIndex\n");

    // C:\  ->  work\  ->  notes.txt
    filo::FileIndex index;
    index.setDrive(L'C');
    index.append(5, 5, FILE_ATTRIBUTE_DIRECTORY, L".", 1);              // root
    index.append(100, 5, FILE_ATTRIBUTE_DIRECTORY, L"work", 4);
    index.append(200, 100, FILE_ATTRIBUTE_NORMAL, L"notes.txt", 9);
    index.finalize();

    const uint32_t folder = index.indexOfFrn(100);
    const uint32_t file = index.indexOfFrn(200);
    check(folder != filo::kNoParent && file != filo::kNoParent, "records are findable by FRN");
    checkPath(index, file, L"C:\\work\\notes.txt", "path before any change");

    // A rename must keep the slot: the child holds the parent's INDEX, so
    // burying the folder and appending a new one leaves the child pointing at
    // a tombstone that still carries the old name.
    index.updateEntry(folder, 5, FILE_ATTRIBUTE_DIRECTORY, L"archive", 7);
    checkPath(index, folder, L"C:\\archive", "folder reports its new name");
    checkPath(index, file, L"C:\\archive\\notes.txt", "CHILD follows a renamed folder");
    check(index.indexOfFrn(100) == folder, "renamed folder keeps its slot");

    // A longer name than the original: the pool has to grow rather than
    // overwrite whatever sat next to it.
    index.updateEntry(folder, 5, FILE_ATTRIBUTE_DIRECTORY,
                      L"a-considerably-longer-folder-name", 33);
    checkPath(index, file, L"C:\\a-considerably-longer-folder-name\\notes.txt",
              "child follows a rename that grows the name");
    checkPath(index, index.indexOfFrn(5), L"C:", "root still resolves");

    // A move: same file, different parent.
    index.append(300, 5, FILE_ATTRIBUTE_DIRECTORY, L"other", 5);
    index.finalize();
    index.updateEntry(index.indexOfFrn(200), 300, FILE_ATTRIBUTE_NORMAL, L"notes.txt", 9);
    checkPath(index, index.indexOfFrn(200), L"C:\\other\\notes.txt", "moved file reports its new parent");

    // NTFS reuses MFT records and distinguishes them by the sequence number in
    // the top 16 bits. Matching on the low 48 alone once handed a file the path
    // of an unrelated one.
    std::printf("\nFRN identity\n");
    filo::FileIndex reuse;
    reuse.setDrive(L'C');
    reuse.append(5, 5, FILE_ATTRIBUTE_DIRECTORY, L".", 1);
    const uint64_t oldFrn = (1ull << 48) | 700;   // sequence 1, record 700
    const uint64_t newFrn = (2ull << 48) | 700;   // sequence 2, SAME record
    reuse.append(oldFrn, 5, FILE_ATTRIBUTE_NORMAL, L"deleted.dat", 11);
    reuse.append(newFrn, 5, FILE_ATTRIBUTE_NORMAL, L"fresh.dat", 9);
    reuse.finalize();
    reuse.markDeleted(reuse.indexOfFrn(oldFrn));

    const uint32_t fresh = reuse.indexOfFrn(newFrn);
    check(fresh != filo::kNoParent && reuse.name(fresh) == L"fresh.dat",
          "same MFT record, different sequence: no confusion");
    check(reuse.indexOfFrn((3ull << 48) | 700) == filo::kNoParent,
          "an unknown sequence number matches nothing");

    // Compaction has to renumber parents, or every path silently shifts.
    std::printf("\ncompact()\n");
    reuse.compact();
    const uint32_t afterCompact = reuse.indexOfFrn(newFrn);
    check(afterCompact != filo::kNoParent, "surviving record still findable");
    checkPath(reuse, afterCompact, L"C:\\fresh.dat", "path survives compaction");
    check(reuse.deletedCount() == 0, "tombstones are gone");

    // Removing vectors is what makes purging a deleted file safe. chunk_id is
    // reissued by SQLite once freed, so a vector left behind would later be
    // handed an unrelated chunk — and compaction moving the wrong bytes would
    // do the same thing while looking like it worked.
    std::printf("\nVectorStore::removeChunks\n");
    filo::VectorStore store;
    constexpr uint32_t kDims = 4;
    store.configure(kDims);

    auto sample = [](uint32_t i) {
        std::vector<float> v(kDims, 0.0f);
        v[i % kDims] = 1.0f;
        v[(i + 1) % kDims] = 0.2f * static_cast<float>(i + 1);
        filo::normalizeVector(v.data(), kDims);
        return v;
    };
    for (uint32_t i = 0; i < 6; ++i) {
        const std::vector<float> v = sample(i);
        store.add(100 + i, v.data());
    }
    check(store.size() == 6, "six vectors stored");

    // The vector of chunk 103 must still find chunk 103 after the arrays have
    // been shuffled around it.
    const std::vector<float> probe = sample(3);
    auto found = store.search(probe.data(), 1, 1);
    check(found.size() == 1 && found[0].chunkId == 103, "search finds its own chunk");

    check(store.removeChunks({101, 104}) == 2, "two chunks reported removed");
    check(store.size() == 4, "size shrank by two");

    found = store.search(probe.data(), 1, 1);
    check(found.size() == 1 && found[0].chunkId == 103,
          "a surviving vector still belongs to its own chunk after compaction");

    const std::vector<uint32_t> expected = {100, 102, 103, 105};
    check(store.chunkIds() == expected, "the removed ids are gone and order is kept");
    check(store.removeChunks({999}) == 0, "removing an absent id changes nothing");

    // --- the query planner ---------------------------------------------------
    //
    // A pure function of (query, clock), which is why the clock is a parameter:
    // "marzo" has to mean the same month every time the test runs.
    std::printf("\nquery planner\n");
    SYSTEMTIME fixed{};
    fixed.wYear = 2026;
    fixed.wMonth = 7;
    fixed.wDay = 28;
    FILETIME fixedFile{};
    SystemTimeToFileTime(&fixed, &fixedFile);
    const int64_t now =
        (static_cast<int64_t>(fixedFile.dwHighDateTime) << 32) | fixedFile.dwLowDateTime;

    auto hasExtension = [](const filo::QueryPlan& plan, const wchar_t* wanted) {
        return std::find(plan.extensions.begin(), plan.extensions.end(), wanted) !=
               plan.extensions.end();
    };

    {
        const filo::QueryPlan plan = filo::planQuery(L"il pdf del contratto di marzo", now);
        check(plan.text == L"contratto", "the subject survives, the filters are lifted out");
        check(hasExtension(plan, L"pdf"), "pdf recognized as a format");
        check(plan.after != 0 && plan.before != 0, "a month becomes a bounded range");
        check(plan.after < plan.before, "the range runs forwards");
    }
    {
        const filo::QueryPlan plan = filo::planQuery(L"foto nei download", now);
        check(plan.folder == L"\\downloads\\", "a folder behind a preposition is a place");
        check(hasExtension(plan, L"jpg") && hasExtension(plan, L"png"),
              "photos mean image extensions");
    }
    {
        // No filter words at all: nothing may be taken away.
        const filo::QueryPlan plan = filo::planQuery(L"contratto noleggio", now);
        check(plan.text == L"contratto noleggio", "an ordinary query is left alone");
        check(!plan.hasFilters(), "and carries no filters");
    }
    {
        const filo::QueryPlan plan = filo::planQuery(L"tipo:xlsx tasse", now);
        check(hasExtension(plan, L"xlsx") && plan.text == L"tasse",
              "an explicit operator is honoured and removed");
    }
    {
        // Every word was a filter. Stripping them all would leave nothing to
        // search for, so the words stay AND the filter applies.
        const filo::QueryPlan plan = filo::planQuery(L"foto", now);
        check(plan.text == L"foto", "a query made only of filter words keeps its text");
        check(plan.hasFilters(), "and still applies the filter");
    }
    {
        // Titles made of function words. Stripping is decided on what the user
        // typed, so anything three words or shorter is left whole.
        check(filo::planQuery(L"This Is Us", now).text == L"this is us",
              "a short title made of stopwords survives intact");
        check(filo::planQuery(L"la casa", now).text == L"la casa",
              "and so does a two-word one");
        check(filo::planQuery(L"where is the spreadsheet with the invoices", now).text ==
                  L"invoices",
              "a seven-word question is stripped to its subject");
    }
    {
        // Punctuation used to hide a filter word from every rule.
        const filo::QueryPlan plan = filo::planQuery(L"la fattura, in pdf, di ieri?", now);
        check(plan.after != 0, "a date followed by a question mark is still a date");
        const filo::QueryPlan bare = filo::planQuery(L"fattura pdf ieri", now);
        check(plan.extensions == bare.extensions,
              "punctuation around a format word changes nothing");
    }
    {
        const filo::QueryPlan plan = filo::planQuery(L"relazione 2024", now);
        check(plan.text == L"relazione" && plan.after != 0, "a bare year is a range");
        const filo::QueryPlan notAYear = filo::planQuery(L"modulo 1234", now);
        check(notAYear.text == L"modulo 1234", "a number that is not a plausible year stays");
    }
    {
        const filo::QueryPlan plan = filo::planQuery(L"pdf di marzo", now);
        // An unknown timestamp earns nothing and loses nothing: the extension
        // still counts, the date simply does not.
        check(plan.satisfiedBy(L"c:\\a\\b.pdf", 0) == 1,
              "an unreadable timestamp earns no time bonus");
        check(plan.satisfiedBy(L"c:\\a\\b.txt", 0) == 0,
              "a wrong extension simply scores less, it is not excluded");

        // And a date that IS readable counts for what it is.
        const int64_t inMarch = plan.after + 5 * 86'400LL * 10'000'000LL;
        check(plan.satisfiedBy(L"c:\\a\\b.pdf", inMarch) == 2,
              "a file dated inside the range satisfies both filters");
        check(plan.satisfiedBy(L"c:\\a\\b.pdf", plan.before + 1) == 1,
              "a file dated outside it satisfies only the extension");
    }

    // --- who owns the file decides before what the file is ------------------
    //
    // The classifier was one if/else chain doing two jobs, and the descriptive
    // branches fired first: a .log under C:\Windows\Logs came out "written by a
    // program" and offerable. These checks exist because nothing else would
    // have noticed that, and nothing else would notice it coming back.
    std::printf("\nreclaim: ownership gates\n");
    {
        check(filo::insideFolder(L"c:\\users\\alice\\a.txt", L"c:\\users\\alice"),
              "a file under the profile is inside it");
        check(!filo::insideFolder(L"c:\\users\\alice-old\\a.txt", L"c:\\users\\alice"),
              "a name that merely starts the same is NOT inside it");
        check(!filo::insideFolder(L"c:\\users\\alice", L"c:\\users\\alice"),
              "the folder is not inside itself");
        check(filo::insideFolder(L"c:\\users\\alice\\a.txt", L"c:\\users\\alice\\"),
              "a trailing separator on the folder changes nothing");

        // C:\ -> Windows\Logs\big.log
        //     -> Users\alice\{work\node_modules\big.bin, AppData\Local\app\big.log}
        //     -> Users\alice-old\big.log
        filo::FileIndex tree;
        tree.setDrive(L'C');
        tree.append(5, 5, FILE_ATTRIBUTE_DIRECTORY, L".", 1);
        tree.append(10, 5, FILE_ATTRIBUTE_DIRECTORY, L"Windows", 7);
        tree.append(11, 10, FILE_ATTRIBUTE_DIRECTORY, L"Logs", 4);
        tree.append(12, 11, FILE_ATTRIBUTE_NORMAL, L"big.log", 7);
        tree.append(20, 5, FILE_ATTRIBUTE_DIRECTORY, L"Users", 5);
        tree.append(21, 20, FILE_ATTRIBUTE_DIRECTORY, L"alice", 5);
        tree.append(22, 21, FILE_ATTRIBUTE_DIRECTORY, L"work", 4);
        tree.append(23, 22, FILE_ATTRIBUTE_DIRECTORY, L"node_modules", 12);
        tree.append(24, 23, FILE_ATTRIBUTE_NORMAL, L"big.bin", 7);
        tree.append(25, 21, FILE_ATTRIBUTE_DIRECTORY, L"AppData", 7);
        tree.append(26, 25, FILE_ATTRIBUTE_DIRECTORY, L"Local", 5);
        tree.append(27, 26, FILE_ATTRIBUTE_NORMAL, L"app.log", 7);
        tree.append(30, 20, FILE_ATTRIBUTE_DIRECTORY, L"alice-old", 9);
        tree.append(31, 30, FILE_ATTRIBUTE_NORMAL, L"big.log", 7);
        // Two more projects, differing only in when their SOURCE was last
        // written. That is the whole rule: the tree's own dates move on every
        // install and say nothing about whether anyone still works here.
        tree.append(40, 21, FILE_ATTRIBUTE_DIRECTORY, L"dormant", 7);
        tree.append(41, 40, FILE_ATTRIBUTE_DIRECTORY, L"node_modules", 12);
        tree.append(42, 41, FILE_ATTRIBUTE_NORMAL, L"dep.bin", 7);
        tree.append(43, 40, FILE_ATTRIBUTE_NORMAL, L"main.c", 6);
        tree.append(50, 21, FILE_ATTRIBUTE_DIRECTORY, L"live", 4);
        tree.append(51, 50, FILE_ATTRIBUTE_DIRECTORY, L"node_modules", 12);
        tree.append(52, 51, FILE_ATTRIBUTE_NORMAL, L"dep.bin", 7);
        tree.append(53, 50, FILE_ATTRIBUTE_NORMAL, L"main.c", 6);
        tree.finalize();

        const int64_t longAgo = now - 400LL * 86'400LL * 10'000'000LL;
        const int64_t yesterday = now - 86'400LL * 10'000'000LL;

        std::vector<filo::FileFact> facts;
        auto addFact = [&](uint64_t frn, uint64_t size, int64_t mtime) {
            filo::FileFact fact;
            fact.slot = tree.indexOfFrn(frn);
            fact.size = size;
            fact.mtime = mtime;
            facts.push_back(fact);
        };
        for (const uint64_t frn : {12ull, 24ull, 27ull, 31ull, 42ull, 52ull}) {
            addFact(frn, 1ull << 30, longAgo);
        }
        addFact(43, 4096, longAgo);      // dormant project: source is old too
        addFact(53, 4096, yesterday);    // live project: touched yesterday

        // The real code reads USERPROFILE, so the test sets it rather than
        // taking a different path than the program does.
        wchar_t saved[MAX_PATH] = {};
        GetEnvironmentVariableW(L"USERPROFILE", saved, MAX_PATH);
        SetEnvironmentVariableW(L"USERPROFILE", L"C:\\Users\\alice");

        filo::ClassifyStats classifyStats;
        const std::vector<filo::Classified> verdicts =
            filo::classifyBiggest(tree, facts, {}, 10, now, &classifyStats);

        SetEnvironmentVariableW(L"USERPROFILE", saved[0] ? saved : nullptr);

        auto kindOf = [&](uint64_t frn) {
            const uint32_t slot = tree.indexOfFrn(frn);
            for (const filo::Classified& verdict : verdicts) {
                if (verdict.slot == slot) return verdict.kind;
            }
            return filo::SpaceClass::Count;   // no verdict at all
        };

        // Not "one verdict per fact": a tree stands for the files inside it, so
        // the counts differ on purpose. What must hold is that the rows and the
        // totals agree, or the screen adds up to a disk nobody has.
        uint64_t counted = 0;
        for (size_t k = 0; k < static_cast<size_t>(filo::SpaceClass::Count); ++k) {
            counted += classifyStats.filesByClass[k];
        }
        check(counted == verdicts.size(), "every row reported is a row counted");
        check(classifyStats.examined == verdicts.size(),
              "including the ones that left through a gate");
        check(kindOf(12) == filo::SpaceClass::NotYours,
              "a Windows log is not yours, whatever else it looks like");
        check(!filo::isOfferable(kindOf(12)), "and so it is never offered");
        check(kindOf(31) == filo::SpaceClass::NotYours,
              "another account whose name starts like yours is still not you");
        check(kindOf(27) == filo::SpaceClass::AppState,
              "a log inside AppData belongs to the app that wrote it");
        check(!filo::isOfferable(kindOf(27)), "and so it is not offered either");

        // A generated tree is reported as a tree, and only when the project
        // around it is finished with. Offering one file out of node_modules is
        // the worst of both worlds: npm install will not repair a package
        // directory that already exists, so half a tree needs a clean reinstall
        // while a whole one comes back.
        check(kindOf(24) == filo::SpaceClass::Count &&
              kindOf(42) == filo::SpaceClass::Count &&
              kindOf(52) == filo::SpaceClass::Count,
              "no single file inside a generated tree is offered on its own");
        check(kindOf(41) == filo::SpaceClass::Regenerable,
              "a tree whose project has not been touched in a year is offered");
        check(filo::isOfferable(kindOf(41)), "and it really can be selected");
        check(kindOf(51) == filo::SpaceClass::ProjectInUse,
              "the same tree in a project touched yesterday is not");
        check(!filo::isOfferable(kindOf(51)), "and it cannot be selected");
        check(kindOf(23) == filo::SpaceClass::ProjectInUse,
              "nor is one with nothing beside it to date the project by");

        // The tree stands for everything underneath it, which is the number the
        // old per-file list could never reach.
        for (const filo::Classified& verdict : verdicts) {
            if (verdict.slot != tree.indexOfFrn(41)) continue;
            check(verdict.size == (1ull << 30) && verdict.files == 1,
                  "a tree carries the size and the file count of its contents");
        }
    }

    // --- an unknown Recycle Bin capacity must refuse, not wave things through -
    //
    // Read-only: this asks Windows about the bin, it does not put anything in
    // one. The check that used to guard oversized files was written as
    // "if (maxBytes != 0)", and maxBytes is zero whenever Windows has not
    // recorded a capacity — which is most machines. So the guard switched
    // itself off exactly where it was needed, and an oversized file went to
    // SHFileOperation with confirmations suppressed, where Windows destroys it
    // and still reports success.
    std::printf("\nreclaim: Recycle Bin limits\n");
    {
        const filo::RecycleBinInfo bin = filo::queryRecycleBin(L'C');
        check(bin.maxBytes > 0 || !bin.usable,
              "a usable bin always reports a capacity, measured or estimated");
        check(!bin.capacityKnown || bin.maxBytes > 0,
              "a capacity read from Windows is never zero");

        // The size test now runs whether or not the capacity was measured, so
        // a file larger than the bin is refused either way.
        std::vector<filo::BinCandidate> huge;
        filo::BinCandidate candidate;
        candidate.path = L"C:\\does-not-exist-filo-selftest.bin";
        candidate.size = ~0ull / 2;
        huge.push_back(candidate);
        const filo::BinResult refused =
            filo::moveToRecycleBin(L'C', huge, /*dryRun=*/true);
        check(refused.binned == 0, "an impossibly large file is never accepted");
    }

    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "THERE ARE FAILURES");
    return failures == 0 ? 0 : 1;
}

// --- prove the Recycle Bin path actually works ------------------------------
//
// On a file this creates itself, never on the user's. "It compiles" is not the
// same claim as "it moves a file to the bin, and refuses when it should", and
// the difference matters more here than anywhere else in the program.
//
// Kept out of --selftest because it leaves something in the Recycle Bin, and a
// test that has side effects on the user's machine should be asked for.
int runBinTest() {
    wchar_t temporary[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, temporary) == 0) {
        std::printf("no temp directory\n");
        return 1;
    }
    const std::wstring path = std::wstring(temporary) + L"filo-bin-test.tmp";

    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        std::printf("cannot create the test file\n");
        return 1;
    }
    const char payload[] = "filo recycle bin test";
    DWORD written = 0;
    WriteFile(file, payload, sizeof(payload), &written, nullptr);
    CloseHandle(file);

    WIN32_FILE_ATTRIBUTE_DATA facts{};
    GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &facts);
    filo::BinCandidate candidate;
    candidate.path = path;
    candidate.size = facts.nFileSizeLow;
    candidate.mtime = (static_cast<int64_t>(facts.ftLastWriteTime.dwHighDateTime) << 32) |
                      facts.ftLastWriteTime.dwLowDateTime;

    std::printf("recycle bin\n");
    check(GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES,
          "the test file exists");

    // A candidate whose recorded size no longer matches the disk must be
    // refused. On a stale index that is the whole difference between deleting
    // the file we compared and deleting whatever has taken its place.
    filo::BinCandidate stale = candidate;
    stale.size += 1;
    const filo::BinResult refused =
        filo::moveToRecycleBin(path[0], {stale}, /*dryRun=*/false);
    check(refused.binned == 0 && refused.changed == 1,
          "a file that changed since it was measured is refused");
    check(GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES,
          "and is still on disk");

    // Size and time can match a DIFFERENT file. The reference number is what
    // says it is the same file object, so a wrong one has to be refused even
    // though everything else agrees.
    filo::BinCandidate impostor = candidate;
    impostor.frn = 0xDEADBEEFull;
    const filo::BinResult wrongIdentity =
        filo::moveToRecycleBin(path[0], {impostor}, /*dryRun=*/false);
    check(wrongIdentity.binned == 0 && wrongIdentity.changed == 1,
          "a file whose reference number does not match is refused");
    check(GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES,
          "and it too is still on disk");

    // Held open for writing by somebody else: leave it alone rather than fight
    // over it. Reported apart from "changed", because nothing is wrong with it.
    const HANDLE writer = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const filo::BinResult busy =
        filo::moveToRecycleBin(path[0], {candidate}, /*dryRun=*/false);
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    check(busy.binned == 0 && busy.locked == 1,
          "a file another process is writing to is left alone");

    // And the identity it does have is accepted, so the check is not simply
    // refusing everything.
    filo::BinCandidate identified = candidate;
    {
        const HANDLE probe = CreateFileW(path.c_str(), GENERIC_READ,
                                         FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        BY_HANDLE_FILE_INFORMATION info{};
        if (probe != INVALID_HANDLE_VALUE && GetFileInformationByHandle(probe, &info)) {
            identified.frn = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) |
                             info.nFileIndexLow;
            identified.volumeSerial = info.dwVolumeSerialNumber;
        }
        if (probe != INVALID_HANDLE_VALUE) CloseHandle(probe);
    }
    const filo::BinResult matches =
        filo::moveToRecycleBin(path[0], {identified}, /*dryRun=*/true);
    check(matches.binned == 1, "the right reference number is accepted");

    const filo::BinResult dry =
        filo::moveToRecycleBin(path[0], {candidate}, /*dryRun=*/true);
    check(dry.binned == 1, "a dry run reports what it would do");
    check(GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES,
          "and touches nothing");

    const filo::BinResult done =
        filo::moveToRecycleBin(path[0], {candidate}, /*dryRun=*/false);
    check(done.binned == 1 && done.error.empty(), "the file is recycled");
    check(GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES,
          "and is gone from its path");
    check(!done.logPath.empty(), "a log records where it went");

    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "THERE ARE FAILURES");
    std::printf("(one small test file is now in your Recycle Bin)\n");
    return failures == 0 ? 0 : 1;
}

#ifdef FILO_HAVE_LLAMA
// Tokenization checks. These need a real model, so they live behind an optional
// argument rather than in the model-free self-test above.
//
// The case that matters is text longer than the context window. llama_tokenize
// reports the requirement and writes nothing, and the old code answered by
// claiming success over a zero-filled buffer — so EVERY over-long chunk got the
// same vector. A test that only embeds short strings would have stayed green
// through all of it.
int runTokenizerTest(const std::wstring& modelPath) {
    filo::Embedder embedder;
    std::wstring error;
    if (!embedder.load(modelPath, {}, &error)) {
        std::printf("cannot load model: %s\n", toUtf8(error).c_str());
        return 1;
    }
    const uint32_t dims = embedder.dimensions();
    const uint32_t window = embedder.contextTokens();
    std::printf("\ntokenizer (model loaded, %u dims, %u token window)\n", dims, window);

    auto sentence = [](const char* seed, int times) {
        std::string out;
        for (int i = 0; i < times; ++i) out += seed;
        return out;
    };

    std::vector<float> a(dims), b(dims), c(dims);

    // Short text: the ordinary path, unchanged.
    const std::string shortText = "Il contratto prevede una penale per il ritardo.";
    check(embedder.embedPassage(shortText, a.data()), "short text embeds");

    // Same text twice must give the same vector, or nothing downstream is
    // reproducible.
    check(embedder.embedPassage(shortText, b.data()), "short text embeds again");
    bool identical = true;
    for (uint32_t i = 0; i < dims; ++i) {
        if (std::fabs(a[i] - b[i]) > 1e-6f) { identical = false; break; }
    }
    check(identical, "repeated calls are deterministic");

    // Longer than the window. Both must succeed AND differ from each other:
    // before the fix every over-long text collapsed onto one vector, because
    // all of them embedded the same buffer of zeros.
    const std::string longA = sentence(
        "Il presente contratto disciplina il rapporto tra le parti e definisce termini, "
        "modalita' di pagamento e penali per il ritardo. ", 40);
    const std::string longB = sentence(
        "La ricetta prevede farina, uova, zucchero e lievito, da mescolare a lungo "
        "prima di infornare a centottanta gradi per quaranta minuti. ", 40);
    check(longA.size() > 3000 && longB.size() > 3000, "long samples exceed the window");

    const uint64_t truncatedBefore = embedder.stats().truncated;
    check(embedder.embedPassage(longA, a.data()), "over-long text A embeds");
    check(embedder.embedPassage(longB, b.data()), "over-long text B embeds");
    check(embedder.stats().truncated >= truncatedBefore + 2,
          "over-long text is reported as truncated");

    double dot = 0;
    for (uint32_t i = 0; i < dims; ++i) dot += static_cast<double>(a[i]) * b[i];
    check(dot < 0.99, "two different over-long texts get DIFFERENT vectors");

    // A vector of all zeros would mean nothing was really embedded.
    double magnitude = 0;
    for (uint32_t i = 0; i < dims; ++i) magnitude += static_cast<double>(a[i]) * a[i];
    check(magnitude > 0.9 && magnitude < 1.1, "the vector is normalized, not empty");

    // The leading part of a long text must dominate: truncation keeps the head,
    // so a long text should still resemble its own opening sentence.
    check(embedder.embedPassage(
              "Il presente contratto disciplina il rapporto tra le parti.", c.data()),
          "opening sentence embeds");
    double dotHead = 0, dotOther = 0;
    for (uint32_t i = 0; i < dims; ++i) {
        dotHead += static_cast<double>(a[i]) * c[i];
        dotOther += static_cast<double>(b[i]) * c[i];
    }
    check(dotHead > dotOther, "a truncated text still matches its own opening");

    // --- the chunker's token estimate, against the real tokenizer -----------
    //
    // This is the check the whole of chunker version 3 rests on, and it is the
    // one thing that cannot be reasoned out on paper: the chunker guesses token
    // counts from bytes, because it runs in the extraction pass where no model
    // is loaded, and coupling chunk offsets to one model file would make an
    // index incomparable with a fresh chunking of the same text.
    //
    // So the guess gets measured here, where a model IS loaded. If a chunk goes
    // over the window the embedder truncates it silently and the tail is lost to
    // search by meaning — which is exactly the bug version 3 exists to fix, and
    // exactly the bug that hid for so long because nothing asked.
    std::printf("\nchunker against the real tokenizer\n");
    {
        struct Sample {
            const char* what;
            std::string text;
        };
        std::vector<Sample> samples;
        samples.push_back({"Italian prose", sentence(
            "Il presente contratto disciplina il rapporto tra le parti e definisce "
            "termini, modalita' di pagamento, penali per il ritardo e condizioni di "
            "risoluzione anticipata. ", 120)});
        // Figures tokenize far worse than prose: this is where a byte-based
        // target went furthest over the window.
        samples.push_back({"a table of figures", sentence(
            "2024-03-17;4821,55;IT60X0542811101000000123456;12,00%;0,000481;"
            "9987654321;44,7\n", 200)});
        samples.push_back({"code", sentence(
            "  const std::vector<uint32_t>& slots = index.childrenOf(slot, &count);\n"
            "  if (slots.empty()) { return kNoParent; }  // nothing to walk\n", 150)});
        samples.push_back({"accented and mixed", sentence(
            "Perché è così? Città, però, già, più — e qualche parola in inglese: "
            "settlement agreement, indemnity clause. ", 120)});

        uint64_t totalChunks = 0;
        for (const Sample& sample : samples) {
            const std::vector<filo::ChunkRecord> pieces = filo::chunkText(sample.text);
            check(!pieces.empty(),
                  (std::string("chunks are produced for ") + sample.what).c_str());

            embedder.resetStats();
            std::vector<float> vector(dims);
            for (const filo::ChunkRecord& piece : pieces) {
                embedder.embedPassage(sample.text.substr(piece.byteOffset, piece.byteLength),
                                      vector.data());
            }
            totalChunks += pieces.size();

            const filo::Embedder::Stats& stats = embedder.stats();
            std::printf("      %-20s %3zu chunks, longest %u tokens of %u, %llu truncated\n",
                        sample.what, pieces.size(), stats.tokensMax, window,
                        static_cast<unsigned long long>(stats.truncated));
            check(stats.truncated == 0,
                  (std::string("no chunk of ") + sample.what + " is truncated").c_str());
            check(stats.tokensMax <= window,
                  (std::string("every chunk of ") + sample.what +
                   " fits the window").c_str());
        }
        check(totalChunks > 0, "the samples really were chunked");
    }

    // Input the guard is meant to stop: one enormous run with no separator.
    std::string blob(20000, 'A');
    const uint64_t rejectedBefore = embedder.stats().rejected;
    const bool blobOk = embedder.embedPassage(blob, a.data());
    check(!blobOk && embedder.stats().rejected > rejectedBefore,
          "a separator-free blob is refused, not crashed on");

    // And the process is still alive and correct afterwards.
    check(embedder.embedPassage(shortText, a.data()), "still working after a refusal");

    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "THERE ARE FAILURES");
    return failures == 0 ? 0 : 1;
}
#endif

#ifdef FILO_HAVE_LLAMA
// --- what the query model makes of a question -------------------------------
//
// Not a pass/fail test: there is no ground truth for "what did this person
// mean". It prints what the model decided so the decisions can be read, which
// is the only honest way to judge a component whose job is interpretation.
int runQueryModelTest(const std::wstring& modelPath) {
    filo::QueryModel model;
    std::wstring error;
    const auto loadStart = std::chrono::steady_clock::now();
    if (!model.ensureLoaded(modelPath, &error)) {
        std::printf("cannot load: %s\n", toUtf8(error).c_str());
        return 1;
    }
    std::printf("model loaded in %.1f s\n\n", secondsSince(loadStart));
    std::fflush(stdout);

    static const wchar_t* const kQuestions[] = {
        L"quanto ho pagato di tasse l'anno scorso",
        L"dove sta il contratto di noleggio che ho firmato",
        L"la fattura in pdf che ho scaricato ieri",
        L"where is the spreadsheet with the invoices",
        L"quel foglio excel delle tasse forfettarie",
        L"le foto delle vacanze",
        L"contratto",
        L"quanto pago se interrompo prima",
    };

    const int64_t now = filo::nowFileTime();
    for (const wchar_t* question : kQuestions) {
        filo::QueryPlan plan = filo::planQuery(question, now);
        const bool asked = filo::QueryModel::worthAsking(plan);

        double milliseconds = 0;
        if (asked) {
            const auto start = std::chrono::steady_clock::now();
            model.refine(question, now, &plan);
            milliseconds = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - start).count();
        }

        std::printf("  %s\n", toUtf8(question).c_str());
        if (!asked) {
            std::printf("      (not asked: the cheap pass already answered)\n");
        } else {
            std::printf("      reply   %s  [%.0f ms]\n", model.lastReply().c_str(),
                        milliseconds);
        }
        std::printf("      search  \"%s\"\n", toUtf8(plan.text).c_str());
        if (!plan.extensions.empty()) {
            std::string formats;
            for (const std::wstring& extension : plan.extensions) {
                if (!formats.empty()) formats += ' ';
                formats += toUtf8(extension);
            }
            std::printf("      format  %s\n", formats.c_str());
        }
        if (plan.after != 0 || plan.before != 0) std::printf("      dated   yes\n");
        std::printf("\n");
        std::fflush(stdout);
    }
    return 0;
}

// --- what the model makes of a file -----------------------------------------
//
// Also not a pass/fail test. The one place in Filo where the model writes prose
// nothing checks, so the only way to judge it is to read it — next to paths
// where the honest answer is "I don't recognise this one", which is the answer
// that matters most and the one a model is least inclined to give.
int runDescribeTest(const std::wstring& modelPath) {
    filo::QueryModel model;
    std::wstring error;
    if (!model.ensureLoaded(modelPath, &error)) {
        std::printf("cannot load: %s\n", toUtf8(error).c_str());
        return 1;
    }

    struct Sample {
        const wchar_t* path;
        uint64_t size;
    };
    static const Sample kFiles[] = {
        {L"C:\\Users\\you\\AppData\\Local\\Android\\Sdk\\system-images\\android-34\\"
         L"google_apis_playstore\\x86_64\\userdata-qemu.img", 11811160064ull},
        {L"C:\\Users\\you\\filo\\build\\CMakeFiles\\filo.dir\\src\\main.cpp.obj", 2411724ull},
        {L"C:\\Users\\you\\Downloads\\Win11_24H2_Italian_x64.iso", 6442450944ull},
        {L"C:\\Users\\you\\AppData\\Roaming\\Code\\Cache\\Cache_Data\\data_2", 536870912ull},
        {L"D:\\Games\\Frontier Saga\\datapc_common.forge", 4294967296ull},
        {L"C:\\Users\\you\\Documents\\contratto-noleggio-2025.pdf", 1258291ull},
        {L"C:\\Users\\you\\Documents\\zqx-4417.bin", 3221225472ull},
        {L"C:\\Users\\you\\AppData\\Local\\filo\\vectors.bin", 359661568ull},
    };

    for (const Sample& file : kFiles) {
        std::wstring answer;
        const auto start = std::chrono::steady_clock::now();
        const bool spoke = model.describe(file.path, file.size, &answer);
        const double milliseconds =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();

        std::printf("  %s\n", toUtf8(file.path).c_str());
        std::printf("      %s  [%.0f ms]\n\n",
                    spoke ? toUtf8(answer).c_str() : "(nothing)", milliseconds);
        std::fflush(stdout);
    }
    return 0;
}
#endif

// --- proving the isolation holds --------------------------------------------
//
// Crash containment never tried under failure is only a hope. Here the failures
// are caused on purpose: if one of these cases brings the coordinator down, or
// leaves stray processes behind, it is better to find out now than on a user's
// machine.
int runWorkerTest() {
    filo::ExtractorPool pool;
    std::wstring error;
    if (!pool.start(2, &error)) {
        std::printf("the worker pool failed to start: %s\n", toUtf8(error).c_str());
        return 1;
    }
    std::printf("pool started: %u workers\n\n", pool.workerCount());

    const std::string sample =
        "Contratto di noleggio.\n\nLa penale per il ritardo nella riconsegna "
        "e' pari al 10% del canone giornaliero.\n";

    struct Case {
        const char* name;
        uint32_t fault;
        bool expectSurvive;
    };
    const Case cases[] = {
        {"normal extraction",             0, true},
        {"access violation",              static_cast<uint32_t>(filo::FaultInjection::AccessViolation), true},
        {"extraction after the crash",    0, true},
        {"stack overflow",                static_cast<uint32_t>(filo::FaultInjection::StackOverflow), true},
        {"noncontinuable exception",      static_cast<uint32_t>(filo::FaultInjection::Abort), true},
        {"infinite loop",                 static_cast<uint32_t>(filo::FaultInjection::InfiniteLoop), true},
        {"final extraction",              0, true},
    };

    bool allPassed = true;
    for (const Case& testCase : cases) {
        std::string text;
        const auto start = std::chrono::steady_clock::now();
        const filo::WorkerStatus status = pool.extract(filo::ContentKind::PlainText,
                                                       sample.data(), sample.size(),
                                                       &text, testCase.fault);
        const double ms = secondsSince(start) * 1000.0;

        const bool coordinatorAlive = true;  // if we had died we would not be here
        const bool textOk = testCase.fault != 0 || (!text.empty() && status == filo::WorkerStatus::Ok);
        const bool passed = coordinatorAlive && (testCase.fault == 0 ? textOk : true);
        if (!passed) allPassed = false;

        std::printf("  %-28s -> %-18s %7.0f ms  %s\n", testCase.name,
                    toUtf8(filo::workerStatusName(status)).c_str(), ms,
                    passed ? "ok" : "FAILED");
    }

    const filo::PoolStats& stats = pool.stats();
    std::printf("\n  requests %llu · succeeded %llu · crashes %llu · timeouts %llu"
                " · restarts %llu\n",
                stats.requests, stats.ok, stats.crashes, stats.timeouts, stats.restarts);

    pool.stop();

    // No orphaned processes: that is what KILL_ON_JOB_CLOSE is checked for.
    std::printf("\n  pool closed. The coordinator is alive: %s\n", "yes");
    std::printf("  RESULT: %s\n", allPassed ? "OK" : "FAILED");
    return allPassed ? 0 : 1;
}

// --- indexing the content ---------------------------------------------------
//
// Reads the candidates, normalizes them, splits them into chunks and writes
// them. The expensive part (open, read, decode, chunk) happens on the reader
// threads; only the write to SQLite is serialized, because in WAL there is one
// writer by definition and more connections do not raise the throughput: they
// produce SQLITE_BUSY.
int runIndexContent(const filo::FileIndex& index, wchar_t drive) {
    const std::wstring path = contentDbPath(drive);
    filo::ContentDb db;
    std::wstring error;
    if (!db.open(path, false, &error)) {
        std::printf("cannot open the database: %s\n", toUtf8(error).c_str());
        return 1;
    }

    // Files deleted since the last run go first. Doing it here rather than in a
    // command the user has to remember is what keeps the index from growing
    // without bound on a machine nobody maintains.
    {
        uint64_t purgedDocs = 0, purgedVectors = 0;
        std::wstring purgeError;
        if (purgeDeletedDocuments(index, drive, db, &purgedDocs, &purgedVectors,
                                  &purgeError)) {
            if (purgedDocs > 0) {
                std::printf("%s deleted files removed from the index (%s vectors)\n",
                            groupDigits(purgedDocs).c_str(),
                            groupDigits(purgedVectors).c_str());
            }
        } else {
            std::printf("WARNING: purge skipped (%s)\n", toUtf8(purgeError).c_str());
        }
    }

    filo::ContentWriter writer(db);
    if (!writer.prepare(&error)) {
        std::printf("prepare failed: %s\n", toUtf8(error).c_str());
        return 1;
    }

    const uint32_t volumeId = filo::volumeSerial(drive);

    filo::SelectionStats selection;
    const std::vector<uint32_t> allCandidates = filo::selectCandidates(index, &selection);

    // Skip the unchanged files BEFORE reading them.
    //
    // The check used to happen after: every candidate was opened, read, hashed
    // and run through an extractor, and only then did the database get asked
    // whether that work was needed. The "unchanged, skipped" counter was
    // therefore a consolation prize — those files had paid the entire cost
    // except the write.
    //
    // Size plus modification time plus extractor version answers it from
    // metadata alone, which is a stat instead of an open-read-parse. Files
    // whose timestamp lies still get caught by the content hash further down,
    // so this is a fast path, not a weaker guarantee.
    struct KnownDoc {
        uint64_t size = 0;
        int64_t  mtime = 0;
        int64_t  extractorVersion = 0;
    };
    std::unordered_map<uint64_t, KnownDoc> known;
    {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db.handle(),
                               "SELECT frn, size, mtime, extractor_ver FROM doc "
                               "WHERE volume_id = ?",
                               -1, &statement, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(statement, 1, static_cast<int64_t>(volumeId));
            while (sqlite3_step(statement) == SQLITE_ROW) {
                known.emplace(static_cast<uint64_t>(sqlite3_column_int64(statement, 0)),
                              KnownDoc{static_cast<uint64_t>(sqlite3_column_int64(statement, 1)),
                                       sqlite3_column_int64(statement, 2),
                                       sqlite3_column_int64(statement, 3)});
            }
            sqlite3_finalize(statement);
        }
    }

    std::vector<uint32_t> candidates;
    candidates.reserve(allCandidates.size());
    uint64_t unchangedByMetadata = 0;
    if (known.empty()) {
        candidates = allCandidates;   // first run: nothing to compare against
    } else {
        for (const uint32_t slot : allCandidates) {
            const auto it = known.find(index.frn(slot));
            if (it != known.end() && it->second.extractorVersion == filo::kExtractorVersion) {
                filo::FileFacts facts;
                if (filo::readFileFacts(index.fullPath(slot).c_str(), &facts) &&
                    facts.size == it->second.size && facts.mtime == it->second.mtime) {
                    ++unchangedByMetadata;
                    continue;
                }
            }
            candidates.push_back(slot);
        }
    }

    if (unchangedByMetadata) {
        std::printf("%s files already indexed and unchanged: skipped without opening\n",
                    groupDigits(unchangedByMetadata).c_str());
    }
    std::printf("indexing content of %s files...\n\n",
                groupDigits(candidates.size()).c_str());

    // The worker pool for the container formats. DOCX, XLSX and PPTX go through
    // here and not through the main process: a malformed archive takes down one
    // worker at most, and it restarts itself.
    filo::ExtractorPool pool;
    const unsigned poolSize =
        std::max(2u, std::max(1u, std::thread::hardware_concurrency()) / 4);
    std::wstring poolError;
    const bool havePool = pool.start(poolSize, &poolError);
    if (!havePool) {
        std::printf("WARNING: workers not started (%s), Office documents will "
                    "be skipped\n", toUtf8(poolError).c_str());
    }

    std::mutex writeMutex;
    std::atomic<uint64_t> indexed{0}, skippedUnchanged{0}, updated{0};
    std::atomic<uint64_t> notText{0}, emptyText{0};
    std::atomic<uint64_t> office{0}, officeEncrypted{0}, officeFailed{0}, needsOcr{0};
    std::atomic<uint64_t> totalChunks{0}, totalTextBytes{0};
    std::wstring writeError;

    const auto start = std::chrono::steady_clock::now();

    const filo::ReaderStats readerStats = filo::ContentReader::run(
        index, candidates, {},
        [&](const filo::ReadResult& result) {
            if (result.verdict != filo::GateVerdict::Pass || !result.data) return;

            // The security boundary runs through here: what stays in the main
            // process is only what has no parser to isolate — validate UTF-8
            // and copy. Everything else does offset arithmetic on bytes of
            // arbitrary origin and goes into the worker.
            const bool isText = result.actualKind == filo::ContentKind::PlainText ||
                                result.actualKind == filo::ContentKind::Code;
            const bool isContainer = result.actualKind == filo::ContentKind::OfficeXml ||
                                     result.actualKind == filo::ContentKind::OpenDocument ||
                                     result.actualKind == filo::ContentKind::Markup ||
                                     result.actualKind == filo::ContentKind::RichText ||
                                     result.actualKind == filo::ContentKind::Ebook ||
#if FILO_HAVE_PDFIUM
                                     result.actualKind == filo::ContentKind::Pdf ||
#endif
                                     false;

            std::string text;
            if (isText) {
                // Plain text stays in the main process: there is no parser to
                // isolate, only validation and a copy, and routing it through
                // the worker would be pure added cost.
                if (!filo::normalizeToUtf8(result.data, result.dataSize, result.encoding,
                                           result.actualKind, &text)) {
                    ++emptyText;
                    return;
                }
            } else if (isContainer && havePool) {
                const filo::WorkerStatus status = pool.extract(
                    result.actualKind, result.data, result.dataSize, &text);
                if (status == filo::WorkerStatus::Encrypted) { ++officeEncrypted; return; }
                if (status == filo::WorkerStatus::NeedsOcr) { ++needsOcr; return; }
                // "No text" is not a failure: an SVG icon or an XML config
                // file holds no prose, and that is normal. Confusing them with
                // errors would hide the real problems.
                if (status == filo::WorkerStatus::Empty || text.empty()) {
                    ++emptyText;
                    return;
                }
                if (status != filo::WorkerStatus::Ok &&
                    status != filo::WorkerStatus::TooBig) {
                    ++officeFailed;
                    return;
                }
                ++office;
            } else {
                // PDF and legacy formats: still waiting for their extractor.
                ++notText;
                return;
            }
            const std::vector<filo::ChunkRecord> chunks = filo::chunkText(text);
            if (chunks.empty()) { ++emptyText; return; }

            filo::DocumentRecord document;
            document.volumeId = volumeId;
            document.frn = index.frn(result.slot);
            document.size = result.size;
            document.mtime = result.mtime;
            document.contentHashLow = result.hashLow;
            document.contentHashHigh = result.hashHigh;
            document.kind = static_cast<uint8_t>(result.actualKind);
            document.state = filo::DocState::Ok;
            // To be bumped every time an extractor changes behaviour: it is
            // what makes documents already in the index be redone even though
            // the file is identical. Version 2: HTML, RTF and EPUB moved to
            // dedicated extractors inside the worker.
            document.extractorVersion = static_cast<uint32_t>(filo::kExtractorVersion);
            document.rankPenalty = filo::contentRankPenalty(index.fullPath(result.slot));

            std::lock_guard<std::mutex> lock(writeMutex);

            // Already there: skipped ONLY if the content is identical, and the
            // proof is the XXH3-128 hash of the bytes, which the reader
            // computes for exactly this. Stopping at "the row exists" would
            // freeze the index: every file edited after its first indexing
            // would stay searchable only through its old text.
            filo::ContentWriter::ExistingDoc existing;
            std::wstring localError;
            if (writer.findDocument(volumeId, document.frn, &existing, &localError) &&
                existing.docId >= 0) {
                if (existing.contentHashLow == document.contentHashLow &&
                    existing.contentHashHigh == document.contentHashHigh &&
                    existing.extractorVersion == document.extractorVersion) {
                    ++skippedUnchanged;
                    return;
                }
                // Different content: out with the old, then in with the new.
                if (!writer.deleteDocument(existing.docId, &localError)) {
                    if (writeError.empty()) writeError = localError;
                    return;
                }
                ++updated;
            }

            if (!writer.addDocument(document, text, chunks, nullptr, &localError)) {
                if (writeError.empty()) writeError = localError;
                return;
            }
            ++indexed;
            totalChunks += chunks.size();
            totalTextBytes += text.size();
        });

    if (!writer.commitBatch(&error) && writeError.empty()) writeError = error;
    // Folds the whole write-ahead log back into the database: without it the
    // -wal file stays as big as the entire indexing session.
    db.checkpoint(true);
    const double seconds = secondsSince(start);

    if (!writeError.empty()) {
        std::printf("WARNING: write errors (%s)\n", toUtf8(writeError).c_str());
    }

    std::printf("  documents indexed       %10s\n", groupDigits(indexed).c_str());
    std::printf("  chunks written          %10s\n", groupDigits(totalChunks).c_str());
    std::printf("  text extracted          %10.1f MB\n",
                totalTextBytes / (1024.0 * 1024.0));
    std::printf("  unchanged (skipped)     %10s\n", groupDigits(skippedUnchanged).c_str());
    std::printf("  changed (redone)        %10s\n", groupDigits(updated).c_str());
    std::printf("  extracted in the worker %10s   (office, html, rtf, epub)\n",
                groupDigits(office).c_str());
    std::printf("    password protected    %10s\n", groupDigits(officeEncrypted).c_str());
    std::printf("    parser failed         %10s\n", groupDigits(officeFailed).c_str());
    std::printf("    images only (OCR)     %10s   (scanned pdfs)\n",
                groupDigits(needsOcr).c_str());
    std::printf("  waiting for extractor   %10s   (pdf, legacy office)\n",
                groupDigits(notText).c_str());
    const filo::PoolStats& poolStats = pool.stats();
    if (poolStats.requests) {
        std::printf("  workers: %llu requests, %llu crashes, %llu timeouts, "
                    "%llu restarts\n",
                    poolStats.requests, poolStats.crashes, poolStats.timeouts,
                    poolStats.restarts);
    }
    std::printf("  no usable text          %10s\n", groupDigits(emptyText).c_str());
    std::printf("\n  time %.1f s (%s files/s, %u threads)\n", seconds,
                groupDigits(static_cast<uint64_t>(readerStats.filesSeen / seconds)).c_str(),
                readerStats.threadsUsed);
    std::printf("  database %.1f MB\n", db.fileBytes() / (1024.0 * 1024.0));
    return 0;
}

// --- how many chunks are DUPLICATES -----------------------------------------
//
// Computing an embedding depends only on the bytes of the chunk: two identical
// chunks give the same vector. On a real disk a great many of them repeat —
// licence headers, template boilerplate, backup copies of the same website —
// and every one of those is half a second of wasted work.
//
// This measurement redoes the extraction without writing anything: it counts
// how many distinct chunks there really are.
int runDuplicateStats(const filo::FileIndex& index) {
    filo::SelectionStats selection;
    const std::vector<uint32_t> candidates = filo::selectCandidates(index, &selection);

    std::mutex mutex;
    std::unordered_set<uint64_t> distinti;
    uint64_t totali = 0, byteTotali = 0, byteDistinti = 0;

    const auto start = std::chrono::steady_clock::now();
    filo::ContentReader::run(
        index, candidates, {},
        [&](const filo::ReadResult& result) {
            if (result.verdict != filo::GateVerdict::Pass || !result.data) return;
            const bool isText = result.actualKind == filo::ContentKind::PlainText ||
                                result.actualKind == filo::ContentKind::Code ||
                                result.actualKind == filo::ContentKind::Markup;
            if (!isText) return;   // plain text is enough for the estimate

            std::string text;
            if (!filo::normalizeToUtf8(result.data, result.dataSize, result.encoding,
                                       result.actualKind, &text)) {
                return;
            }
            for (const filo::ChunkRecord& chunk : filo::chunkText(text)) {
                const uint64_t hash = XXH3_64bits(text.data() + chunk.byteOffset,
                                                  chunk.byteLength);
                std::lock_guard<std::mutex> lock(mutex);
                ++totali;
                byteTotali += chunk.byteLength;
                if (distinti.insert(hash).second) byteDistinti += chunk.byteLength;
            }
        });

    const double seconds = secondsSince(start);
    const uint64_t duplicati = totali - distinti.size();
    std::printf("total chunks      %12s\n", groupDigits(totali).c_str());
    std::printf("distinct chunks   %12s\n", groupDigits(distinti.size()).c_str());
    std::printf("duplicates        %12s  (%.1f%%)\n", groupDigits(duplicati).c_str(),
                100.0 * duplicati / (totali ? totali : 1));
    std::printf("\nwork that reusing the vectors would save: %.1f%%\n",
                100.0 * duplicati / (totali ? totali : 1));
    std::printf("(measured in %.1f s)\n", seconds);
    return 0;
}

// --- what the index is made of ----------------------------------------------
//
// How many chunks there are per document type. It decides WHAT is worth putting
// through the model: semantic search changes everything on an invoice or a
// contract, while in a source file you look for an exact identifier, and that
// is a task word search is already better at.
int runChunkStats(wchar_t drive) {
    filo::ContentDb db;
    std::wstring error;
    if (!db.open(contentDbPath(drive), true, &error)) {
        std::printf("no content index\n");
        return 1;
    }

    sqlite3_stmt* statement = nullptr;
    const char* kSql =
        "SELECT doc.kind, COUNT(DISTINCT doc.doc_id), COUNT(chunk.chunk_id) "
        "FROM doc LEFT JOIN chunk ON chunk.doc_id = doc.doc_id GROUP BY doc.kind";
    if (sqlite3_prepare_v2(db.handle(), kSql, -1, &statement, nullptr) != SQLITE_OK) {
        std::printf("query failed\n");
        return 1;
    }

    std::printf("%-20s %10s %12s\n", "type", "documents", "chunks");
    uint64_t proseChunks = 0, codeChunks = 0, totalChunks = 0;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const auto kind = static_cast<filo::ContentKind>(sqlite3_column_int(statement, 0));
        const uint64_t docs = static_cast<uint64_t>(sqlite3_column_int64(statement, 1));
        const uint64_t chunks = static_cast<uint64_t>(sqlite3_column_int64(statement, 2));
        totalChunks += chunks;
        if (kind == filo::ContentKind::Code) codeChunks += chunks;
        else proseChunks += chunks;

        std::printf("%-20s %10s %12s\n", toUtf8(filo::kindName(kind)).c_str(),
                    groupDigits(docs).c_str(), groupDigits(chunks).c_str());
    }
    sqlite3_finalize(statement);

    std::printf("\n  prose   %12s chunks  (%.0f%%)\n", groupDigits(proseChunks).c_str(),
                100.0 * proseChunks / (totalChunks ? totalChunks : 1));
    std::printf("  code    %12s chunks  (%.0f%%)\n", groupDigits(codeChunks).c_str(),
                100.0 * codeChunks / (totalChunks ? totalChunks : 1));
    // What is left after taking out machine-generated content (which the rank
    // penalty already marks) and source code.
    sqlite3_stmt* refined = nullptr;
    const char* kRefined =
        "SELECT COUNT(chunk.chunk_id) FROM doc JOIN chunk ON chunk.doc_id = doc.doc_id "
        "WHERE doc.rank_penalty = 0 AND doc.kind != ?";
    uint64_t worthwhile = 0;
    if (sqlite3_prepare_v2(db.handle(), kRefined, -1, &refined, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(refined, 1, static_cast<int>(filo::ContentKind::Code));
        if (sqlite3_step(refined) == SQLITE_ROW) {
            worthwhile = static_cast<uint64_t>(sqlite3_column_int64(refined, 0));
        }
        sqlite3_finalize(refined);
    }

    std::printf("\n  prose written by people: %s chunks\n"
                "  (code and generated content excluded)\n",
                groupDigits(worthwhile).c_str());
    // Measured with --embedbench on the 97m r2 model over Vulkan, on 2 KB
    // chunks, warm. The number this replaced was 447 ms, left over from a CPU
    // run before the GPU backend existed; it had been quietly turning a
    // twenty-minute job into a six-hour forecast ever since.
    constexpr double kSecondsPerChunk = 0.024;
    std::printf("\n  at %.0f ms per chunk (measured, GPU, warm):\n", kSecondsPerChunk * 1000);
    std::printf("    everything             %5.0f min\n", totalChunks * kSecondsPerChunk / 60.0);
    std::printf("    prose only             %5.0f min\n", proseChunks * kSecondsPerChunk / 60.0);
    std::printf("    prose, not generated   %5.0f min\n", worthwhile * kSecondsPerChunk / 60.0);
    return 0;
}

// --- searching the content --------------------------------------------------
int runContentSearch(const filo::FileIndex& index, wchar_t drive,
                     const std::wstring& query) {
    filo::ContentDb db;
    std::wstring error;
    if (!db.open(contentDbPath(drive), true, &error)) {
        std::printf("no content index: run Filo.exe --index first\n");
        return 1;
    }

    const std::string queryUtf8 = toUtf8(query);
    double milliseconds = 0;
    const std::vector<filo::ContentHit> hits =
        filo::searchContent(db, queryUtf8, 15, &milliseconds);

    std::printf("\"%s\" in the CONTENT: %s documents in %.1f ms\n\n",
                queryUtf8.c_str(), groupDigits(hits.size()).c_str(), milliseconds);

    for (const filo::ContentHit& hit : hits) {
        const uint32_t slot = index.indexOfFrn(hit.frn);
        const bool gone = slot == filo::kNoParent || index.isDeleted(slot);
        const std::wstring where =
            gone ? L"(file no longer present)" : index.fullPath(slot);
        std::printf("  [%6.2f] %dx  %s\n", hit.score, hit.passages,
                    toUtf8(where).c_str());
    }
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // The worker role is recognized BEFORE anything else: it is the same
    // executable, but it must not load indexes, open databases or show windows.
    // All it has to do is listen on its channel.
    if (argc >= 3 && std::wstring(argv[1]) == L"--extractor-worker") {
        return filo::workerMain(argv[2]);
    }

    SetConsoleOutputCP(CP_UTF8);

    if (argc >= 3 && std::wstring(argv[1]) == L"--extract") {
        return runExtractOne(argv[2]);
    }
    // Neither of these needs a model, and they are how anyone checks a build is
    // sound. They used to sit inside the llama.cpp block below, so a clone
    // without llama.cpp had no way to verify itself at all: --selftest was not
    // a recognised argument, fell through to normal operation, and failed
    // trying to open the volume.
    if (argc >= 2 && std::wstring(argv[1]) == L"--selftest") {
        const int indexResult = runSelfTest();
#if FILO_HAVE_LLAMA
        // With a model path the tokenizer checks run too; without one the
        // index checks still stand on their own.
        if (argc >= 3) return runTokenizerTest(argv[2]) | indexResult;
#endif
        return indexResult;
    }
    if (argc >= 2 && std::wstring(argv[1]) == L"--bintest") {
        return runBinTest();
    }
#if FILO_HAVE_LLAMA
    if (argc >= 3 && std::wstring(argv[1]) == L"--embedtest") {
        return runEmbedTest(argv[2]);
    }
    if (argc >= 3 && std::wstring(argv[1]) == L"--asktest") {
        return runQueryModelTest(argv[2]);
    }
    if (argc >= 3 && std::wstring(argv[1]) == L"--describetest") {
        return runDescribeTest(argv[2]);
    }
    if (argc >= 3 && std::wstring(argv[1]) == L"--embedbench") {
        return runEmbedBench(argv[2]);
    }
    if (argc >= 3 && std::wstring(argv[1]) == L"--evalmodel") {
        return runModelEval(argv[2]);
    }
#endif

    wchar_t drive = L'C';
    std::wstring query;
    bool forceRescan = false;
    bool showCandidates = false;
    bool forceUi = false;
    bool runReadBenchmark = false;
    bool runIndexing = false;
    bool contentSearch = false;
    bool showChunkStats = false;
    bool showDupStats = false;
    std::wstring embedModelPath;
    std::wstring uiModelPath;
    std::wstring questionModelPath;
    bool runRepenalty = false;
    bool runPurging = false;
    bool runReclaimMeasure = false;
    bool runReclaimDuplicates = false;
    bool runReclaimClassify = false;
    bool applyChanges = false;
    int64_t embedChunkLimit = 0;   // 0 = no limit

    std::vector<std::wstring> positional;
    for (int i = 1; i < argc; ++i) {
        const std::wstring argument = argv[i];
        if (argument == L"--rescan") {
            forceRescan = true;
        } else if (argument == L"--candidates") {
            showCandidates = true;
        } else if (argument == L"--dbtest") {
            return runDbTest();
        } else if (argument == L"--workertest") {
            return runWorkerTest();
        } else if (argument == L"--ui") {
            forceUi = true;
        } else if (argument == L"--readtest") {
            runReadBenchmark = true;
        } else if (argument == L"--index") {
            runIndexing = true;
        } else if (argument == L"--find") {
            contentSearch = true;
        } else if (argument == L"--chunkstats") {
            showChunkStats = true;
        } else if (argument == L"--dupstats") {
            showDupStats = true;
        } else if (argument == L"--embed" && i + 1 < argc) {
            embedModelPath = argv[++i];
        } else if (argument == L"--repenalize") {
            runRepenalty = true;
        } else if (argument == L"--purge") {
            runPurging = true;
        } else if (argument == L"--reclaimscan") {
            runReclaimMeasure = true;
        } else if (argument == L"--reclaimdupes") {
            runReclaimDuplicates = true;
        } else if (argument == L"--reclaimtop") {
            runReclaimClassify = true;
        } else if (argument == L"--apply") {
            // Nothing is ever moved without this. A dry run is the only
            // default a feature that deletes is allowed to have.
            applyChanges = true;
        } else if (argument == L"--question-model" && i + 1 < argc) {
            // The generative model for layer 5. Separate from --model, which is
            // the embedding model: they are different sizes, different jobs and
            // one of them is optional in a way the other is not.
            questionModelPath = argv[++i];
        } else if (argument == L"--model" && i + 1 < argc) {
            // Where the WINDOW looks for the embedding model. Deliberately not
            // embedModelPath: that one selects the --embed mode and would start
            // an indexing run instead of opening the UI.
            uiModelPath = argv[++i];
        } else if (argument == L"--limit" && i + 1 < argc) {
            // Stop after this many chunks and print the full breakdown. A
            // profile has to end on its own to report anything: killing the
            // process for time takes the numbers with it.
            embedChunkLimit = _wtoi64(argv[++i]);
        } else {
            positional.push_back(argument);
        }
    }
    if (!positional.empty() && !positional[0].empty()) {
        drive = static_cast<wchar_t>(towupper(positional[0][0]));
    }
    if (positional.size() > 1) query = positional[1];

    // A window is coming, so nothing gets printed. Hiding the console is not
    // the same thing and was not enough: the text still goes to stdout, and if
    // the program was started from a terminal the user already had open, that
    // window belongs to THEM and hiding it is rude. So the console is hidden
    // only when we are its only client — meaning we created it by being
    // double-clicked — and the messages are simply not written either way.
    g_quiet = (query.empty() || forceUi) && !showCandidates;
    if (g_quiet) {
        DWORD owners[2] = {};
        if (GetConsoleProcessList(owners, 2) == 1) {
            if (HWND console = GetConsoleWindow()) ShowWindow(console, SW_HIDE);
        }
    }

    const std::wstring snapshot = snapshotPath(drive);
    filo::FileIndex index;
    bool indexChanged = false;

    // --- loading -------------------------------------------------------------
    bool haveIndex = false;
    if (!forceRescan) {
        const auto start = std::chrono::steady_clock::now();
        std::wstring loadError;
        if (index.load(snapshot, &loadError)) {
            haveIndex = true;
            say("snapshot: %s records in %.0f ms\n",
                        groupDigits(index.size()).c_str(), secondsSince(start) * 1000.0);
        } else {
            say("snapshot unusable (%s)\n", toUtf8(loadError).c_str());
        }
    }

    // --- incremental catch-up ------------------------------------------------
    if (haveIndex) {
        filo::UsnReader reader(drive);
        filo::CatchupStats catchup;
        std::wstring catchupError;

        if (!reader.catchUp(&index, &catchup, &catchupError)) {
            // Typically: no elevation. The loaded index stays valid, just a
            // little out of date: you can still search it.
            say("journal not read (%s) - index not up to date\n",
                        toUtf8(catchupError).c_str());
        } else if (catchup.needsFullScan) {
            say("journal can no longer be aligned: a full scan is needed\n");
            haveIndex = false;
        } else {
            const uint64_t applied = catchup.created + catchup.deleted + catchup.updated;
            say("journal: %s entries read in %.0f ms",
                groupDigits(catchup.recordsRead).c_str(),
                catchup.elapsedSeconds * 1000.0);
            if (applied > 0) {
                say(" -> %llu new, %llu removed, %llu changed", catchup.created,
                    catchup.deleted, catchup.updated);
                indexChanged = true;
            } else {
                say(" -> already aligned");
            }
            say("\n");
        }
    }

    if (!haveIndex) {
        if (!fullScan(drive, &index)) return 1;
        indexChanged = true;
    }

    // --- saving --------------------------------------------------------------
    if (indexChanged) {
        // Past a certain share of tombstones compacting pays off: it frees
        // memory and shortens the chains of records sharing one MFT slot. Below
        // that threshold it is not worth the cost.
        if (index.deletedCount() * 20 > index.size()) {
            const auto start = std::chrono::steady_clock::now();
            const size_t removed = index.deletedCount();
            index.compact();
            say("compacted: %s tombstones removed in %.0f ms\n",
                        groupDigits(removed).c_str(), secondsSince(start) * 1000.0);
        }

        // Journal arrivals sit at the end, outside the sorted region: before
        // writing we rebuild the FRN lookup structure, or on the next start
        // their parents would not be resolvable.
        index.rebuildFrnOrder();

        const auto start = std::chrono::steady_clock::now();
        std::wstring saveError;
        if (index.save(snapshot, &saveError)) {
            say("snapshot saved in %.0f ms\n", secondsSince(start) * 1000.0);
        } else {
            say("WARNING: snapshot not saved (%s)\n",
                        toUtf8(saveError).c_str());
        }
    }

    say("index: %s records (%s buried), %.1f MB\n\n",
                groupDigits(index.size()).c_str(),
                groupDigits(index.deletedCount()).c_str(),
                index.memoryBytes() / (1024.0 * 1024.0));

    if (runReadBenchmark) {
        return runReadTest(index, {8, 16, 32, 48});
    }
    if (showChunkStats) {
        return runChunkStats(drive);
    }
    if (showDupStats) {
        return runDuplicateStats(index);
    }
#if FILO_HAVE_LLAMA
    if (!embedModelPath.empty()) {
        return runEmbedIndex(index, drive, embedModelPath, embedChunkLimit);
    }
#endif
    if (runReclaimMeasure) {
        return runReclaimScan(index);
    }
    if (runReclaimClassify) {
        return runReclaimTop(index);
    }
    if (runReclaimDuplicates) {
        return runReclaimDupes(index, drive, applyChanges);
    }
    if (runPurging) {
        return runPurge(index, drive);
    }
    if (runRepenalty) {
        return runRepenalize(index, drive);
    }
    if (runIndexing) {
        return runIndexContent(index, drive);
    }
    if (contentSearch) {
        if (query.empty()) {
            std::printf("usage: Filo.exe C: \"la penale\" --find\n");
            return 1;
        }
        return runContentSearch(index, drive, query);
    }

    // --- selecting the extraction candidates ---------------------------------
    if (showCandidates) {
        filo::SelectionStats selection;
        const std::vector<uint32_t> candidates =
            filo::selectCandidates(index, &selection);

        std::printf("extraction candidates: %s of %s (%.1f%%) in %.0f ms\n\n",
                    groupDigits(selection.candidates).c_str(),
                    groupDigits(selection.total).c_str(),
                    100.0 * static_cast<double>(selection.candidates) /
                        static_cast<double>(selection.total ? selection.total : 1),
                    selection.elapsedSeconds * 1000.0);

        std::printf("  by content type\n");
        for (size_t k = 1; k < 16; ++k) {
            if (selection.byKind[k] == 0) continue;
            std::printf("    %-18s %12s\n",
                        toUtf8(filo::kindName(static_cast<filo::ContentKind>(k))).c_str(),
                        groupDigits(selection.byKind[k]).c_str());
        }

        std::printf("\n  discarded\n");
        for (size_t r = 1; r < 8; ++r) {
            if (selection.byReason[r] == 0) continue;
            std::printf("    %-30s %12s\n",
                        toUtf8(filo::reasonName(static_cast<filo::SkipReason>(r))).c_str(),
                        groupDigits(selection.byReason[r]).c_str());
        }
        std::printf("\n");

        // A taste of what will actually reach the extractor.
        std::printf("  examples\n");
        for (size_t i = 0; i < candidates.size() && i < 8; ++i) {
            const size_t step = candidates.size() / 8 ? candidates.size() / 8 : 1;
            const size_t pick = i * step;
            if (pick >= candidates.size()) break;
            std::printf("    %s\n", toUtf8(index.fullPath(candidates[pick])).c_str());
        }
        std::printf("\n");

        // The overall distribution on this disk is a DEVELOPER's and does not
        // represent the end user. The document folders are the slice that looks
        // most like them: look in there.
        std::vector<UserArea> areas = buildUserAreas();
        uint64_t elsewhere = 0;
        uint64_t elsewhereByKind[16] = {};

        for (const uint32_t slot : candidates) {
            const std::wstring path = filo::toLowerCopy(index.fullPath(slot));
            const size_t kind =
                static_cast<size_t>(filo::classifyByName(index.name(slot)));

            bool matched = false;
            for (UserArea& area : areas) {
                if (path.compare(0, area.prefix.size(), area.prefix) == 0) {
                    ++area.byKind[kind];
                    ++area.total;
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                ++elsewhereByKind[kind];
                ++elsewhere;
            }
        }

        std::printf("  distribution in the document folders (proxy for a normal user)\n");
        uint64_t documentAreaTotal = 0;
        uint64_t documentAreaByKind[16] = {};
        for (const UserArea& area : areas) {
            if (area.total == 0) continue;
            documentAreaTotal += area.total;
            std::printf("    %-12s %8s   ", toUtf8(area.label).c_str(),
                        groupDigits(area.total).c_str());
            for (size_t k = 1; k < 16; ++k) {
                if (area.byKind[k] == 0) continue;
                documentAreaByKind[k] += area.byKind[k];
                std::printf("%s:%s  ",
                            toUtf8(filo::kindName(static_cast<filo::ContentKind>(k))).c_str(),
                            groupDigits(area.byKind[k]).c_str());
            }
            std::printf("\n");
        }

        std::printf("\n    TOTAL for the document folders: %s candidates\n",
                    groupDigits(documentAreaTotal).c_str());
        for (size_t k = 1; k < 16; ++k) {
            if (documentAreaByKind[k] == 0) continue;
            std::printf("      %-18s %8s  (%.1f%%)\n",
                        toUtf8(filo::kindName(static_cast<filo::ContentKind>(k))).c_str(),
                        groupDigits(documentAreaByKind[k]).c_str(),
                        100.0 * static_cast<double>(documentAreaByKind[k]) /
                            static_cast<double>(documentAreaTotal ? documentAreaTotal : 1));
        }
        std::printf("\n    elsewhere (projects, tools, games): %s candidates\n",
                    groupDigits(elsewhere).c_str());
        for (size_t k = 1; k < 16; ++k) {
            if (elsewhereByKind[k] == 0) continue;
            std::printf("      %-18s %8s\n",
                        toUtf8(filo::kindName(static_cast<filo::ContentKind>(k))).c_str(),
                        groupDigits(elsewhereByKind[k]).c_str());
        }
        std::printf("\n");
    }

    if (query.empty() || forceUi) {
        if (showCandidates) return 0;

        // No query on the command line: that is normal use, so open the window.
        wchar_t statusBuffer[256];
        swprintf_s(statusBuffer, L"%s files indexed · %.0f MB in memory",
                   toWide(groupDigits(index.size())).c_str(),
                   index.memoryBytes() / (1024.0 * 1024.0));

        // The content index is optional: if it has not been built yet the
        // window still works and searches names only.
        filo::ContentDb content;
        std::wstring contentError;
        const bool haveContent = content.open(contentDbPath(drive), true, &contentError);
        if (haveContent) {
            swprintf_s(statusBuffer,
                       L"%s files · content indexed · %.0f MB in memory",
                       toWide(groupDigits(index.size())).c_str(),
                       index.memoryBytes() / (1024.0 * 1024.0));
        }

        filo::WebViewWindow window;
        const FoundModels models = findModels();

        // Semantic search, if a model and a matching vector file are both
        // present. Each layer here is optional in the same way: names always
        // work, content works once indexed, meaning works once embedded.
        if (haveContent) {
            const std::wstring modelPath =
                uiModelPath.empty() ? models.embedding : uiModelPath;
            std::wstring semanticError;
            if (!modelPath.empty() &&
                window.loadSemantic(modelPath, contentDbPath(drive) + L".vec",
                                    static_cast<uint32_t>(content.metaInt("content_gen", 0)),
                                    &semanticError)) {
                swprintf_s(statusBuffer,
                           L"%s files · content + meaning indexed · %.0f MB in memory",
                           toWide(groupDigits(index.size())).c_str(),
                           index.memoryBytes() / (1024.0 * 1024.0));
            }
        }

        // Layer 5, if a generative model is around. Not loaded yet: the window
        // only opens it the first time a query actually reads like a question.
        window.useQuestionModel(questionModelPath.empty() ? models.question
                                                          : questionModelPath);

        // What to do once files really reach the Recycle Bin.
        //
        // Filo moved them, so it already knows: no rescan, no USN journal, and
        // no administrator rights needed to find out. Without this the index
        // keeps answering with their old paths for the rest of the session, and
        // on a machine that cannot read the journal, until somebody rebuilds it
        // by hand — which is not something to ask of a person who just emptied
        // some space.
        //
        // Runs on the reclaim thread with the index held for writing.
        window.onRecycled([&index, &content, drive, haveContent,
                           snapshot = snapshotPath(drive)](
                              const std::vector<uint32_t>& slots) {
            for (const uint32_t slot : slots) index.markDeleted(slot);

            // Saved straight away. A crash between the deletion and the next
            // clean exit would otherwise leave an index that lists files which
            // are already in the bin, and nothing would ever correct it.
            std::wstring error;
            if (!index.save(snapshot, &error)) return;
            if (!haveContent) return;

            // The text and the vectors go too. purgeDeletedDocuments works from
            // what the index now says is dead, which is exactly what was just
            // marked, and it removes vectors before rows for the reason set out
            // where it is defined.
            uint64_t docs = 0, vectors = 0;
            purgeDeletedDocuments(index, drive, content, &docs, &vectors, &error);
        });

        // Following the disk while the window is open.
        //
        // The journal used to be read once, here at startup, and never again: a
        // file another program created during the session did not exist as far
        // as Filo was concerned until the next launch. Every thirty seconds is
        // cheap because the common answer is "nothing happened" and costs a
        // single call.
        //
        // Returning -1 means the journal cannot be read at all, which on an
        // ordinary account is not a fault but the normal state. The window
        // answers that by watching folders instead — which notices changes
        // without any rights, and says the index is out of date rather than
        // quietly serving results from a stale one.
        window.refreshWith(
            [&index, drive, snapshot = snapshotPath(drive)]() -> int64_t {
                filo::UsnReader reader(drive);
                filo::CatchupStats catchup;
                std::wstring error;
                if (!reader.catchUp(&index, &catchup, &error)) return -1;
                if (catchup.needsFullScan) return -1;

                const int64_t applied = static_cast<int64_t>(
                    catchup.created + catchup.deleted + catchup.updated);
                // Written down straight away. An index held only in memory is
                // an index that loses a session's worth of changes to one crash.
                if (applied > 0) index.save(snapshot, &error);
                return applied;
            },
            /*everySeconds=*/30);

        // Where to watch when the journal is out of reach. The profile rather
        // than the volume: it needs no rights, and it is where the files a
        // person searches for actually live.
        {
            std::vector<std::wstring> roots;
            wchar_t profile[MAX_PATH] = {};
            if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) > 0) {
                roots.push_back(profile);
            }
            window.watchFolders(std::move(roots));
        }

        std::wstring uiError;
        if (!window.create(&index, haveContent ? &content : nullptr, statusBuffer, query,
                           &uiError)) {
            std::printf("the window did not start: %s\n", toUtf8(uiError).c_str());
            return 1;
        }
        return window.run();
    }

    // --- searching -----------------------------------------------------------
    const std::wstring needle = filo::toLowerCopy(query);

    // We run the search several times and keep the best time: a single
    // measurement is dirtied by the scheduler and by the state of the CPU cache.
    double bestSeconds = 1e9;
    std::vector<uint32_t> hits;
    for (int run = 0; run < kSearchRuns; ++run) {
        const auto start = std::chrono::steady_clock::now();
        hits = filo::searchNames(index, needle);
        bestSeconds = std::min(bestSeconds, secondsSince(start));
    }

    std::printf("\"%s\": %s results in %.1f ms (%u threads, best of %d runs)\n\n",
                toUtf8(query).c_str(), groupDigits(hits.size()).c_str(),
                bestSeconds * 1000.0,
                std::max(1u, std::thread::hardware_concurrency()), kSearchRuns);

    for (size_t i = 0; i < hits.size() && i < kMaxResults; ++i) {
        std::printf("  %s\n", toUtf8(index.fullPath(hits[i])).c_str());
    }
    if (hits.size() > kMaxResults) {
        std::printf("  ... and %s more\n",
                    groupDigits(hits.size() - kMaxResults).c_str());
    }

    return 0;
}
