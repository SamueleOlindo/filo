#pragma once

#include <cstddef>
#include <string>

namespace filo {

// Extracts the readable text from HTML, XHTML and MHTML web archives.
//
// Three distinct jobs, which have to happen in this order:
//  1. Recognizing the ENCODING the document declares about itself. A page can
//     say it is windows-1252 in its own <meta>, and believing it to be UTF-8
//     means sending corrupted accents into the index.
//  2. Removing the tags, but also the CONTENT of scripts and stylesheets,
//     which are code and would be pure noise in the index.
//  3. Resolving entities: "&amp;egrave;" and an accented "e" have to become the
//     same character, or they are two different words.
bool extractHtml(const char* data, size_t size, std::string* out);

// Strips tags from HTML already decoded to UTF-8. Exposed because the EPUB and
// RTF extractors need it too (mail attachments wrap HTML inside RTF).
void stripHtmlToText(const char* data, size_t size, std::string* out);

}  // namespace filo
