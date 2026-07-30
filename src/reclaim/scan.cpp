#include "reclaim/scan.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <unordered_map>

#include "content/gate.h"
#include "search/matcher.h"

namespace filo {
namespace {

// Below this there is no point spreading the work: creating the threads costs
// more than the stats do.
constexpr size_t kMinRecordsPerThread = 20'000;

bool sameName(std::wstring_view indexed, const wchar_t* enumerated) {
    size_t i = 0;
    for (; i < indexed.size(); ++i) {
        const wchar_t other = enumerated[i];
        if (other == L'\0') return false;
        if (lowerFast(indexed[i]) != lowerFast(other)) return false;
    }
    return enumerated[i] == L'\0';
}

}  // namespace

ChildIndex buildChildIndex(const FileIndex& index) {
    const size_t count = index.size();
    ChildIndex out;
    out.offset.assign(count + 1, 0);

    for (size_t i = 0; i < count; ++i) {
        const uint32_t parent = index.parent(i);
        if (parent != kNoParent && parent < count) ++out.offset[parent + 1];
    }
    for (size_t i = 0; i < count; ++i) out.offset[i + 1] += out.offset[i];

    out.child.resize(out.offset[count]);
    std::vector<uint32_t> cursor(out.offset.begin(), out.offset.end() - 1);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t parent = index.parent(i);
        if (parent != kNoParent && parent < count) {
            out.child[cursor[parent]++] = static_cast<uint32_t>(i);
        }
    }
    return out;
}

std::vector<FileFact> collectFileFacts(const FileIndex& index, ScanStats* stats,
                                       unsigned threadCount) {
    const auto start = std::chrono::steady_clock::now();
    const size_t count = index.size();

    *stats = ScanStats{};
    stats->examined = count;
    if (count == 0) return {};

    const ChildIndex children = buildChildIndex(index);

    // Only the directories are visited; their contents come back in batches.
    std::vector<uint32_t> directories;
    directories.reserve(count / 4);
    for (size_t i = 0; i < count; ++i) {
        if (index.isDeleted(i)) continue;
        if (!(index.attributes(i) & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (index.attributes(i) & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (children.offset[i + 1] == children.offset[i]) continue;   // nothing inside
        directories.push_back(static_cast<uint32_t>(i));
    }

    if (threadCount == 0) threadCount = std::max(1u, std::thread::hardware_concurrency());
    threadCount = static_cast<unsigned>(std::max<size_t>(
        1, std::min<size_t>(threadCount, directories.size() / 1'000 + 1)));

    std::vector<std::vector<FileFact>> partial(threadCount);
    std::vector<ScanStats> partialStats(threadCount);
    std::vector<std::thread> workers;
    workers.reserve(threadCount - 1);

    const size_t slice = (directories.size() + threadCount - 1) / threadCount;

    auto measure = [&](unsigned worker) {
        const size_t begin = worker * slice;
        const size_t end = std::min(begin + slice, directories.size());
        auto& out = partial[worker];
        auto& mine = partialStats[worker];

        // Reused across directories so the allocation happens once per thread
        // rather than once per folder.
        std::unordered_map<std::wstring, uint32_t> byName;

        for (size_t d = begin; d < end; ++d) {
            const uint32_t directory = directories[d];
            const std::wstring pattern = index.fullPath(directory) + L"\\*";

            WIN32_FIND_DATAW found{};
            // FindExInfoBasic skips the 8.3 short name, which nobody here wants
            // and which the file system would otherwise look up for every entry.
            // LARGE_FETCH asks for a bigger buffer, so fewer round trips.
            const HANDLE search =
                FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &found,
                                 FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (search == INVALID_HANDLE_VALUE) continue;

            const uint32_t childBegin = children.offset[directory];
            const uint32_t childEnd = children.offset[directory + 1];

            // A linear walk per entry is quadratic in the size of the folder,
            // and the folders this feature exists to find are exactly the huge
            // flat ones: node_modules with ten thousand files costs a hundred
            // million comparisons on its own. Above a handful of children the
            // names go into a map first.
            byName.clear();
            const bool useMap = (childEnd - childBegin) > 16;
            if (useMap) {
                for (uint32_t k = childBegin; k < childEnd; ++k) {
                    const uint32_t candidate = children.child[k];
                    if (index.isDeleted(candidate)) continue;
                    byName.emplace(toLowerCopy(std::wstring(index.name(candidate))),
                                   candidate);
                }
            }

            do {
                if (found.cFileName[0] == L'.' &&
                    (found.cFileName[1] == L'\0' ||
                     (found.cFileName[1] == L'.' && found.cFileName[2] == L'\0'))) {
                    continue;
                }
                if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    ++mine.directories;
                    continue;
                }
                if (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    ++mine.reparsePoints;
                    continue;
                }
                if (isCloudPlaceholder(found.dwFileAttributes)) {
                    ++mine.cloudOnly;
                    continue;
                }

                // Attach the entry to its record. Directories hold four files
                // on average here, so a linear walk beats building a map.
                uint32_t slot = kNoParent;
                if (useMap) {
                    const auto it = byName.find(toLowerCopy(std::wstring(found.cFileName)));
                    if (it != byName.end()) slot = it->second;
                } else {
                    for (uint32_t k = childBegin; k < childEnd; ++k) {
                        const uint32_t candidate = children.child[k];
                        if (index.isDeleted(candidate)) continue;
                        if (!sameName(index.name(candidate), found.cFileName)) continue;
                        slot = candidate;
                        break;
                    }
                }
                if (slot == kNoParent) {
                    // On disk but not in the index: created since the snapshot,
                    // which on a machine that cannot read the USN journal is
                    // every file made since the last elevated scan.
                    ++mine.unreadable;
                    continue;
                }

                FileFact fact;
                fact.slot = slot;
                fact.size = (static_cast<uint64_t>(found.nFileSizeHigh) << 32) |
                            found.nFileSizeLow;
                fact.mtime = (static_cast<int64_t>(found.ftLastWriteTime.dwHighDateTime) << 32) |
                             found.ftLastWriteTime.dwLowDateTime;

                ++mine.measured;
                mine.totalBytes += fact.size;
                out.push_back(fact);
            } while (FindNextFileW(search, &found));

            FindClose(search);
        }
    };

    for (unsigned worker = 1; worker < threadCount; ++worker) {
        workers.emplace_back(measure, worker);
    }
    measure(0);
    for (std::thread& worker : workers) worker.join();

    for (const ScanStats& mine : partialStats) {
        stats->measured += mine.measured;
        stats->directories += mine.directories;
        stats->unreadable += mine.unreadable;
        stats->cloudOnly += mine.cloudOnly;
        stats->reparsePoints += mine.reparsePoints;
        stats->totalBytes += mine.totalBytes;
    }

    std::vector<FileFact> facts;
    facts.reserve(stats->measured);
    for (auto& part : partial) facts.insert(facts.end(), part.begin(), part.end());

    stats->threadsUsed = threadCount;
    stats->elapsedSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return facts;
}

std::vector<FileFact> collectFileFactsPerFile(const FileIndex& index, ScanStats* stats,
                                              unsigned threadCount) {
    const auto start = std::chrono::steady_clock::now();
    const size_t total = index.size();

    *stats = ScanStats{};
    stats->examined = total;
    if (total == 0) return {};

    if (threadCount == 0) {
        threadCount = std::max(1u, std::thread::hardware_concurrency());
    }
    threadCount = static_cast<unsigned>(
        std::max<size_t>(1, std::min<size_t>(threadCount, total / kMinRecordsPerThread + 1)));

    // Each thread fills its own vector: no shared writes, no lock, and the
    // slices concatenate in slot order at the end.
    std::vector<std::vector<FileFact>> partial(threadCount);
    std::vector<ScanStats> partialStats(threadCount);
    std::vector<std::thread> workers;
    workers.reserve(threadCount - 1);

    const size_t slice = (total + threadCount - 1) / threadCount;

    auto measure = [&](unsigned worker) {
        const size_t begin = worker * slice;
        const size_t end = std::min(begin + slice, total);
        auto& out = partial[worker];
        auto& mine = partialStats[worker];
        out.reserve((end - begin) / 2);   // roughly half of an NTFS volume is files

        for (size_t i = begin; i < end; ++i) {
            if (index.isDeleted(i)) continue;

            const uint32_t attributes = index.attributes(i);
            if (attributes & FILE_ATTRIBUTE_DIRECTORY) { ++mine.directories; continue; }
            if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                // A junction or symlink is a second name for bytes counted
                // elsewhere. Following it inflates every total and can loop.
                ++mine.reparsePoints;
                continue;
            }
            if (isCloudPlaceholder(attributes)) { ++mine.cloudOnly; continue; }

            const std::wstring path = index.fullPath(i);
            WIN32_FILE_ATTRIBUTE_DATA facts{};
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &facts)) {
                // Gone since the snapshot, locked, or denied. On a machine
                // without administrator rights the index cannot be refreshed
                // from the journal, so "gone" is the common case here and not
                // an error worth reporting.
                ++mine.unreadable;
                continue;
            }

            FileFact fact;
            fact.slot = static_cast<uint32_t>(i);
            fact.size = (static_cast<uint64_t>(facts.nFileSizeHigh) << 32) |
                        facts.nFileSizeLow;
            fact.mtime = (static_cast<int64_t>(facts.ftLastWriteTime.dwHighDateTime) << 32) |
                         facts.ftLastWriteTime.dwLowDateTime;

            ++mine.measured;
            mine.totalBytes += fact.size;
            out.push_back(fact);
        }
    };

    for (unsigned worker = 1; worker < threadCount; ++worker) {
        workers.emplace_back(measure, worker);
    }
    measure(0);
    for (std::thread& worker : workers) worker.join();

    for (const ScanStats& mine : partialStats) {
        stats->measured += mine.measured;
        stats->directories += mine.directories;
        stats->unreadable += mine.unreadable;
        stats->cloudOnly += mine.cloudOnly;
        stats->reparsePoints += mine.reparsePoints;
        stats->totalBytes += mine.totalBytes;
    }

    std::vector<FileFact> facts;
    facts.reserve(stats->measured);
    for (auto& part : partial) {
        facts.insert(facts.end(), part.begin(), part.end());
    }

    stats->threadsUsed = threadCount;
    stats->elapsedSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return facts;
}

}  // namespace filo
