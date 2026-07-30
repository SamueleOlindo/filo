#pragma once

#include <string>

#include "content/gate.h"
#include "content/selector.h"

namespace filo {

// Turns a file's raw bytes into clean UTF-8 text, ready for the index.
//
// Three distinct jobs, all of them necessary:
//  1. CONVERTING the encoding. A UTF-16 file copied as-is produces strings full
//     of zeros; a latin-1 one produces broken accents.
//  2. STRIPPING control characters, which the index has no use for and which
//     break snippets.
//  3. PRESERVING paragraph boundaries, because those are where the chunker
//     cuts: losing them means splitting sentences in half.
//
// For markup it also removes the tags: without that the index fills up with
// "div", "span" and "xmlns", which are not content and skew the BM25 score.
bool normalizeToUtf8(const char* data, size_t size, TextEncoding encoding,
                     ContentKind kind, std::string* out);

}  // namespace filo
