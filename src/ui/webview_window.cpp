#include "ui/webview_window.h"

#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <wrl.h>

#include <WebView2.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "content/gate.h"
#include "index/watcher.h"
#include "ntfs/volume.h"
#include "reclaim/paths.h"
#include "reclaim/classify.h"
#include "reclaim/duplicates.h"
#include "reclaim/recycle.h"
#include "reclaim/scan.h"
#include "search/matcher.h"
#include "search/query_model.h"
#include "search/query_plan.h"
#include "search/searcher.h"
#include "store/query.h"
#include "store/snippet.h"
#include "ui/app_html.h"
#include "vector/embedder.h"
#include "vector/vector_store.h"

namespace filo {
namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

constexpr wchar_t kWindowClass[] = L"FiloMainWindow";
constexpr size_t kMaxRows = 200;

// Posted by the search thread when a result is ready to hand over. The message
// loop is the only safe place to touch WebView2 from.
constexpr UINT kSearchDone = WM_APP + 1;

// Anything a background thread wants to say to the page. WebView2 is
// apartment-threaded, so the message has to be handed to the loop rather than
// posted directly.
constexpr UINT kUiMessage = WM_APP + 2;

// Minimum cosine similarity for a semantic hit to count as a hit at all.
//
// Nearest-neighbour search has no notion of "nothing matched": it always
// returns its top K, so this is what separates an answer from the closest
// available noise. Set from FILO_MIN_SIMILARITY so it can be tuned against real
// queries rather than guessed once.
float minSimilarity() {
    wchar_t override[32] = {};
    if (GetEnvironmentVariableW(L"FILO_MIN_SIMILARITY", override, 32) > 0) {
        const float value = static_cast<float>(_wtof(override));
        if (value > 0.0f && value < 1.0f) return value;
    }
    return 0.65f;
}
const float kMinSimilarity = minSimilarity();

// How many documents the meaning list may contribute.
//
// RRF weighs by rank, so a long tail of weak semantic matches cannot outrank a
// strong lexical hit — but it can still fill the page below it. Twenty is about
// what fits on screen before the user would scroll.
constexpr size_t kMaxMeaningVotes = 20;

// Weight of a meaning vote relative to a word vote.
//
// Plain RRF treats every list as equally trustworthy. Here they are not: if the
// words are literally in the document, that is a fact; if a vector is nearby,
// that is a model's opinion — and this model scores 0.760 MRR, so its opinion
// is worth having but not worth as much. At 1.0 a semantic-only hit tied with
// the best lexical hit and won the tie by hash order.
constexpr double kMeaningWeight = 0.5;

// Reciprocal Rank Fusion constant. 60 is the value the original paper settled
// on and it has held up: large enough that the top few ranks are close
// together, small enough that rank 200 contributes almost nothing.
constexpr double kRrfK = 60.0;

// What one satisfied query filter is worth, in the same currency as an RRF
// vote.
//
// It was a whole first-place vote, and the arithmetic says that was far too
// much: a result at rank 200 scores 1/260 = 0.0038, and a full boost of 1/60 =
// 0.0167 took it to 0.0205 — past the 0.0167 of the BEST lexical hit. Any file
// that happened to be a PDF jumped the queue over the document the user was
// actually looking for.
//
// A quarter of a vote lifts a match by roughly twenty positions, which is what
// "prefer these" should mean. Reordering the page, not reversing it.
constexpr double kFilterBoost = 0.25 / kRrfK;

// --- minimal JSON -----------------------------------------------------------
//
// We produce the messages crossing the bridge on both sides and they have a
// fixed shape: this does not need a general parser, it needs two correct
// functions. Less code means less surface for bugs.

std::wstring jsonEscape(std::wstring_view text) {
    std::wstring out;
    out.reserve(text.size() + 8);
    for (const wchar_t c : text) {
        switch (c) {
            case L'"':  out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n"; break;
            case L'\r': out += L"\\r"; break;
            case L'\t': out += L"\\t"; break;
            default:
                if (c < 0x20) {
                    wchar_t buffer[8];
                    swprintf_s(buffer, L"\\u%04x", static_cast<unsigned>(c));
                    out += buffer;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Extracts a key's string value. Returns an empty string if absent.
std::wstring jsonField(const std::wstring& json, const wchar_t* key) {
    std::wstring pattern = L"\"";
    pattern += key;
    pattern += L"\":";

    size_t at = json.find(pattern);
    if (at == std::wstring::npos) return {};
    at += pattern.size();
    while (at < json.size() && (json[at] == L' ')) ++at;
    if (at >= json.size() || json[at] != L'"') return {};
    ++at;

    std::wstring value;
    while (at < json.size() && json[at] != L'"') {
        if (json[at] == L'\\' && at + 1 < json.size()) {
            ++at;
            switch (json[at]) {
                case L'n': value += L'\n'; break;
                case L'r': value += L'\r'; break;
                case L't': value += L'\t'; break;
                case L'u': {
                    if (at + 4 < json.size()) {
                        value += static_cast<wchar_t>(
                            wcstoul(json.substr(at + 1, 4).c_str(), nullptr, 16));
                        at += 4;
                    }
                    break;
                }
                default: value += json[at];
            }
        } else {
            value += json[at];
        }
        ++at;
    }
    return value;
}

// Extracts a key's NUMBER value, which jsonField cannot: it requires a quote
// after the colon and returns nothing for "id":7. Read through jsonField, every
// question about a file asked about file 0 — the answer even came back labelled
// id 0, so it only ever landed on the first row and every other row sat on
// "thinking…" forever. Returns `missing` when the key is absent or not a number,
// so a caller can tell "no id" from "id 0".
int64_t jsonNumber(const std::wstring& json, const wchar_t* key, int64_t missing) {
    std::wstring pattern = L"\"";
    pattern += key;
    pattern += L"\":";

    size_t at = json.find(pattern);
    if (at == std::wstring::npos) return missing;
    at += pattern.size();
    while (at < json.size() && json[at] == L' ') ++at;

    const size_t start = at;
    if (at < json.size() && (json[at] == L'-' || json[at] == L'+')) ++at;
    const size_t firstDigit = at;
    while (at < json.size() && json[at] >= L'0' && json[at] <= L'9') ++at;
    if (at == firstDigit) return missing;

    return _wtoi64(json.substr(start, at - start).c_str());
}

// The source is compiled /utf-8, so kAppHtml is UTF-8: the accented characters
// and UI symbols have to be genuinely converted, not widened byte by byte.
std::wstring utf8ToWide(const char* utf8) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (size <= 1) return {};
    std::wstring out(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), size);
    return out;
}

std::string toUtf8Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0,
                                         nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring userDataFolder() {
    wchar_t base[MAX_PATH] = {};
    const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    std::wstring directory = (len > 0 && len < MAX_PATH) ? base : L".";
    directory += L"\\filo";
    CreateDirectoryW(directory.c_str(), nullptr);
    directory += L"\\webview";
    CreateDirectoryW(directory.c_str(), nullptr);
    return directory;
}

}  // namespace

// ------------------------------------------------------------------ Impl

struct WebViewWindow::Impl {
    HWND window = nullptr;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    FileIndex* index = nullptr;
    ContentDb* content = nullptr;   // optional: without it, only names are searched
    std::wstring statusLine;
    std::wstring initialQuery;
    std::wstring probePath;  // smoke test: see runSearch
    bool navigated = false;  // the page is loaded once and never navigates again
    std::wstring startView;  // which screen a capture should open on
    bool ready = false;

    // Semantic search. Both are optional and arrive together: without a model
    // and a matching vector file the app behaves exactly as before, with two
    // lists instead of three, and says so in the status line rather than
    // pretending.
    Embedder embedder;
    VectorStore vectors;
    bool semantic = false;

    // Layer 5. Touched only by the search thread, so it needs no lock.
    QueryModel questionModel;
    std::wstring questionModelPath;

    // Explaining a file gets its own thread rather than queueing behind a scan
    // that takes a minute. The model has a lock of its own, so the two never
    // run at once — a search simply skips the model while an explanation is
    // being written.
    std::thread describeWorker;
    std::atomic<bool> describeBusy{false};
    int describedSeen = 0;   // a capture waits for the second answer, see drainOutbox

    // Search runs here, never on the UI thread. See startSearchThread().
    std::thread worker;
    std::mutex searchMutex;
    std::condition_variable searchSignal;
    std::wstring pendingQuery;
    bool hasPending = false;
    bool stopping = false;
    uint64_t requestGeneration = 0;

    std::wstring readyJson;
    std::wstring readyProbe;
    bool hasReady = false;

    // Reclaiming space. Its own thread: the pass takes about a minute and must
    // hold up neither the window nor a search.
    std::thread reclaimWorker;
    std::mutex outboxLock;
    std::vector<std::wstring> outbox;
    wchar_t reclaimDrive = L'C';
    std::atomic<bool> reclaimBusy{false};

    // What the last scan found, addressed by the ids sent to the page.
    //
    // A shared_ptr to a CONST vector, replaced whole and never edited in place.
    // It used to be a plain member that each scan cleared and refilled, which
    // was wrong twice over. The describe thread held a REFERENCE into it while
    // a new scan could be freeing it. And the page's ids are positions in it,
    // so a click held over from the previous scan named a different file — the
    // page would ask to delete row 12 and get whatever row 12 had become.
    //
    // Taking a copy of the pointer is enough to keep the whole snapshot alive
    // for as long as a worker is looking at it, and the generation lets the
    // host refuse an id that belongs to a scan nobody is looking at any more.
    struct Reclaimable {
        BinCandidate candidate;
        // Whether it may be DELETED. Every listed file gets an id so the model
        // can be asked what it is — that was the whole point of the feature and
        // it was backwards, only reaching files the rules had already explained.
        // Being nameable is not permission; this is.
        bool offerable = false;
    };
    struct ReclaimSnapshot {
        uint64_t generation = 0;
        std::vector<Reclaimable> entries;
    };

    std::mutex snapshotLock;
    std::shared_ptr<const ReclaimSnapshot> snapshot;
    uint64_t scanGeneration = 0;

    std::shared_ptr<const ReclaimSnapshot> currentSnapshot() {
        std::lock_guard<std::mutex> lock(snapshotLock);
        return snapshot;
    }

    // Guards the file index against being read while it is being changed.
    //
    // Searching reads the whole index and holds no lock at all today, which was
    // fine while nothing ever wrote to it after startup. Striking recycled
    // files out of it is a write, and a write racing a search is a torn read of
    // a path — so searching and scanning take this shared, and the deletion
    // aftermath takes it exclusively for the moment it needs.
    std::shared_mutex indexLock;

    // Applied after files really reached the bin, with the slots that moved.
    // The window knows what happened; main.cpp owns the index files and the
    // database, so it is the one that writes the change down.
    WebViewWindow::AfterRecycle afterRecycle;

    // --- keeping the index honest while the window is open ------------------
    WebViewWindow::Refresh refresh;
    unsigned refreshSeconds = 0;
    std::thread refreshWorker;
    std::mutex refreshMutex;
    std::condition_variable refreshSignal;
    bool refreshStopping = false;

    // Watching folders is the answer when the journal is out of reach, which is
    // what happens without administrator rights. It reports that something
    // changed; it does NOT edit the index.
    //
    // Applying a change by PATH would mean finding the entry that path belongs
    // to, and the index is built the other way round: it maps a file reference
    // number to a slot, with no path lookup at all. Searching for it would cost
    // a walk of 2.4 million records per event, and a build writes thousands of
    // files a second. So the watcher's job is to know the index has gone out of
    // date and say so, rather than to guess its way to a wrong one.
    std::vector<std::wstring> watchRoots;
    FolderWatcher watcher;
    std::atomic<bool> indexStale{false};
    std::atomic<bool> watching{false};

    // Hands a message to the UI thread. Safe from anywhere.
    void postFromWorker(const std::wstring& json) {
        {
            std::lock_guard<std::mutex> lock(outboxLock);
            outbox.push_back(json);
        }
        PostMessageW(window, kUiMessage, 0, 0);
    }

    void drainOutbox() {
        std::vector<std::wstring> batch;
        {
            std::lock_guard<std::mutex> lock(outboxLock);
            batch.swap(outbox);
        }
        for (const std::wstring& json : batch) {
            postToWeb(json);
            // A capture asked for the space screen waits for this, not for the
            // search results: the list of files is the thing to look at, and it
            // waits again for the model's answer to the first of them, since
            // that is the part a screenshot is the only way to check.
            //
            // The capture no longer fires on the second answer: the page has
            // one more thing to exercise after that — taking a recycled row off
            // the list — and it says when it is done by asking for the capture
            // itself.
            (void)describedSeen;
        }
    }

    void reportProgress(const std::wstring& phase, const std::wstring& detail) {
        postFromWorker(L"{\"type\":\"reclaimProgress\",\"phase\":\"" + jsonEscape(phase) +
                       L"\",\"detail\":\"" + jsonEscape(detail) + L"\"}");
    }

    // --- the scan, on the reclaim thread ------------------------------------
    //
    // Duplicates are one branch of this, not the whole of it. The screen leads
    // with what the disk is MADE of — rebuilt output, old installers, program
    // logs, downloads gone cold — because on a real working disk that is 52 GB
    // against the 5 GB identical copies account for.
    void scanForSpace() {
        reportProgress(L"Measuring the disk",
                       L"Reading the size of every file. About ten seconds.");

        ScanStats scanStats;
        const std::vector<FileFact> facts = collectFileFacts(*index, &scanStats);

        DuplicateOptions options;
        options.onProgress = [this](const wchar_t* phase, uint64_t done, uint64_t total) {
            wchar_t detail[96];
            swprintf_s(detail, L"%llu of %llu", static_cast<unsigned long long>(done),
                       static_cast<unsigned long long>(total));
            reportProgress(phase, detail);
        };

        DuplicateStats duplicateStats;
        const std::vector<DuplicateGroup> groups =
            findExactDuplicates(*index, facts, options, &duplicateStats);

        reportProgress(L"Working out what everything is", L"Almost there.");
        ClassifyStats classifyStats;
        const std::vector<Classified> classified =
            classifyBiggest(*index, facts, groups, 4000, nowFileTime(), &classifyStats);

        std::unordered_map<uint32_t, const FileFact*> bySlot;
        for (const FileFact& fact : facts) bySlot[fact.slot] = &fact;

        auto fresh = std::make_shared<ReclaimSnapshot>();
        fresh->generation = ++scanGeneration;
        const uint32_t driveSerial = volumeSerial(reclaimDrive);

        // Gives a file an id the page can send back.
        //
        // EVERY listed file gets one, offerable or not, because an id is what
        // lets the page ask the model what a file is — and the files worth
        // asking about are exactly the ones the rules could not explain, which
        // are the ones without a checkbox. Permission to delete is the separate
        // `offerable` flag, checked again on the way back in.
        auto claim = [&](uint32_t slot, const std::wstring& path, bool offerable) -> int64_t {
            const auto found = bySlot.find(slot);
            if (found == bySlot.end()) return -1;

            Reclaimable entry;
            entry.candidate.path = path;
            entry.candidate.size = found->second->size;
            entry.candidate.mtime = found->second->mtime;
            entry.candidate.slot = slot;
            // Recorded now so the deletion can prove it is the same file object
            // and not merely something of the same size at the same path.
            entry.candidate.frn = index->frn(slot);
            entry.candidate.volumeSerial = driveSerial;
            entry.offerable = offerable;

            const auto id = static_cast<int64_t>(fresh->entries.size());
            fresh->entries.push_back(std::move(entry));
            return id;
        };

        std::wstring json = L"{\"type\":\"reclaimResults\",\"gen\":" +
                            std::to_wstring(fresh->generation) + L",\"offerable\":" +
                            std::to_wstring(classifyStats.offerableBytes) +
                            L",\"examined\":" + std::to_wstring(scanStats.measured) +
                            L",\"total\":" + std::to_wstring(scanStats.totalBytes) +
                            L",\"categories\":[";

        bool firstCategory = true;
        for (size_t k = 0; k < static_cast<size_t>(SpaceClass::Count); ++k) {
            const auto kind = static_cast<SpaceClass>(k);

            // Identical copies get the group treatment instead of a flat list:
            // seeing which one is kept is most of what makes the choice easy.
            if (kind == SpaceClass::Duplicate) {
                if (groups.empty()) continue;
                if (!firstCategory) json += L',';
                firstCategory = false;
                json += L"{\"name\":\"" + jsonEscape(spaceClassName(kind)) +
                        L"\",\"bytes\":" + std::to_wstring(duplicateStats.reclaimableBytes) +
                        L",\"files\":" + std::to_wstring(groups.size()) +
                        L",\"offerable\":true,\"groups\":[";
                size_t shown = 0;
                for (const DuplicateGroup& group : groups) {
                    if (shown >= 150) break;
                    if (shown) json += L',';
                    ++shown;
                    json += L"{\"reclaimable\":" + std::to_wstring(group.reclaimable) +
                            L",\"each\":" + std::to_wstring(group.bytesEach) + L",\"files\":[";
                    for (size_t f = 0; f < group.slots.size(); ++f) {
                        if (f) json += L',';
                        const std::wstring path = index->fullPath(group.slots[f]);
                        // The keeper gets an id too, so it can be asked about,
                        // but never permission to be deleted.
                        const int64_t id = claim(group.slots[f], path, /*offerable=*/f != 0);
                        json += L"{\"id\":" + std::to_wstring(id) + L",\"pick\":" +
                                (f == 0 ? L"false" : L"true") + L",\"path\":\"" +
                                jsonEscape(forDisplay(path)) + L"\"}";
                    }
                    json += L"]}";
                }
                json += L"]}";
                continue;
            }

            if (classifyStats.filesByClass[k] == 0) continue;
            if (!firstCategory) json += L',';
            firstCategory = false;

            const bool offerable = isOfferable(kind);
            json += L"{\"name\":\"" + jsonEscape(spaceClassName(kind)) + L"\",\"bytes\":" +
                    std::to_wstring(classifyStats.bytesByClass[k]) + L",\"files\":" +
                    std::to_wstring(classifyStats.filesByClass[k]) + L",\"offerable\":" +
                    (offerable ? L"true" : L"false") + L",\"items\":[";

            size_t shown = 0;
            bool firstItem = true;
            for (const Classified& item : classified) {
                if (item.kind != kind) continue;
                if (shown >= 150) break;   // beyond this nobody scrolls
                if (!firstItem) json += L',';
                firstItem = false;
                ++shown;

                const std::wstring path = index->fullPath(item.slot);
                const int64_t id = claim(item.slot, path, offerable);
                json += L"{\"id\":" + std::to_wstring(id) + L",\"pick\":" +
                        (offerable ? L"true" : L"false") + L",\"size\":" +
                        std::to_wstring(item.size) + L",\"path\":\"" +
                        jsonEscape(forDisplay(path)) + L"\",\"why\":\"" +
                        jsonEscape(item.reason) + L"\"}";
            }
            json += L"]}";
        }
        json += L"]}";

        // Published only once it is complete, and as a whole. A worker that
        // already holds the previous one keeps it alive until it is finished
        // with it.
        {
            std::lock_guard<std::mutex> lock(snapshotLock);
            snapshot = fresh;
        }
        postFromWorker(json);
    }

    // --- the deletion, on the reclaim thread --------------------------------
    void recyclePicked(const std::vector<int64_t>& picks, uint64_t generation) {
        const auto held = currentSnapshot();
        if (!held || held->generation != generation) {
            // The ids name positions in a list that has since been rebuilt, so
            // they no longer mean what the page thinks. Refusing is the only
            // safe answer: row 12 of the old scan is a different file now.
            postFromWorker(L"{\"type\":\"reclaimDone\",\"binned\":0,\"bytes\":0,"
                           L"\"missing\":0,\"changed\":0,\"tooBig\":0,\"failed\":0,"
                           L"\"error\":\"the disk was scanned again, so nothing was "
                           L"moved — check the list and choose once more\"}");
            return;
        }

        std::vector<BinCandidate> chosen;
        for (const int64_t id : picks) {
            if (id < 0 || static_cast<size_t>(id) >= held->entries.size()) continue;
            const Reclaimable& entry = held->entries[static_cast<size_t>(id)];
            // Checked here and not only in the page. Every listed file has an
            // id so it can be asked about; only some of them may be deleted,
            // and that decision belongs on this side of the bridge.
            if (!entry.offerable) continue;
            chosen.push_back(entry.candidate);
        }

        // slot -> id, so the page can be told exactly which of ITS rows went.
        // The result comes back in slots, because that is what the index speaks;
        // the page only knows the positions it was handed.
        std::unordered_map<uint32_t, int64_t> idOfSlot;
        for (size_t i = 0; i < held->entries.size(); ++i) {
            idOfSlot[held->entries[i].candidate.slot] = static_cast<int64_t>(i);
        }

        const BinResult result = moveToRecycleBin(reclaimDrive, chosen, /*dryRun=*/false);

        // The three reasons for skipping go over separately. They used to be
        // added together and reported as "changed since Filo compared them",
        // which is a fair description of one of them and a lie about the other
        // two — and "too big for the Recycle Bin" is the one the user most
        // needs to hear, because it means Filo refused on purpose rather than
        // failing.
        std::wstring json = L"{\"type\":\"reclaimDone\",\"binned\":" +
                            std::to_wstring(result.binned) + L",\"bytes\":" +
                            std::to_wstring(result.bytesFreed) + L",\"missing\":" +
                            std::to_wstring(result.missing) + L",\"changed\":" +
                            std::to_wstring(result.changed) + L",\"tooBig\":" +
                            std::to_wstring(result.wouldBePermanent) + L",\"failed\":" +
                            std::to_wstring(result.failed) + L",\"error\":\"" +
                            jsonEscape(result.error) + L"\",\"recycled\":[";

        // Which rows to take off the screen. Without this the page had nothing
        // to go on but "something was deleted", so it threw the whole list away
        // and the user had to spend another minute scanning the disk to see the
        // rest of it. Filo knows exactly which rows went; saying so is cheap.
        bool firstId = true;
        for (const uint32_t slot : result.recycledSlots) {
            const auto found = idOfSlot.find(slot);
            if (found == idOfSlot.end()) continue;
            if (!firstId) json += L',';
            firstId = false;
            json += std::to_wstring(found->second);
        }
        json += L"]}";

        // Filo moved these itself, so nothing has to go looking to find out.
        // Without this the index keeps returning them at their old path until
        // the next launch — and on a machine that cannot read the USN journal,
        // for ever. Exclusive, because searching reads the index unlocked and a
        // path being rewritten underneath it is a torn read.
        if (afterRecycle && !result.recycledSlots.empty()) {
            std::unique_lock<std::shared_mutex> guard(indexLock);
            afterRecycle(result.recycledSlots);
        }

        postFromWorker(json);
    }

    // --- what is this file? -------------------------------------------------
    //
    // The one place in Filo where the model writes prose that nothing checks,
    // about a file the user is deciding whether to delete. The answer is
    // labelled as the model's, and the reason Filo is sure of stays next to it
    // unchanged.
    void startDescribe(int64_t id, uint64_t generation) {
        bool expected = false;
        if (!describeBusy.compare_exchange_strong(expected, true)) {
            // One question at a time. Saying so beats leaving the row spinning
            // on "thinking…" for an answer that will never arrive.
            postToWeb(L"{\"type\":\"described\",\"id\":" + std::to_wstring(id) +
                      L",\"text\":\"(still answering the last question)\"}");
            return;
        }
        if (describeWorker.joinable()) describeWorker.join();

        // A copy of the POINTER, taken once. The thread used to hold a
        // reference into a vector a new scan could clear while it read.
        //
        // The generation matters here too, though nothing is deleted: if a scan
        // finished between the page drawing the row and the click reaching us,
        // the same id names a different file, and describing the wrong one is
        // worse than declining.
        const auto held = currentSnapshot();
        const bool current = held && held->generation == generation;

        describeWorker = std::thread([this, id, held, current] {
            std::wstring text;
            if (!current) text = L"(the list has changed — scan again)";
            if (current && held && id >= 0 &&
                static_cast<size_t>(id) < held->entries.size() &&
                !questionModelPath.empty()) {
                const BinCandidate& candidate = held->entries[static_cast<size_t>(id)].candidate;
                std::wstring loadError;
                if (questionModel.ensureLoaded(questionModelPath, &loadError)) {
                    questionModel.describe(candidate.path, candidate.size, &text);
                } else {
                    text = L"(no language model installed — see the readme)";
                }
            }
            if (text.empty()) text = L"(the model had nothing useful to say)";

            // Freed BEFORE the answer is sent, not after. The other way round,
            // a second question asked the instant the first answer appears —
            // which is exactly when someone asks it — loses the race and gets
            // told the model is busy when it is already free.
            describeBusy = false;
            postFromWorker(L"{\"type\":\"described\",\"id\":" + std::to_wstring(id) +
                           L",\"text\":\"" + jsonEscape(text) + L"\"}");
        });
    }

    // --- following the disk while the window is open ------------------------
    //
    // The journal was read once, at startup, and never again: a file another
    // program made during the session simply did not exist as far as Filo was
    // concerned. This is the thread that keeps asking.
    void startRefreshThread() {
        if (!refresh || refreshSeconds == 0) return;

        refreshWorker = std::thread([this] {
            for (;;) {
                {
                    std::unique_lock<std::mutex> lock(refreshMutex);
                    refreshSignal.wait_for(lock, std::chrono::seconds(refreshSeconds),
                                           [this] { return refreshStopping; });
                    if (refreshStopping) return;
                }

                int64_t applied = 0;
                {
                    // Exclusive: this rewrites index entries, and searching
                    // reads them. Held only for the catch-up itself, which is
                    // milliseconds when nothing has happened — the common case,
                    // and the reason a timer is affordable at all.
                    std::unique_lock<std::shared_mutex> guard(indexLock);
                    applied = refresh();
                }

                if (applied < 0) {
                    // The journal is out of reach: almost always no elevation.
                    // Fall back to watching, once, and stop asking.
                    beginWatching();
                    return;
                }
                if (applied > 0) {
                    indexStale = false;
                    // FromWorker, not sendStatus: WebView2 is apartment-threaded
                    // and may only be touched from the thread that created it.
                    // Called directly from here it hangs the whole window, which
                    // is exactly what it did.
                    sendStatusFromWorker();
                }
            }
        });
    }

    // Watching does not repair the index, and must not pretend to. It notices,
    // and noticing is the difference between an index that is quietly wrong and
    // one that says so.
    void beginWatching() {
        bool expected = false;
        if (!watching.compare_exchange_strong(expected, true)) return;
        if (watchRoots.empty()) return;

        const size_t roots = watcher.start(watchRoots, [this](
                                              const std::vector<FolderWatcher::Event>&) {
            if (!indexStale.exchange(true)) sendStatusFromWorker();
        });
        diag(L"journal out of reach: watching " + std::to_wstring(roots) + L" folders");
        if (roots == 0) watching = false;
    }

    void startReclaim(bool scan, std::vector<int64_t> picks, uint64_t generation) {
        // One at a time. A second scan while the first is running would fight
        // it for the disk and finish with a stale candidate list.
        bool expected = false;
        if (!reclaimBusy.compare_exchange_strong(expected, true)) return;
        if (reclaimWorker.joinable()) reclaimWorker.join();

        reclaimWorker = std::thread([this, scan, picks, generation] {
            if (scan) {
                // Shared, not exclusive: measuring the disk reads the index for
                // about a minute and must not shut searching out for that long.
                std::shared_lock<std::shared_mutex> guard(indexLock);
                scanForSpace();
            } else {
                recyclePicked(picks, generation);
            }
            reclaimBusy = false;
        });
    }

    // Diagnostic log. Initializing WebView2 is a chain of asynchronous
    // callbacks: if one fails there is no error to check on return, the window
    // simply stays white. Without these lines every failure is invisible.
    void diag(const std::wstring& line) {
        wchar_t base[MAX_PATH] = {};
        const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        std::wstring path = (len > 0 && len < MAX_PATH) ? base : L".";
        path += L"\\filo\\ui-diag.log";

        HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;

        const std::wstring text = line + L"\r\n";
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                              static_cast<int>(text.size()),
                                              nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                            utf8.data(), bytes, nullptr, nullptr);
        DWORD written = 0;
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        CloseHandle(file);
    }

    void diagHr(const wchar_t* what, HRESULT hr) {
        wchar_t buffer[128];
        swprintf_s(buffer, L"%s -> 0x%08lX", what, static_cast<unsigned long>(hr));
        diag(buffer);
    }

    void postToWeb(const std::wstring& json) {
        if (webview) webview->PostWebMessageAsJson(json.c_str());
    }

    // --- showing a path without showing whose it is -------------------------
    //
    // For screenshots, and for anyone pasting one into a bug report. With
    // FILO_ANON_HOME set, the user's profile folder is displayed as that value
    // instead — C:\Users\alice\Documents\x.pdf shown as C:\Users\you\...
    //
    // It changes only what the PAGE is shown, never what Filo acts on. That is
    // safe here for a structural reason rather than a hopeful one: the page
    // names files to delete by id into a snapshot the host built, never by
    // path, so a displayed path cannot reach the deletion code at all. The two
    // messages that DO carry a path back — open and reveal — are put through
    // the inverse below, so a file opened from an anonymised list still opens.
    //
    // Off unless the variable is set, and it names the replacement rather than
    // being a plain on/off, so nobody switches it on by accident.
    std::wstring anonHome;
    std::wstring realHome;       // as Windows spells it, for building real paths
    std::wstring realHomeLower;  // for the comparison, which is case-insensitive

    std::wstring forDisplay(const std::wstring& path) const {
        if (anonHome.empty() || realHomeLower.empty()) return path;
        // Both sides lowercase. Comparing a folded path against an unfolded
        // folder matched nothing at all, which is a failure that looks exactly
        // like the feature being switched off.
        if (!insideFolder(toLowerCopy(path), realHomeLower)) return path;
        return anonHome + path.substr(realHomeLower.size());
    }

    std::wstring fromDisplay(const std::wstring& path) const {
        if (anonHome.empty() || realHome.empty()) return path;
        if (toLowerCopy(path).compare(0, anonHome.size(), toLowerCopy(anonHome)) != 0) {
            return path;
        }
        return realHome + path.substr(anonHome.size());
    }

    std::wstring statusJson() {
        std::wstring line = statusLine;
        // Said in the status line rather than as an alert. It is a standing
        // condition, not an event, and the honest phrasing matters: results may
        // be missing or may name files that have moved. Better than a search
        // that is quietly wrong and looks right.
        if (indexStale) {
            line += L" · files have changed since this index was built — "
                    L"run Filo as administrator to bring it up to date";
        }
        return L"{\"type\":\"status\",\"text\":\"" + jsonEscape(line) + L"\"}";
    }

    // UI thread only. postToWeb goes straight at WebView2, which is
    // apartment-threaded: from any other thread this hangs the window.
    void sendStatus() { postToWeb(statusJson()); }

    // The watcher and the refresh timer run on their own threads, so they hand
    // the message to the message loop instead.
    void sendStatusFromWorker() { postFromWorker(statusJson()); }

    // One result, wherever it came from.
    struct Merged {
        uint32_t slot = 0;
        double   fused = 0.0;
        bool     byName = false;
        bool     byContent = false;
        bool     byMeaning = false;   // found by the vectors, not by the words
        int      satisfied = 0;       // how many query filters this file meets
        int      passages = 0;
        int64_t  chunkId = 0;
    };

    // Runs entirely on the search thread. Produces text; touches no COM.
    //
    // Everything it reads — the name index, the content database, the model,
    // the vector store — is owned by this thread for the duration. That is not
    // tidiness: SQLite is built with SQLITE_THREADSAFE=2, which forbids two
    // threads using one connection, and a llama context is single-threaded in
    // the same way. Keeping the UI thread out of all of it is what makes the
    // rule easy to hold.
    void runSearch(const std::wstring& rawQuery, std::wstring* outJson,
                   std::wstring* outProbe) {
        // Held for the whole search. Every line below reads the index, and
        // striking recycled files out of it is now a real write that can arrive
        // at any moment. Shared, so searches and the disk scan still overlap.
        std::shared_lock<std::shared_mutex> guard(indexLock);

        std::wstring json = L"{\"type\":\"results\",\"q\":\"";
        json += jsonEscape(rawQuery);
        json += L"\",";

        if (rawQuery.empty()) {
            json += L"\"total\":0,\"ms\":0,\"nameHits\":0,\"contentHits\":0,\"items\":[]}";
            *outJson = std::move(json);
            return;
        }

        const auto start = std::chrono::steady_clock::now();

        // --- what the query asks for beyond its words ------------------------
        //
        // "il pdf del contratto di marzo" carries a subject, a format and a
        // time. Without this all three go to the text search, so "pdf" and
        // "marzo" get hunted for INSIDE the documents — which is the opposite
        // of what was meant, and why that query used to return nothing.
        const int64_t now = nowFileTime();
        QueryPlan plan = planQuery(rawQuery, now);

        // The two word lists, for whichever plan we end up with. Gathered here
        // rather than inline because they may have to be gathered twice.
        std::vector<uint32_t> nameHits;
        std::vector<ContentHit> contentHits;
        double contentMs = 0;
        auto gatherWords = [&](const QueryPlan& attempt) {
            nameHits = searchNames(*index, toLowerCopy(attempt.text));
            contentHits.clear();
            contentMs = 0;
            if (content) {
                contentHits =
                    searchContent(*content, toUtf8Narrow(attempt.text), 60, &contentMs);
            }
        };

        gatherWords(plan);

        // Layer 5, and it only speaks when spoken to.
        //
        // It used to speak FIRST, before names, text or meaning had been tried,
        // which put about a second on every query that merely looked like a
        // question — including all the ones the cheap layers were about to
        // answer perfectly well. Now the cheap layers run, and the model is
        // asked only when they came back with nothing at all. Names cost 13 ms
        // against the in-memory index and the text search a few more, so the
        // probe is close to free next to what it can save.
        //
        // Loaded on this thread the first time one is needed: three seconds for
        // a 3B model is unacceptable at startup and invisible on a search that
        // is already asynchronous. Someone who never asks a question never pays
        // it at all.
        const bool nothingYet = nameHits.empty() && contentHits.empty();
        if (nothingYet && !questionModelPath.empty() && QueryModel::worthAsking(plan)) {
            std::wstring loadError;
            if (questionModel.ensureLoaded(questionModelPath, &loadError)) {
                if (questionModel.refine(rawQuery, now, &plan)) {
                    diag(L"question: " + rawQuery + L" -> " + plan.text);
                    gatherWords(plan);
                }
            } else {
                diag(L"question model unavailable: " + loadError);
            }
        }

        const std::wstring searchText = plan.text;

        // --- list 1: the names ------------------------------------------------
        const std::wstring needle = toLowerCopy(searchText);
        const size_t nameTotal = nameHits.size();

        // searchNames returns MFT order: it has to be re-sorted by relevance
        // before it can be fused with the other list.
        std::vector<std::pair<double, uint32_t>> ranked;
        ranked.reserve(nameHits.size());
        for (const uint32_t slot : nameHits) {
            ranked.emplace_back(nameRelevance(index->name(slot), needle), slot);
        }
        std::partial_sort(
            ranked.begin(),
            ranked.begin() + static_cast<ptrdiff_t>(std::min<size_t>(ranked.size(), 200)),
            ranked.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });
        if (ranked.size() > 200) ranked.resize(200);

        // --- list 2: the content is already in contentHits, from gatherWords --

        // --- fusion -----------------------------------------------------------
        //
        // Reciprocal Rank Fusion: every list votes with 1/(k + position). No
        // comparable scores are needed, and indeed there are none: name
        // relevance, bm25 and cosine similarity live on different scales and
        // normalizing them would be an arbitrary calibration. Only the ORDER
        // counts — which is exactly what let the vector results join later
        // without touching any of this.
        std::unordered_map<uint32_t, Merged> merged;

        for (size_t rank = 0; rank < ranked.size(); ++rank) {
            Merged& entry = merged[ranked[rank].second];
            entry.slot = ranked[rank].second;
            entry.byName = true;
            entry.fused += 1.0 / (kRrfK + static_cast<double>(rank));
        }
        // --- list 3: meaning --------------------------------------------------
        //
        // This is the one that answers a question the other two cannot: the
        // words in the query need not appear in the document at all. Searching
        // for "penale" finds a contract that only says "clausola risarcitoria",
        // because the two sit near each other in the vector space.
        //
        // The floor matters more than the ceiling. A vector search always
        // returns its top K, however unrelated they are, so without a minimum
        // similarity every query would drag in forty arbitrary documents and
        // RRF would dutifully rank them.
        size_t meaningHits = 0;
        if (semantic && content) {
            std::vector<float> queryVector(embedder.dimensions());
            // The FULL query goes to the embedder, filters and all: a vector is
            // built from meaning, and "il pdf del contratto di marzo" means
            // more as a sentence than "contratto" does alone.
            if (embedder.embedQuery(toUtf8Narrow(rawQuery), queryVector.data())) {
                const auto neighbours = vectors.search(queryVector.data(), 60);

                std::unordered_map<int64_t, bool> seenDocuments;
                size_t rank = 0;
                for (const VectorStore::Neighbor& neighbour : neighbours) {
                    if (neighbour.score < kMinSimilarity) break;   // sorted: the rest is worse
                    if (rank >= kMaxMeaningVotes) break;

                    int64_t docId = 0;
                    uint64_t frn = 0;
                    double rankPenalty = 0.0;
                    if (!documentOfChunk(*content, neighbour.chunkId, &docId, &frn,
                                         &rankPenalty)) {
                        continue;
                    }
                    // Machine-generated content never votes on meaning.
                    //
                    // The lexical search demotes it; the semantic one has to
                    // refuse it outright, because similarity gives it no way to
                    // be wrong. Searching for "risarcimento danni" surfaced
                    // build manifests out of android/app/build/intermediates at
                    // 0.6 similarity — a number this model also assigns to
                    // genuinely related prose, so no threshold separates them.
                    if (rankPenalty > 0.0) continue;

                    // One vote per document: a long contract has many chunks
                    // and would otherwise vote for itself a dozen times.
                    if (!seenDocuments.emplace(docId, true).second) continue;

                    const uint32_t slot = index->indexOfFrn(frn);
                    if (slot == kNoParent || index->isDeleted(slot)) continue;

                    Merged& entry = merged[slot];
                    entry.slot = slot;
                    entry.byMeaning = true;
                    // Only claim the chunk for the snippet if the word search
                    // did not already point at a better one.
                    if (entry.chunkId == 0) entry.chunkId = neighbour.chunkId;
                    entry.fused += kMeaningWeight / (kRrfK + static_cast<double>(rank));
                    ++rank;
                    ++meaningHits;
                }
            }
        }

        for (size_t rank = 0; rank < contentHits.size(); ++rank) {
            const ContentHit& hit = contentHits[rank];
            // indexOfFrn also returns TOMBSTONES: a file deleted after indexing
            // still has its record, marked. Without this
            // check it would appear among the results with the name and path of
            // something that no longer exists — while the name list already
            // filters it, so the two halves would behave differently.
            const uint32_t slot = index->indexOfFrn(hit.frn);
            if (slot == kNoParent || index->isDeleted(slot)) continue;
            Merged& entry = merged[slot];
            entry.slot = slot;
            entry.byContent = true;
            entry.passages = hit.passages;
            entry.chunkId = hit.bestChunkId;
            entry.fused += 1.0 / (kRrfK + static_cast<double>(rank));
        }

        std::vector<Merged> results;
        results.reserve(merged.size());
        for (auto& entry : merged) results.push_back(entry.second);

        // --- the filters, applied as a lift and never as a gate ---------------
        //
        // A satisfied filter adds score; an unsatisfied one subtracts nothing.
        // Excluding would be stronger and is exactly the wrong kind of strong:
        // if the format was guessed wrong the right answer disappears, and the
        // user sees an empty list with nothing on screen to say why.
        //
        // One satisfied filter is worth a first-place vote, which reorders the
        // page without letting an irrelevant file that happens to be a PDF beat
        // a genuinely relevant one.
        if (plan.hasFilters()) {
            const bool needsTime = plan.after != 0 || plan.before != 0;
            for (Merged& entry : results) {
                const std::wstring lowerPath = toLowerCopy(index->fullPath(entry.slot));

                // Modification time is only read when a time filter is active,
                // and only for results that survived the fusion: a stat per
                // candidate would otherwise cost more than the search.
                int64_t mtime = 0;
                if (needsTime) {
                    FileFacts facts;
                    if (readFileFacts(index->fullPath(entry.slot).c_str(), &facts)) {
                        mtime = facts.mtime;
                    }
                }
                entry.satisfied = plan.satisfiedBy(lowerPath, mtime);
                entry.fused += kFilterBoost * entry.satisfied;
            }
        }

        std::sort(results.begin(), results.end(),
                  [](const Merged& a, const Merged& b) { return a.fused > b.fused; });
        if (results.size() > kMaxRows) results.resize(kMaxRows);

        const double milliseconds =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();

        // The words actually searched for, which is what should be highlighted.
        // Highlighting the raw query paints "il" and "del" all over a snippet
        // and makes a correct result look like a sloppy one.
        json += L"\"terms\":\"" + jsonEscape(searchText) + L"\",";
        json += L"\"total\":" + std::to_wstring(merged.size());
        json += L",\"nameHits\":" + std::to_wstring(nameTotal);
        json += L",\"contentHits\":" + std::to_wstring(contentHits.size());
        json += L",\"meaningHits\":" + std::to_wstring(meaningHits);
        json += L",\"ms\":" + std::to_wstring(milliseconds);
        json += L",\"items\":[";

        // Previews cost one file read each: only for the first rows, which are
        // the only ones visible without scrolling.
        const std::vector<std::string> terms = splitTerms(toUtf8Narrow(searchText));
        int snippetsBuilt = 0;

        for (size_t i = 0; i < results.size(); ++i) {
            const Merged& entry = results[i];
            const std::wstring path = index->fullPath(entry.slot);
            const size_t cut = path.find_last_of(L'\\');
            const std::wstring directory =
                cut == std::wstring::npos ? path : path.substr(0, cut);

            std::wstring snippet;
            // Six previews instead of twelve: each costs a file read, and six
            // is already more than fits on screen without scrolling.
            // Semantic hits need the preview MORE than lexical ones, not less:
            // the query words are by definition absent from the text, so
            // without a snippet the row is a filename with no stated reason for
            // being there. Leaving them out made a semantic result look like a
            // bug.
            if ((entry.byContent || entry.byMeaning) && content && snippetsBuilt < 6) {
                uint32_t offset = 0, length = 0;
                int64_t docId = -1;
                if (chunkRange(*content, entry.chunkId, &offset, &length, &docId)) {
                    // PDFs and Office documents had their text extracted once,
                    // at indexing time, and kept. Reading it back is a row
                    // lookup; re-parsing the file would be a worker round trip
                    // per result, which is why these used to show no preview.
                    std::string stored;
                    readDocumentText(*content, docId, &stored);

                    const std::string text =
                        buildSnippet(path, stored, offset, length, terms, 240);
                    if (!text.empty()) {
                        snippet = utf8ToWide(text.c_str());
                        ++snippetsBuilt;
                    }
                }
            }

            if (i) json += L',';
            json += L"{\"name\":\"";
            json += jsonEscape(index->name(entry.slot));
            json += L"\",\"dir\":\"";
            json += jsonEscape(forDisplay(directory));
            json += L"\",\"path\":\"";
            json += jsonEscape(forDisplay(path));
            json += L"\",\"inName\":";
            json += entry.byName ? L"true" : L"false";
            json += L",\"inBody\":";
            json += entry.byContent ? L"true" : L"false";
            json += L",\"byMeaning\":";
            json += entry.byMeaning ? L"true" : L"false";
            json += L",\"passages\":" + std::to_wstring(entry.passages);
            json += L",\"snippet\":\"";
            json += jsonEscape(snippet);
            json += L"\"}";
        }
        json += L"]}";
        *outJson = std::move(json);

        // Automatic smoke test. Reaching here means the page loaded, the
        // JavaScript started and the bridge works in BOTH directions: none of
        // those steps can be checked by looking at pixels, because WebView2
        // does not let itself be photographed.
        //
        // Only the text is built here. Writing the file and asking WebView2 for
        // a screenshot are the caller's job, on the UI thread.
        if (!probePath.empty()) {
            std::wstring report = L"query=" + rawQuery;
            report += L"\nnames=" + std::to_wstring(nameTotal);
            report += L"\ncontent=" + std::to_wstring(contentHits.size());
            report += L"\nfused=" + std::to_wstring(merged.size());
            report += L"\nms=" + std::to_wstring(milliseconds);
            report += L"\nsnippets=" + std::to_wstring(snippetsBuilt);
            if (!results.empty()) {
                report += L"\nfirst=" + index->fullPath(results[0].slot);
                report += L"\nfirst_from=";
                report += results[0].byName ? L"name" : L"";
                report += results[0].byContent ? L"+text" : L"";
                report += results[0].byMeaning ? L"+meaning" : L"";
            }
            report += L"\n";
            *outProbe = std::move(report);
        }
    }

    // --- the search thread ---------------------------------------------------
    //
    // Search used to run inside the WebView2 message callback, so every
    // keystroke that survived the debounce froze the window for as long as it
    // took: a name scan over 2.4 million records, an FTS query, an embedding of
    // the query, a vector scan, and up to six snippets. Thirty to seventy
    // milliseconds, on the thread that draws.
    //
    // A generation counter is what makes typing feel right. Each request bumps
    // it; a result whose generation is no longer current is dropped rather than
    // shown, so a slow search for "cont" can never overwrite the results for
    // "contratto" that the user has already moved on to.
    void startSearchThread() {
        worker = std::thread([this] {
            for (;;) {
                std::wstring query;
                uint64_t generation = 0;
                {
                    std::unique_lock<std::mutex> lock(searchMutex);
                    searchSignal.wait(lock, [this] { return hasPending || stopping; });
                    if (stopping) return;
                    query = pendingQuery;
                    generation = requestGeneration;
                    hasPending = false;
                }

                std::wstring json, probe;
                runSearch(query, &json, &probe);

                {
                    std::lock_guard<std::mutex> lock(searchMutex);
                    // Superseded while we were working: drop it silently.
                    if (generation != requestGeneration) continue;
                    readyJson = std::move(json);
                    readyProbe = std::move(probe);
                    hasReady = true;
                }
                PostMessageW(window, kSearchDone, 0, 0);
            }
        });
    }

    void stopRefreshThread() {
        // The watcher first: its sink posts to the window, and the window is
        // about to go away.
        watcher.stop();
        {
            std::lock_guard<std::mutex> lock(refreshMutex);
            refreshStopping = true;
        }
        refreshSignal.notify_all();
        if (refreshWorker.joinable()) refreshWorker.join();
    }

    void stopSearchThread() {
        {
            std::lock_guard<std::mutex> lock(searchMutex);
            stopping = true;
        }
        searchSignal.notify_all();
        if (worker.joinable()) worker.join();
    }

    void requestSearch(const std::wstring& query) {
        {
            std::lock_guard<std::mutex> lock(searchMutex);
            pendingQuery = query;
            hasPending = true;
            ++requestGeneration;
        }
        searchSignal.notify_one();
    }

    // On the UI thread: hand the finished result to the page.
    void deliverSearch() {
        std::wstring json, probe;
        {
            std::lock_guard<std::mutex> lock(searchMutex);
            if (!hasReady) return;
            hasReady = false;
            json = std::move(readyJson);
            probe = std::move(readyProbe);
        }

        postToWeb(json);
        // On a space capture the search result is not what should be
        // photographed: the reclaim results arrive a minute later and are the
        // screen worth looking at.
        if (probe.empty() || startView == L"space") return;
        capturePage(probe);
    }

    void capturePage(const std::wstring& probe) {
        HANDLE file = CreateFileW(probePath.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, probe.data(),
                                                  static_cast<int>(probe.size()), nullptr,
                                                  0, nullptr, nullptr);
            std::string utf8(static_cast<size_t>(bytes), '\0');
            WideCharToMultiByte(CP_UTF8, 0, probe.data(), static_cast<int>(probe.size()),
                                utf8.data(), bytes, nullptr, nullptr);
            DWORD written = 0;
            WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
            CloseHandle(file);
        }

        // CapturePreview photographs the page AS WEBVIEW2 DRAWS IT. PrintWindow
        // cannot reach it, because the content lives on a composition surface
        // separate from the Win32 window.
        const std::wstring imagePath = probePath + L".png";
        IStream* stream = nullptr;
        if (SUCCEEDED(SHCreateStreamOnFileEx(imagePath.c_str(),
                                             STGM_CREATE | STGM_READWRITE,
                                             FILE_ATTRIBUTE_NORMAL, TRUE, nullptr,
                                             &stream))) {
            HWND target = window;
            Impl* self = this;
            webview->CapturePreview(
                COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream,
                Callback<ICoreWebView2CapturePreviewCompletedHandler>(
                    [stream, target, self](HRESULT result) -> HRESULT {
                        self->diagHr(L"CapturePreview", result);
                        stream->Release();
                        PostMessageW(target, WM_CLOSE, 0, 0);
                        return S_OK;
                    })
                    .Get());
        } else {
            PostMessageW(window, WM_CLOSE, 0, 0);
        }
    }

    void onWebMessage(const std::wstring& json) {
        const std::wstring type = jsonField(json, L"type");

        if (type == L"search") {
            requestSearch(jsonField(json, L"q"));
        } else if (type == L"reclaimScan") {
            startReclaim(/*scan=*/true, {}, 0);
        } else if (type == L"describe") {
            startDescribe(jsonNumber(json, L"id", -1),
                          static_cast<uint64_t>(jsonNumber(json, L"gen", 0)));
        } else if (type == L"probeCapture") {
            // The page says it has finished exercising itself. It reports what
            // the removal did, so the check is a NUMBER in the probe file and
            // not just a picture somebody has to squint at.
            if (!probePath.empty() && webview) {
                webview->ExecuteScript(
                    L"window.filoRemoval || 'no removal test'",
                    Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
                        [this](HRESULT, LPCWSTR result) -> HRESULT {
                            std::wstring note = result ? result : L"";
                            capturePage(L"space captured, removal: " + note + L"\n");
                            return S_OK;
                        }).Get());
            }
        } else if (type == L"reclaimRecycle") {
            // The page sends ids into the candidate list, never paths: a path
            // arriving from the page would be a path the C++ side never
            // measured, and measuring is what makes the deletion safe.
            std::vector<int64_t> picks;
            const size_t at = json.find(L"\"picks\"");
            if (at != std::wstring::npos) {
                for (size_t i = json.find(L'[', at); i != std::wstring::npos && i < json.size();
                     ++i) {
                    if (json[i] == L']') break;
                    if (json[i] < L'0' || json[i] > L'9') continue;
                    int64_t value = 0;
                    while (i < json.size() && json[i] >= L'0' && json[i] <= L'9') {
                        value = value * 10 + (json[i] - L'0');
                        ++i;
                    }
                    picks.push_back(value);
                    --i;
                }
            }
            startReclaim(/*scan=*/false, picks, jsonNumber(json, L"gen", 0));
        } else if (type == L"ready") {
            ready = true;
            sendStatus();
            if (!startView.empty() && webview) {
                std::wstring script = L"window.filoShowView('" + jsonEscape(startView) + L"')";
                // A capture of the space screen has to press the button too:
                // there is nothing to photograph until the scan has run.
                if (startView == L"space" && !probePath.empty()) {
                    script += L";window.filoExpandFirst=1;window.filoAskFirst=1"
                              L";document.getElementById('scan').click()";
                }
                webview->ExecuteScript(script.c_str(), nullptr);
            }
            if (!initialQuery.empty()) {
                std::wstring payload = L"{\"type\":\"prefill\",\"q\":\"";
                payload += jsonEscape(initialQuery);
                payload += L"\"}";
                postToWeb(payload);
            }
        } else if (type == L"open") {
            // Back through the inverse of forDisplay: with an anonymised home
            // the page is holding a path that does not exist on this disk.
            const std::wstring path = fromDisplay(jsonField(json, L"path"));
            // ShellExecute returns a value <= 32 on failure. Without checking
            // it, a failed open is indistinguishable from a successful one.
            const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
                window, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
            diag(L"open \"" + path + L"\" -> " + std::to_wstring(result));
            if (result <= 32) {
                // Fallback: opening the containing folder is still more useful
                // than doing nothing.
                std::wstring arguments = L"/select,\"" + path + L"\"";
                ShellExecuteW(window, L"open", L"explorer.exe", arguments.c_str(),
                              nullptr, SW_SHOWNORMAL);
            }
        } else if (type == L"reveal") {
            // /select, opens Explorer with the file already highlighted.
            const std::wstring path = fromDisplay(jsonField(json, L"path"));
            std::wstring arguments = L"/select,\"" + path + L"\"";
            const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
                window, L"open", L"explorer.exe", arguments.c_str(), nullptr,
                SW_SHOWNORMAL));
            diag(L"reveal \"" + path + L"\" -> " + std::to_wstring(result));
        } else if (type == L"hide") {
            PostMessageW(window, WM_CLOSE, 0, 0);
        }
    }
};

WebViewWindow::~WebViewWindow() {
    // The thread holds pointers into Impl and into the caller's database, so it
    // has to be gone before any of that is.
    if (impl_) {
        impl_->stopRefreshThread();
        impl_->stopSearchThread();
        if (impl_->reclaimWorker.joinable()) impl_->reclaimWorker.join();
        if (impl_->describeWorker.joinable()) impl_->describeWorker.join();
    }
    delete impl_;
}

int WebViewWindow::run() {
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    // Stopped here as well as in the destructor: the window is closing and the
    // caller's ContentDb goes out of scope right after run() returns, while the
    // search thread may still be halfway through a query against it.
    if (impl_) {
        impl_->stopRefreshThread();
        impl_->stopSearchThread();
        if (impl_->reclaimWorker.joinable()) impl_->reclaimWorker.join();
        if (impl_->describeWorker.joinable()) impl_->describeWorker.join();
    }
    return 0;
}

bool WebViewWindow::loadSemantic(const std::wstring& modelPath,
                                 const std::wstring& vectorPath,
                                 uint32_t contentGeneration, std::wstring* error) {
    if (!impl_) impl_ = new Impl();

    if (!impl_->embedder.load(modelPath, {}, error)) return false;

    const VectorStore::Identity identity{modelIdentity(modelPath),
                                         impl_->embedder.dimensions(),
                                         static_cast<uint32_t>(kChunkerVersion),
                                         contentGeneration};
    bool mismatch = false;
    if (!impl_->vectors.load(vectorPath, identity, &mismatch, error)) return false;
    if (impl_->vectors.size() == 0) {
        *error = L"the vector file is empty";
        return false;
    }

    impl_->semantic = true;
    return true;
}

bool WebViewWindow::semanticReady() const { return impl_ && impl_->semantic; }

void WebViewWindow::useQuestionModel(const std::wstring& modelPath) {
    if (!impl_) impl_ = new Impl();
    if (GetFileAttributesW(modelPath.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    impl_->questionModelPath = modelPath;
}

void WebViewWindow::onRecycled(AfterRecycle callback) {
    if (!impl_) impl_ = new Impl();
    impl_->afterRecycle = std::move(callback);
}

void WebViewWindow::refreshWith(Refresh callback, unsigned everySeconds) {
    if (!impl_) impl_ = new Impl();
    impl_->refresh = std::move(callback);
    impl_->refreshSeconds = everySeconds;
}

void WebViewWindow::watchFolders(std::vector<std::wstring> roots) {
    if (!impl_) impl_ = new Impl();
    impl_->watchRoots = std::move(roots);
}

bool WebViewWindow::create(FileIndex* index, ContentDb* content,
                           const std::wstring& statusLine,
                           const std::wstring& initialQuery, std::wstring* error) {
    if (!impl_) impl_ = new Impl();
    impl_->index = index;
    impl_->content = content;
    impl_->statusLine = statusLine;
    impl_->initialQuery = initialQuery;
    impl_->reclaimDrive = index ? index->drive() : L'C';

    wchar_t probe[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"FILO_UI_PROBE", probe, MAX_PATH) > 0) {
        impl_->probePath = probe;
    }
    // Which screen the capture should photograph. WebView2 cannot be clicked
    // from outside, so without this only the search view would ever be seen.
    wchar_t view[32] = {};
    if (GetEnvironmentVariableW(L"FILO_UI_VIEW", view, 32) > 0) impl_->startView = view;

    // Screenshots and bug reports: show somebody else's home as a neutral one.
    wchar_t anon[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"FILO_ANON_HOME", anon, MAX_PATH) > 0) {
        wchar_t home[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH) > 0) {
            impl_->anonHome = anon;
            impl_->realHome = home;
            impl_->realHomeLower = toLowerCopy(home);
        }
    }

    impl_->diag(L"=== interface start ===");

    // WebView2 is COM. Without an apartment initialized on this thread the
    // creation callbacks are never delivered and the window simply stays white,
    // with no call reporting an error.
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        impl_->diagHr(L"CoInitializeEx", comResult);
        *error = L"COM initialization failed";
        return false;
    }
    impl_->diagHr(L"CoInitializeEx", comResult);

    const HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = [](HWND window, UINT message, WPARAM wParam,
                                 LPARAM lParam) -> LRESULT {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        switch (message) {
            case WM_SIZE:
                if (self && self->controller) {
                    RECT bounds;
                    GetClientRect(window, &bounds);
                    self->controller->put_Bounds(bounds);
                }
                return 0;
            case kUiMessage:
                if (self) self->drainOutbox();
                return 0;
            case kSearchDone:
                // The search thread finished something. Handing it to the page
                // has to happen here: WebView2 is apartment-threaded and would
                // reject the call from anywhere else.
                if (self) self->deliverSearch();
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            default:
                return DefWindowProcW(window, message, wParam, lParam);
        }
    };
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // Resource 1: the icon compiled into the executable from res/filo.rc.
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&windowClass);

    impl_->window = CreateWindowExW(
        0, kWindowClass, L"Filo", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 620,
        nullptr, nullptr, instance, nullptr);

    if (!impl_->window) {
        *error = L"window creation failed";
        return false;
    }
    SetWindowLongPtrW(impl_->window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl_));

    // Started only now: the thread posts to `window`, so the handle has to
    // exist before it can run.
    impl_->startSearchThread();
    impl_->startRefreshThread();

    // WebView2 initializes in two asynchronous steps: first the environment,
    // then the controller. Only inside the second can the page be loaded.
    const std::wstring dataFolder = userDataFolder();
    Impl* impl = impl_;

    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, dataFolder.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [impl](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                impl->diagHr(L"environment created", result);
                if (FAILED(result) || !environment) return result;

                environment->CreateCoreWebView2Controller(
                    impl->window,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [impl](HRESULT result2,
                               ICoreWebView2Controller* controller) -> HRESULT {
                            impl->diagHr(L"controller created", result2);
                            if (FAILED(result2) || !controller) return result2;

                            impl->controller = controller;
                            controller->get_CoreWebView2(&impl->webview);

                            ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(impl->webview->get_Settings(&settings))) {
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                                settings->put_AreDevToolsEnabled(TRUE);
                            }

                            EventRegistrationToken token;
                            impl->webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [impl](ICoreWebView2*,
                                           ICoreWebView2WebMessageReceivedEventArgs* args)
                                        -> HRESULT {
                                        LPWSTR raw = nullptr;
                                        if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) {
                                            impl->onWebMessage(raw);
                                            CoTaskMemFree(raw);
                                        }
                                        return S_OK;
                                    })
                                    .Get(),
                                &token);

                            // The page is handed over once, as a string, and
                            // must never go anywhere else.
                            //
                            // The Content-Security-Policy in the page already
                            // refuses every kind of subresource, but a policy
                            // does not stop the document ITSELF from being
                            // replaced: one `location = ...` and the window is
                            // showing a remote page with the message bridge
                            // still attached to it. Nothing in Filo does that
                            // today, and this is what keeps it true.
                            //
                            // The first navigation is the NavigateToString
                            // below; everything after it is cancelled.
                            EventRegistrationToken startToken;
                            impl->webview->add_NavigationStarting(
                                Callback<ICoreWebView2NavigationStartingEventHandler>(
                                    [impl](ICoreWebView2*,
                                           ICoreWebView2NavigationStartingEventArgs* args)
                                        -> HRESULT {
                                        if (impl->navigated) {
                                            args->put_Cancel(TRUE);
                                            impl->diag(L"navigation blocked");
                                        }
                                        impl->navigated = true;
                                        return S_OK;
                                    })
                                    .Get(),
                                &startToken);

                            RECT bounds;
                            GetClientRect(impl->window, &bounds);
                            controller->put_Bounds(bounds);

                            EventRegistrationToken navToken;
                            impl->webview->add_NavigationCompleted(
                                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [impl](ICoreWebView2*,
                                           ICoreWebView2NavigationCompletedEventArgs* args)
                                        -> HRESULT {
                                        BOOL success = FALSE;
                                        COREWEBVIEW2_WEB_ERROR_STATUS status =
                                            COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                                        args->get_IsSuccess(&success);
                                        args->get_WebErrorStatus(&status);
                                        wchar_t buffer[128];
                                        swprintf_s(buffer,
                                                   L"navigation completed: success=%d status=%d",
                                                   success ? 1 : 0, static_cast<int>(status));
                                        impl->diag(buffer);
                                        return S_OK;
                                    })
                                    .Get(),
                                &navToken);

                            const std::wstring page = utf8ToWide(kAppHtml);
                            impl->diag(L"page: " + std::to_wstring(page.size()) +
                                       L" characters");
                            impl->diagHr(L"NavigateToString",
                                         impl->webview->NavigateToString(page.c_str()));
                            return S_OK;
                        })
                        .Get());
                return S_OK;
            })
            .Get());

    if (FAILED(hr)) {
        *error = L"WebView2 unavailable: install the Microsoft Edge WebView2 runtime";
        return false;
    }

    ShowWindow(impl_->window, SW_SHOW);
    UpdateWindow(impl_->window);
    return true;
}

}  // namespace filo
