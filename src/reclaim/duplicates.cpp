#include "reclaim/duplicates.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "reclaim/paths.h"

// Header-only: the implementation comes with the include, per translation unit.
#define XXH_INLINE_ALL
#include <xxhash.h>

#include "search/matcher.h"

namespace filo {
namespace {

// Below this a duplicate is not worth the risk of getting it wrong. NTFS keeps
// files this small inside the MFT record itself, so deleting them frees very
// little even in bulk.
constexpr uint64_t kMinimumSize = 4096;

// Read from each end for the fingerprint. Enough to separate files that merely
// share a size; small enough that the cost does not depend on how big they are.
constexpr uint64_t kEdgeBytes = 64 * 1024;

constexpr uint64_t kFullHashChunk = 1u << 20;

struct Fingerprint {
    uint64_t low = 0;
    uint64_t high = 0;
    bool operator==(const Fingerprint& other) const {
        return low == other.low && high == other.high;
    }
};

struct FingerprintHash {
    size_t operator()(const Fingerprint& value) const {
        return static_cast<size_t>(value.low ^ (value.high * 0x9E3779B97F4A7C15ull));
    }
};

HANDLE openForReading(const std::wstring& path) {
    return CreateFileW(path.c_str(), GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                       OPEN_EXISTING,
                       FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_NO_RECALL, nullptr);
}

bool readAt(HANDLE file, uint64_t offset, void* buffer, DWORD bytes, DWORD* read) {
    LARGE_INTEGER where;
    where.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, where, nullptr, FILE_BEGIN)) return false;
    return ReadFile(file, buffer, bytes, read, nullptr) != 0;
}

// Hashes the ends of a file, plus its size so that two files differing only in
// their middle at least differ here when the middles are shorter than the ends.
bool fingerprintOf(const std::wstring& path, uint64_t size, Fingerprint* out,
                   uint64_t* bytesRead) {
    const HANDLE file = openForReading(path);
    if (file == INVALID_HANDLE_VALUE) return false;

    std::vector<char> buffer(static_cast<size_t>(std::min(size, kEdgeBytes) * 2 + 8));
    size_t filled = 0;

    DWORD got = 0;
    const DWORD head = static_cast<DWORD>(std::min(size, kEdgeBytes));
    if (!readAt(file, 0, buffer.data(), head, &got)) {
        CloseHandle(file);
        return false;
    }
    filled += got;

    if (size > kEdgeBytes) {
        const DWORD tail = static_cast<DWORD>(std::min(size - kEdgeBytes, kEdgeBytes));
        DWORD tailGot = 0;
        if (readAt(file, size - tail, buffer.data() + filled, tail, &tailGot)) {
            filled += tailGot;
        }
    }
    CloseHandle(file);

    std::memcpy(buffer.data() + filled, &size, sizeof(size));
    filled += sizeof(size);

    const XXH128_hash_t hash = XXH3_128bits(buffer.data(), filled);
    out->low = hash.low64;
    out->high = hash.high64;
    *bytesRead = filled;
    return true;
}

bool fullHashOf(const std::wstring& path, Fingerprint* out, uint64_t* bytesRead) {
    const HANDLE file = openForReading(path);
    if (file == INVALID_HANDLE_VALUE) return false;

    XXH3_state_t* state = XXH3_createState();
    if (!state) {
        CloseHandle(file);
        return false;
    }
    XXH3_128bits_reset(state);

    std::vector<char> buffer(kFullHashChunk);
    uint64_t total = 0;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &got,
                      nullptr)) {
            XXH3_freeState(state);
            CloseHandle(file);
            return false;
        }
        if (got == 0) break;
        XXH3_128bits_update(state, buffer.data(), got);
        total += got;
    }
    CloseHandle(file);

    const XXH128_hash_t hash = XXH3_128bits_digest(state);
    XXH3_freeState(state);
    out->low = hash.low64;
    out->high = hash.high64;
    *bytesRead = total;
    return true;
}

// Places where a duplicate is not a duplicate, it is the design.
//
// This was not in the plan; it came out of running the thing. The first results
// on a real disk proposed deleting objects inside `.git/lfs`, files inside the
// pnpm content store, and copies inside node_modules — all of them genuinely
// byte-identical, and every one of those deletions breaks something. A package
// store keeps one copy per hash ON PURPOSE and the tree that points at it is
// not redundant; a git object IS the repository.
//
// These are not exceptions to the duplicate rule, they belong to a different
// category: regenerable, where the honest offer is "delete the whole
// node_modules, your package manager will rebuild it" and never "delete this
// one file out of the middle of it".
bool inManagedLocation(const std::wstring& lowerPath) {
    for (const wchar_t* marker : {
             L"\\.git\\",           L"\\.svn\\",          L"\\.hg\\",
             L"\\node_modules\\",   L"\\.pnpm-store\\",   L"\\pnpm\\store\\",
             L"\\.cargo\\registry\\", L"\\.nuget\\packages\\", L"\\.m2\\repository\\",
             L"\\.gradle\\caches\\", L"\\site-packages\\", L"\\ms-playwright\\",
             L"\\.yarn\\cache\\",   L"\\winsxs\\",        L"\\servicing\\",
             L"\\.vscode\\extensions\\", L"\\installer\\",
         }) {
        if (lowerPath.find(marker) != std::wstring::npos) return true;
    }
    return false;
}

// The user's own area: their profile, minus the parts applications own.
//
// THIS IS AN ALLOWLIST, AND THAT IS THE POINT.
//
// The first version excluded dangerous places one marker at a time and the list
// never stopped growing: git objects, package stores, then Edge profile data,
// then a vector database's segments, then a game launcher's depots, then files
// belonging to ANOTHER USER of the same machine. Every run of the real thing
// found another one, which is the signature of a rule stated backwards.
//
// Turned around it is short and it holds: a copy is safe to remove only when
// the person deleting it is the one who put it there. Two users each having
// their own copy of an application's data is correct, not wasteful. AppData is
// application state, not documents. Program Files belongs to an installer.
//
// The cost is real — duplicated installers under AppData are genuine waste this
// will not offer — and it is the right cost to pay. Those belong to categories
// with their own rules, not to "identical, therefore removable".
bool inUserArea(const std::wstring& lowerPath, const std::wstring& lowerProfile) {
    if (lowerProfile.empty()) return false;
    if (!insideFolder(lowerPath, lowerProfile)) return false;

    for (const wchar_t* owned : {L"\\appdata\\", L"\\.local\\share\\", L"\\.cache\\",
                                 L"\\.config\\", L"\\ntuser", L"\\.ollama\\"}) {
        if (lowerPath.find(owned) != std::wstring::npos) return false;
    }
    return true;
}

// Output a tool produced from something else that is also on this disk.
//
// It matters for choosing which copy to keep, and the first run got it exactly
// backwards: it kept build/static/media/video-1.a1061371.mp4 and offered to
// bin src/media/.../video-1.mp4, because the build path happened to be
// shallower. Deleting the source to keep the artifact is the wrong way round in
// every case — the artifact can be rebuilt from the source and never the
// reverse.
bool looksGenerated(const std::wstring& lowerPath) {
    for (const wchar_t* marker : {L"\\build\\", L"\\dist\\", L"\\out\\", L"\\target\\",
                                  L"\\obj\\", L"\\.next\\", L"\\coverage\\",
                                  L"\\__pycache__\\", L"\\bin\\debug\\",
                                  L"\\bin\\release\\"}) {
        if (lowerPath.find(marker) != std::wstring::npos) return true;
    }
    return false;
}

bool inTransitFolder(const std::wstring& lowerPath) {
    for (const wchar_t* marker : {L"\\downloads\\", L"\\temp\\", L"\\tmp\\",
                                  L"\\appdata\\local\\temp\\", L"\\cestino\\",
                                  L"\\$recycle.bin\\"}) {
        if (lowerPath.find(marker) != std::wstring::npos) return true;
    }
    return false;
}

size_t depthOf(const std::wstring& path) {
    return static_cast<size_t>(std::count(path.begin(), path.end(), L'\\'));
}

}  // namespace

uint32_t chooseKeeper(const FileIndex& index, const std::vector<FileFact>& facts,
                      const std::vector<uint32_t>& members) {
    std::unordered_map<uint32_t, int64_t> mtimes;
    for (const FileFact& fact : facts) mtimes[fact.slot] = fact.mtime;

    uint32_t best = members.empty() ? kNoParent : members.front();
    std::wstring bestPath;
    bool bestGenerated = true;
    bool bestInTransit = true;
    size_t bestDepth = SIZE_MAX;
    int64_t bestMtime = INT64_MAX;

    for (const uint32_t slot : members) {
        const std::wstring path = index.fullPath(slot);
        const std::wstring lower = toLowerCopy(path);
        const bool generated = looksGenerated(lower);
        const bool transit = inTransitFolder(lower);
        const size_t depth = depthOf(path);
        const auto found = mtimes.find(slot);
        const int64_t mtime = found == mtimes.end() ? 0 : found->second;

        // In order: never keep the artifact over the source it was built from;
        // then a file lives where it was filed and accumulates copies where it
        // passed through, so Downloads loses to anywhere else; then the
        // shallowest path; then the oldest, which is the copy other things are
        // most likely to already point at.
        const bool better =
            (generated != bestGenerated) ? (!generated)
            : (transit != bestInTransit) ? (!transit)
            : (depth != bestDepth)       ? (depth < bestDepth)
                                         : (mtime < bestMtime);
        if (better || bestPath.empty()) {
            best = slot;
            bestPath = path;
            bestGenerated = generated;
            bestInTransit = transit;
            bestDepth = depth;
            bestMtime = mtime;
        }
    }
    return best;
}

std::vector<SizeGroup> groupBySize(const FileIndex& index,
                                   const std::vector<FileFact>& facts,
                                   uint64_t minimumSize) {
    wchar_t profile[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    const std::wstring lowerProfile =
        (length > 0 && length < MAX_PATH) ? toLowerCopy(profile) : std::wstring();

    std::unordered_map<uint64_t, std::vector<uint32_t>> bySize;
    for (const FileFact& fact : facts) {
        if (fact.size < minimumSize) continue;
        const std::wstring lower = toLowerCopy(index.fullPath(fact.slot));
        if (!inUserArea(lower, lowerProfile)) continue;
        if (inManagedLocation(lower)) continue;
        bySize[fact.size].push_back(fact.slot);
    }

    std::vector<SizeGroup> groups;
    groups.reserve(bySize.size() / 8);
    for (auto& entry : bySize) {
        if (entry.second.size() < 2) continue;
        SizeGroup group;
        group.size = entry.first;
        group.slots = std::move(entry.second);
        groups.push_back(std::move(group));
    }

    // Biggest possible saving first. A budget spent in this order buys the most
    // it can, and stopping early costs the least.
    std::sort(groups.begin(), groups.end(),
              [](const SizeGroup& a, const SizeGroup& b) {
                  return a.potential() > b.potential();
              });
    return groups;
}

std::vector<DuplicateGroup> findExactDuplicates(const FileIndex& index,
                                                const std::vector<FileFact>& facts,
                                                const DuplicateOptions& options,
                                                DuplicateStats* stats,
                                                unsigned threadCount) {
    *stats = DuplicateStats{};
    if (threadCount == 0) threadCount = std::max(1u, std::thread::hardware_concurrency());

    // --- stage 1: size ------------------------------------------------------
    //
    // Free, and it throws away almost everything. No disk access at all.
    auto stageStart = std::chrono::steady_clock::now();
    for (const FileFact& fact : facts) {
        if (fact.size >= options.minimumSize) ++stats->considered;
    }

    std::vector<SizeGroup> sized = groupBySize(index, facts, options.minimumSize);

    // Spend the budget on the largest wins. Beyond it the groups are still
    // there; they simply have not been confirmed yet.
    std::vector<std::pair<uint64_t, std::vector<uint32_t>>> candidates;
    size_t budget = 0;
    for (SizeGroup& group : sized) {
        if (options.fileBudget != 0 && budget + group.slots.size() > options.fileBudget) {
            break;
        }
        budget += group.slots.size();
        stats->sharingASize += group.slots.size();
        candidates.emplace_back(group.size, std::move(group.slots));
    }
    stats->sizeSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - stageStart).count();
    if (candidates.empty()) return {};

    // --- stage 2: fingerprint the ends --------------------------------------
    stageStart = std::chrono::steady_clock::now();
    struct Candidate {
        uint64_t size = 0;
        uint32_t slot = 0;
        Fingerprint print;
        bool ok = false;
    };
    std::vector<Candidate> work;
    work.reserve(stats->sharingASize);
    for (const auto& entry : candidates) {
        for (const uint32_t slot : entry.second) work.push_back({entry.first, slot, {}, false});
    }

    std::atomic<uint64_t> printedBytes{0};
    std::atomic<uint64_t> failed{0};
    std::atomic<uint64_t> done{0};
    std::mutex progressLock;
    {
        std::vector<std::thread> workers;
        const size_t slice = (work.size() + threadCount - 1) / threadCount;
        auto run = [&](unsigned worker) {
            const size_t begin = worker * slice;
            const size_t end = std::min(begin + slice, work.size());
            uint64_t mine = 0, mineFailed = 0;
            for (size_t i = begin; i < end; ++i) {
                uint64_t read = 0;
                if (fingerprintOf(index.fullPath(work[i].slot), work[i].size,
                                  &work[i].print, &read)) {
                    work[i].ok = true;
                    mine += read;
                } else {
                    ++mineFailed;
                }
                if (options.onProgress && (done.fetch_add(1) % 64) == 0) {
                    std::lock_guard<std::mutex> lock(progressLock);
                    options.onProgress(L"Comparing candidates", done.load(), work.size());
                }
            }
            printedBytes += mine;
            failed += mineFailed;
        };
        for (unsigned worker = 1; worker < threadCount; ++worker) {
            workers.emplace_back(run, worker);
        }
        run(0);
        for (std::thread& worker : workers) worker.join();
    }
    stats->fingerprinted = work.size();
    stats->fingerprintBytes = printedBytes.load();
    stats->unreadable = failed.load();
    stats->fingerprintSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - stageStart).count();

    // --- stage 3: confirm with a full hash ----------------------------------
    //
    // Only where size AND both ends agree. Two different files can still get
    // this far — disk images share headers, database files share tails — and
    // this is about to propose a deletion.
    stageStart = std::chrono::steady_clock::now();
    std::unordered_map<Fingerprint, std::vector<size_t>, FingerprintHash> byPrint;
    for (size_t i = 0; i < work.size(); ++i) {
        if (work[i].ok) byPrint[work[i].print].push_back(i);
    }

    std::vector<std::vector<size_t>> toConfirm;
    for (auto& entry : byPrint) {
        if (entry.second.size() >= 2) toConfirm.push_back(std::move(entry.second));
    }

    std::vector<DuplicateGroup> groups;
    std::atomic<uint64_t> hashedBytes{0};
    std::atomic<uint64_t> hashedCount{0};
    std::mutex groupLock;
    {
        std::vector<std::thread> workers;
        std::atomic<size_t> next{0};
        auto run = [&]() {
            for (;;) {
                const size_t at = next.fetch_add(1);
                if (at >= toConfirm.size()) return;
                if (options.onProgress) {
                    std::lock_guard<std::mutex> lock(groupLock);
                    options.onProgress(L"Confirming byte for byte", at, toConfirm.size());
                }
                const std::vector<size_t>& members = toConfirm[at];

                std::unordered_map<Fingerprint, std::vector<uint32_t>, FingerprintHash>
                    confirmed;
                std::unordered_map<Fingerprint, std::vector<uint64_t>, FingerprintHash> frns;
                uint64_t mineBytes = 0, mineCount = 0;

                for (const size_t which : members) {
                    Fingerprint full;
                    uint64_t read = 0;
                    if (!fullHashOf(index.fullPath(work[which].slot), &full, &read)) continue;
                    mineBytes += read;
                    ++mineCount;

                    // Hardlinks: one MFT record wearing two names. Identical by
                    // construction, and deleting one frees nothing.
                    const uint64_t frn = index.frn(work[which].slot);
                    auto& seen = frns[full];
                    if (std::find(seen.begin(), seen.end(), frn) != seen.end()) continue;
                    seen.push_back(frn);
                    confirmed[full].push_back(work[which].slot);
                }
                hashedBytes += mineBytes;
                hashedCount += mineCount;

                for (auto& entry : confirmed) {
                    if (entry.second.size() < 2) continue;
                    DuplicateGroup group;
                    group.bytesEach = work[members.front()].size;
                    group.reclaimable = group.bytesEach * (entry.second.size() - 1);

                    const uint32_t keeper = chooseKeeper(index, facts, entry.second);
                    group.slots.push_back(keeper);
                    for (const uint32_t slot : entry.second) {
                        if (slot != keeper) group.slots.push_back(slot);
                    }

                    std::lock_guard<std::mutex> lock(groupLock);
                    groups.push_back(std::move(group));
                }
            }
        };
        for (unsigned worker = 1; worker < threadCount; ++worker) workers.emplace_back(run);
        run();
        for (std::thread& worker : workers) worker.join();
    }
    stats->fullyHashed = hashedCount.load();
    stats->fullHashBytes = hashedBytes.load();
    stats->fullHashSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - stageStart).count();

    // Count the hardlinks we refused to promise savings for.
    stats->hardlinks = hashedCount.load();
    for (const DuplicateGroup& group : groups) stats->hardlinks -= group.slots.size();

    std::sort(groups.begin(), groups.end(),
              [](const DuplicateGroup& a, const DuplicateGroup& b) {
                  return a.reclaimable > b.reclaimable;
              });

    stats->groups = groups.size();
    for (const DuplicateGroup& group : groups) stats->reclaimableBytes += group.reclaimable;
    return groups;
}

}  // namespace filo
