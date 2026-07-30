#pragma once

#include <cstddef>
#include <string>

namespace filo {

enum class OfficeResult {
    Ok,
    NotAnArchive,
    Encrypted,     // CFB container with the encrypted package inside
    Unsupported,
    Empty,
};

// Extracts the text from DOCX, XLSX, PPTX and the OpenDocument formats.
//
// They are all ZIP archives containing XML. The work is figuring out WHICH
// parts hold text meant for the user and which are scaffolding: in a .docx the
// visible text sits in w:t, but the same file also carries styles, revisions,
// field codes and metadata that would be nothing but noise in the index.
OfficeResult extractOfficeXml(const char* data, size_t size, std::string* out);

}  // namespace filo
