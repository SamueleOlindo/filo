#pragma once

#include <cstddef>
#include <string>

namespace filo {

// Extracts the text from an EPUB.
//
// An EPUB is a ZIP holding XHTML chapters plus a manifest declaring their
// reading order. For indexing the order barely matters — nobody needs the
// chapters in sequence to find a sentence — so we take every document in
// natural name order instead of interpreting the manifest: less code, less
// attack surface, same useful result.
bool extractEpub(const char* data, size_t size, std::string* out);

}  // namespace filo
