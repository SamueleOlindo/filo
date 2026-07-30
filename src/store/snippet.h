#pragma once

#include <string>
#include <vector>

namespace filo {

// Rebuilds the preview text around the match.
//
// The FTS5 table is contentless: it keeps the index but not the text. So the
// passage has to be recovered from somewhere else, and there are two cases.
//
// For .txt, source and markup the file on disk IS the text, so it is read and
// normalized again with the same steps indexing used — one read per result
// shown, and in exchange the index does not carry a second copy of everything.
//
// For PDFs and Office documents that is not possible: reading them as plain
// text yields the file's internals — object references, page boxes, font names
// — which is worse in a preview than showing nothing, and is exactly what used
// to appear. Those formats have their extracted text stored in the database
// instead, and `storedText` is how it gets here.
//
// `chunkOffset` and `chunkLength` are coordinates in the NORMALIZED text, not
// in the raw file.
std::string buildSnippet(const std::wstring& path, const std::string& storedText,
                         uint32_t chunkOffset, uint32_t chunkLength,
                         const std::vector<std::string>& terms, size_t maxChars);

// Splits the user's query into the terms to centre the preview on.
std::vector<std::string> splitTerms(const std::string& userQuery);

}  // namespace filo
