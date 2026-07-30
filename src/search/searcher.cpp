#include "search/searcher.h"

#include <algorithm>
#include <thread>

#include "search/matcher.h"

namespace filo {

double nameRelevance(std::wstring_view name, const std::wstring& lowerNeedle) {
    if (lowerNeedle.empty() || name.empty()) return 0.0;

    // Position of the match, searched on the folded form.
    size_t at = std::wstring_view::npos;
    const size_t limit = name.size() >= lowerNeedle.size()
                             ? name.size() - lowerNeedle.size()
                             : 0;
    for (size_t i = 0; name.size() >= lowerNeedle.size() && i <= limit; ++i) {
        size_t j = 0;
        while (j < lowerNeedle.size() && lowerFast(name[i + j]) == lowerNeedle[j]) ++j;
        if (j == lowerNeedle.size()) { at = i; break; }
    }
    if (at == std::wstring_view::npos) return 0.0;

    double score = 1.0;

    // The name IS the query: nothing can beat that.
    if (name.size() == lowerNeedle.size()) score += 6.0;
    // Starts with the query: almost as strong.
    else if (at == 0) score += 4.0;
    // Starts a word: "invoice" inside "new invoice.pdf".
    else {
        const wchar_t before = name[at - 1];
        if (before == L' ' || before == L'-' || before == L'_' || before == L'.') {
            score += 2.5;
        }
    }

    // The query covers much of the name: the more it does, the more the file IS
    // that thing rather than merely mentioning it.
    score += 3.0 * static_cast<double>(lowerNeedle.size()) /
             static_cast<double>(name.size());

    // All else equal, short names first: those are the ones a person wrote, not
    // the ones generated with suffixes and copy numbers.
    score -= 0.004 * static_cast<double>(name.size());
    return score;
}

std::vector<uint32_t> searchNames(const FileIndex& index,
                                  const std::wstring& lowerNeedle,
                                  unsigned threadCount) {
    const size_t total = index.size();
    if (total == 0 || lowerNeedle.empty()) return {};

    if (threadCount == 0) {
        threadCount = std::max(1u, std::thread::hardware_concurrency());
    }
    // Below a certain size the cost of spawning threads exceeds the gain.
    if (total < 50'000) threadCount = 1;
    threadCount = static_cast<unsigned>(
        std::min<size_t>(threadCount, (total + 9'999) / 10'000));

    // Raw pointers taken once: inside the hot loop we do not even want the cost
    // of re-reading the vector through the object.
    const wchar_t* const pool = index.namePool();
    const uint32_t* const offsets = index.nameOffsets();
    const uint16_t* const lengths = index.nameLengths();
    const uint32_t* const attributes = index.attributesData();
    const wchar_t* const needle = lowerNeedle.data();
    const size_t needleLength = lowerNeedle.size();

    // Every thread writes into its OWN vector: no shared writes, therefore no
    // lock and no false sharing on the cache lines.
    std::vector<std::vector<uint32_t>> partial(threadCount);
    std::vector<std::thread> workers;
    workers.reserve(threadCount - 1);

    const size_t chunk = (total + threadCount - 1) / threadCount;

    auto scanRange = [&](unsigned slot) {
        const size_t begin = slot * chunk;
        const size_t end = std::min(begin + chunk, total);
        auto& out = partial[slot];

        for (size_t i = begin; i < end; ++i) {
            const uint32_t start = offsets[i];
            const uint32_t length = lengths[i];
            if (containsCaseInsensitive(pool + start, length, needle, needleLength)) {
                // The tombstone filter sits HERE and not before the comparison:
                // it fires only on the handful of records that match, instead
                // of adding a memory read across all 2.4 million.
                if ((attributes[i] & kDeletedAttribute) == 0) {
                    out.push_back(static_cast<uint32_t>(i));
                }
            }
        }
    };

    // The calling thread takes its own slice instead of sitting there waiting.
    for (unsigned slot = 1; slot < threadCount; ++slot) {
        workers.emplace_back(scanRange, slot);
    }
    scanRange(0);

    for (auto& worker : workers) worker.join();

    // The slices are contiguous and in order, so concatenating them in slot
    // order already yields ascending indices: no final sort to pay for.
    size_t hitCount = 0;
    for (const auto& slice : partial) hitCount += slice.size();

    std::vector<uint32_t> hits;
    hits.reserve(hitCount);
    for (const auto& slice : partial) {
        hits.insert(hits.end(), slice.begin(), slice.end());
    }
    return hits;
}

}  // namespace filo
