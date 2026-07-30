#include "store/query.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>

namespace filo {
namespace {

// Hard ceiling on the chunk rows scored. FTS5 has no top-k pruning: cost is
// linear in the matching rows (~0.8 us each), so this number is also the
// ceiling on latency.
//
// It was 500, and that was a bug: being a limit on CHUNKS and not on DOCUMENTS,
// a single log containing the word on every line filled all the slots on its
// own, and the search returned few documents even when hundreds held the term.
// Now it reads much deeper and prunes in C++, once enough DISTINCT documents
// have actually been collected.
constexpr int kChunkLimit = 20000;

// Minimum rows to read before an early exit is allowed.
constexpr size_t kMinRowsScanned = 500;

// How many chunks of one document contribute to the passage count. Past that,
// the score bonus would start rewarding repetitive files.
constexpr int kMaxPassagesCounted = 8;

bool isSeparator(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Does the term contain at least one character the tokenizer would recognize?
//
// A punctuation-only term — the dash in "report - 2024", an asterisk, an em
// dash pasted out of Word — produces ZERO tokens, and FTS5 turns an empty
// phrase into a node that never matches anything: the AND containing it zeroes
// the ENTIRE content search, silently, while the name search keeps answering
// and makes everything look normal.
bool hasTokenChar(const std::string& term) {
    for (size_t i = 0; i < term.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(term[i]);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
            return true;
        }
        // Above ASCII it is nearly always an accented letter: it takes the
        // lead byte of a multi-byte sequence that is not Latin punctuation,
        // which lives in the 0xC2 0x80-0xBF block.
        if (c >= 0xC3) return true;
        if (c == 0xC2 && i + 1 < term.size()) {
            const unsigned char next = static_cast<unsigned char>(term[i + 1]);
            if (next >= 0xC0) return true;   // letters, not symbols
        }
    }
    return false;
}

}  // namespace

std::string buildMatchExpression(const std::string& userQuery) {
    std::string expression;
    size_t i = 0;

    while (i < userQuery.size()) {
        while (i < userQuery.size() && isSeparator(userQuery[i])) ++i;
        if (i >= userQuery.size()) break;

        std::string term;
        if (userQuery[i] == '"') {
            // The user asked for an exact phrase: keep it whole.
            ++i;
            while (i < userQuery.size() && userQuery[i] != '"') term += userQuery[i++];
            if (i < userQuery.size()) ++i;
        } else {
            while (i < userQuery.size() && !isSeparator(userQuery[i])) term += userQuery[i++];
        }
        if (term.empty() || !hasTokenChar(term)) continue;

        // Inner quotes are doubled, as in SQL.
        std::string quoted = "\"";
        for (const char c : term) {
            if (c == '"') quoted += '"';
            quoted += c;
        }
        quoted += '"';

        if (!expression.empty()) expression += " AND ";
        expression += quoted;
    }
    return expression;
}

std::vector<ContentHit> searchContent(ContentDb& db, const std::string& userQuery,
                                      size_t limit, double* elapsedMs, bool* truncated) {
    if (truncated) *truncated = false;
    const auto start = std::chrono::steady_clock::now();
    std::vector<ContentHit> results;

    const std::string expression = buildMatchExpression(userQuery);
    if (expression.empty()) return results;

    // ORDER BY bm25(ft), NOT "ORDER BY rank".
    //
    // It looks equivalent but is measurably slower: with "rank" FTS5 builds an
    // internal query that sorts WITHOUT being given the LIMIT, whereas this way
    // the sort happens in SQLite's core, which does have the limit.
    // Do not put "rank" back thinking you are simplifying.
    static const char* kSql =
        "SELECT chunk.doc_id, chunk.chunk_id, bm25(ft) AS s "
        "FROM ft, chunk "
        "WHERE ft MATCH ? AND chunk.chunk_id = ft.rowid "
        "ORDER BY s LIMIT ?";

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db.handle(), kSql, -1, &statement, nullptr) != SQLITE_OK) {
        return results;
    }
    sqlite3_bind_text(statement, 1, expression.c_str(),
                      static_cast<int>(expression.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 2, kChunkLimit);

    // Chunk -> document aggregation, done here and not in SQL: a GROUP BY would
    // force SQLite to materialize and re-sort, whereas the rows already arrive
    // ordered by score and all we need is the first hit per document plus the
    // passage count.
    std::unordered_map<int64_t, ContentHit> byDocument;
    size_t rowsScanned = 0;
    bool stoppedEarly = false;

    while (sqlite3_step(statement) == SQLITE_ROW) {
        ++rowsScanned;
        const int64_t docId = sqlite3_column_int64(statement, 0);
        const int64_t chunkId = sqlite3_column_int64(statement, 1);
        const double score = sqlite3_column_double(statement, 2);

        auto it = byDocument.find(docId);
        if (it == byDocument.end()) {
            ContentHit hit;
            hit.docId = docId;
            hit.bestChunkId = chunkId;
            hit.score = score;
            hit.passages = 1;
            byDocument.emplace(docId, hit);
        } else if (it->second.passages < kMaxPassagesCounted) {
            ++it->second.passages;
        }

        // We stop only once there are enough DISTINCT documents, not enough
        // rows: that is the difference between pruning the results and
        // truncating them on a single repetitive file.
        if (byDocument.size() >= limit * 3 && rowsScanned >= kMinRowsScanned) {
            stoppedEarly = true;
            break;
        }
    }
    sqlite3_finalize(statement);
    if (truncated) *truncated = stoppedEarly || rowsScanned >= kChunkLimit;

    // The penalty applies to the aggregated documents, not the rows: a few
    // hundred instead of tens of thousands, and the lookup in the hot loop
    // stays free of extra queries.
    if (!byDocument.empty()) {
        sqlite3_stmt* penalty = nullptr;
        if (sqlite3_prepare_v2(db.handle(), "SELECT rank_penalty FROM doc WHERE doc_id = ?",
                               -1, &penalty, nullptr) == SQLITE_OK) {
            for (auto& entry : byDocument) {
                sqlite3_reset(penalty);
                sqlite3_bind_int64(penalty, 1, entry.first);
                if (sqlite3_step(penalty) == SQLITE_ROW) {
                    // bm25 is negative and lower means better: adding the
                    // penalty pushes the document down the ranking.
                    entry.second.score += sqlite3_column_double(penalty, 0);
                }
            }
            sqlite3_finalize(penalty);
        }
    }

    results.reserve(byDocument.size());
    for (auto& entry : byDocument) {
        // A document that answers in several places is more relevant than one
        // that answers in a single spot. Logarithmic: the second passage counts
        // a lot, the tenth almost nothing. bm25 is negative, hence the
        // subtraction.
        entry.second.score -= 0.6 * std::log(1.0 + entry.second.passages);
        results.push_back(entry.second);
    }
    std::sort(results.begin(), results.end(),
              [](const ContentHit& a, const ContentHit& b) { return a.score < b.score; });
    if (results.size() > limit) results.resize(limit);

    // The metadata is only needed for the few documents we will actually show.
    sqlite3_stmt* meta = nullptr;
    if (sqlite3_prepare_v2(db.handle(),
                           "SELECT volume_id, frn FROM doc WHERE doc_id = ?", -1,
                           &meta, nullptr) == SQLITE_OK) {
        for (ContentHit& hit : results) {
            sqlite3_reset(meta);
            sqlite3_bind_int64(meta, 1, hit.docId);
            if (sqlite3_step(meta) == SQLITE_ROW) {
                hit.volumeId = static_cast<uint32_t>(sqlite3_column_int64(meta, 0));
                hit.frn = static_cast<uint64_t>(sqlite3_column_int64(meta, 1));
            }
        }
        sqlite3_finalize(meta);
    }

    *elapsedMs = std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - start).count();
    return results;
}

bool documentOfChunk(ContentDb& db, int64_t chunkId, int64_t* docId, uint64_t* frn,
                     double* rankPenalty) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db.handle(),
                           "SELECT d.doc_id, d.frn, d.rank_penalty FROM chunk c "
                           "JOIN doc d ON d.doc_id = c.doc_id WHERE c.chunk_id = ?",
                           -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(statement, 1, chunkId);

    bool found = false;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        *docId = sqlite3_column_int64(statement, 0);
        *frn = static_cast<uint64_t>(sqlite3_column_int64(statement, 1));
        if (rankPenalty) *rankPenalty = sqlite3_column_double(statement, 2);
        found = true;
    }
    sqlite3_finalize(statement);
    return found;
}

bool chunkRange(ContentDb& db, int64_t chunkId, uint32_t* offset, uint32_t* length,
                int64_t* docId) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db.handle(),
                           "SELECT boff, blen, doc_id FROM chunk WHERE chunk_id = ?", -1,
                           &statement, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(statement, 1, chunkId);
    bool found = false;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        *offset = static_cast<uint32_t>(sqlite3_column_int(statement, 0));
        *length = static_cast<uint32_t>(sqlite3_column_int(statement, 1));
        if (docId) *docId = sqlite3_column_int64(statement, 2);
        found = true;
    }
    sqlite3_finalize(statement);
    return found;
}

}  // namespace filo
