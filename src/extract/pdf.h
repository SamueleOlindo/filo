#pragma once

#include <cstddef>
#include <string>

namespace filo {

enum class PdfResult {
    Ok,
    NotAPdf,
    Encrypted,   // password protected: the text is out of reach
    NeedsOcr,    // image-only pages: there is no text to extract
    Failed,
};

// Extracts the text from a PDF.
//
// ALWAYS runs inside the isolated worker, and that is not a theoretical
// precaution: PDF is the most complex and most often malformed format we will
// meet, and its parsers are historically the number one source of
// vulnerabilities.
//
// Telling "contains no text" apart from "I could not read it" is part of the
// result, not a detail: a scanned invoice is an image and no engine will ever
// pull a word out of it. Saying so is the only way the user does not conclude
// the search is broken.
PdfResult extractPdf(const char* data, size_t size, std::string* out,
                     int* pageCount, int* pagesWithText);

// Call once per process, before first use.
void initPdfEngine();
void shutdownPdfEngine();

}  // namespace filo
