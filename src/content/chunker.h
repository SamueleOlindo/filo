#pragma once

#include <string>
#include <vector>

#include "store/content_db.h"

namespace filo {

// Splits text into indexable chunks.
//
// Why not index the whole document: BM25 rewards documents where the terms are
// DENSE. In a 200-page manual two words searched together could sit chapters
// apart and the score would treat them as neighbours. A chunk brings the
// comparison back to a scale where "close" really means close, and it is also
// the unit layer 3 computes embeddings over.
//
// A chunk is sized in TOKENS, against the embedding model's context window:
// past the window the model reads nothing and says nothing about it. The count
// is ESTIMATED from the bytes, never counted — the extraction pass has no
// tokenizer to ask — so the budget is set well under the window and the
// estimate leans long. See chunker.cpp for both numbers.
//
// The ranges returned are BYTE offsets inside `text`, and never fall in the
// middle of a UTF-8 character.
std::vector<ChunkRecord> chunkText(const std::string& text);

}  // namespace filo
