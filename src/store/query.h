#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "store/content_db.h"

namespace filo {

struct ContentHit {
    int64_t  docId = 0;
    uint32_t volumeId = 0;
    uint64_t frn = 0;
    double   score = 0.0;   // lower = more relevant (bm25 convention)
    int      passages = 0;  // how many distinct chunks it appears in
    int64_t  bestChunkId = 0;
};

// Translates what the user typed into FTS5 syntax.
//
// The raw text cannot be passed through: quotes, asterisks, parentheses and the
// word OR carry special meaning, and a single apostrophe is enough to make the
// query fail. Every term is wrapped in quotes (which in FTS5 means "literal")
// and joined to the others with AND.
//
// The AND is also a defence: FTS5 cannot prune results, so cost grows with the
// number of matching rows. An implicit OR on a common word would produce
// hundreds of thousands of rows to score.
std::string buildMatchExpression(const std::string& userQuery);

// Searches the content and aggregates chunks per document.
// `truncated`, when provided, says whether the search stopped reading before
// exhausting the matches: the list is then partial and must be presented as
// such, rather than implying those are all the documents there are.
std::vector<ContentHit> searchContent(ContentDb& db, const std::string& userQuery,
                                      size_t limit, double* elapsedMs,
                                      bool* truncated = nullptr);

// Where a chunk sits inside its document's normalized text, and which document
// that is — the doc_id is what fetches stored text for formats that cannot be
// re-read from the file.
bool chunkRange(ContentDb& db, int64_t chunkId, uint32_t* offset, uint32_t* length,
                int64_t* docId);

// The document a chunk belongs to: row id, file reference number and rank
// penalty. Semantic search finds CHUNKS; the result list is made of FILES, so
// the two have to be reconciled before the lists can be fused.
//
// The penalty travels with it because the semantic voter needs it as much as
// the lexical one — arguably more. Machine-generated files match a surprising
// number of things "by meaning" without being about any of them.
bool documentOfChunk(ContentDb& db, int64_t chunkId, int64_t* docId, uint64_t* frn,
                     double* rankPenalty);

}  // namespace filo
