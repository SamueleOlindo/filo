#pragma once

#include <cstddef>
#include <cstdint>

#include "content/selector.h"

namespace filo {

// How a file's text is encoded.
//
// Getting this wrong makes the content unusable silently: a UTF-16 file read as
// UTF-8 produces garbage that still lands in the index, and the user will never
// find that document without ever learning why.
enum class TextEncoding : uint8_t {
    Unknown = 0,
    Ascii,      // bytes < 128 only: valid as UTF-8 and as any code page
    Utf8,       // valid multi-byte (with or without BOM)
    Utf16LE,
    Utf16BE,
    Legacy,     // not Unicode: needs a statistical detector (next layer)
    Binary      // not text at all
};

// Why a file was stopped before extraction.
enum class GateVerdict : uint8_t {
    Pass = 0,
    CloudPlaceholder,  // OneDrive "online only" file: opening it would download it
    TooBig,
    Empty,
    Unreadable,
    BinaryContent,     // text extension, binary content
};

struct FileFacts {
    uint64_t size = 0;
    int64_t  mtime = 0;      // FILETIME as a 64-bit integer
    uint32_t attributes = 0;
};

// --- gate 0: attributes, before opening -------------------------------------
//
// OneDrive "online only" files exist as placeholders: reading their content
// triggers a DOWNLOAD. On an account with tens of gigabytes that would mean
// saturating the user's connection in order to index.
bool isCloudPlaceholder(uint32_t attributes);

// Reads size, date and attributes without opening the file (does not hydrate
// cloud placeholders).
bool readFileFacts(const wchar_t* path, FileFacts* facts);

// Size ceiling per category. A 40 MB .cpp is generated, not written; a 40 MB
// PDF is a legitimate manual.
uint64_t sizeLimitFor(ContentKind kind);

// --- gate 1: the REAL format ------------------------------------------------
//
// The extension is a hint, not a fact: a password-protected .docx is not a ZIP
// but a CFB container, and a .txt can be a renamed executable. The leading
// bytes always beat the name.
ContentKind detectByMagic(const char* data, size_t size, ContentKind fromExtension);

// --- gate 2: text or binary, and in which encoding --------------------------
TextEncoding detectEncoding(const char* data, size_t size);

// Result of validating UTF-8 over a range.
enum class Utf8Check : uint8_t {
    Valid,
    Invalid,
    Truncated,  // valid all the way, but the last sequence is cut off
};

Utf8Check checkUtf8(const char* data, size_t size);

// Valid across the WHOLE range, no shortcuts.
//
// detectEncoding looks at the first 8 KB only, because it has to be fast across
// hundreds of thousands of files. But its verdict is then applied to the entire
// file, and a file with an ASCII header and latin-1 accents further down — the
// classic log with an English header and an Italian body — would be copied raw,
// with the accented words in its tail corrupted in the index. So before
// decoding, everything really does get checked.
bool isValidUtf8(const char* data, size_t size);

// How many bytes to skip for the encoding signature (BOM), if present.
size_t bomLength(TextEncoding encoding, const char* data, size_t size);

const wchar_t* encodingName(TextEncoding encoding);
const wchar_t* verdictName(GateVerdict verdict);

}  // namespace filo
