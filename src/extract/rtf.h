#pragma once

#include <cstddef>
#include <string>

namespace filo {

// Extracts the readable text from an RTF document.
//
// RTF is not markup the way HTML is: it is a GROUP-based format, where whole
// sections are font tables, colour palettes, embedded images and binary
// objects. The user's text is a fraction of the file, and reading it without
// skipping the right sections fills the index with typeface names and image
// bytes interpreted as words.
bool extractRtf(const char* data, size_t size, std::string* out);

}  // namespace filo
