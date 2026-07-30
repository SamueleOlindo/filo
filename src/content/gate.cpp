#include "content/gate.h"

#include <windows.h>

#include <cstring>

namespace filo {
namespace {

// Compares the leading bytes against a signature.
bool startsWith(const char* data, size_t size, const unsigned char* signature,
                size_t signatureLength) {
    if (size < signatureLength) return false;
    return std::memcmp(data, signature, signatureLength) == 0;
}

constexpr unsigned char kPdf[]  = {'%', 'P', 'D', 'F', '-'};
constexpr unsigned char kZip[]  = {'P', 'K', 0x03, 0x04};
constexpr unsigned char kZipEmpty[] = {'P', 'K', 0x05, 0x06};
constexpr unsigned char kCfb[]  = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
constexpr unsigned char kRtf[]  = {'{', '\\', 'r', 't', 'f'};

// Signatures of things that are NOT text. A file starting with these bytes is
// stopped whatever extension it carries: it is almost always a renamed file or
// a format we cannot read.
struct BinarySignature {
    const unsigned char* bytes;
    size_t length;
};

constexpr unsigned char kPng[]   = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
constexpr unsigned char kJpeg[]  = {0xFF, 0xD8, 0xFF};
constexpr unsigned char kGif[]   = {'G', 'I', 'F', '8'};
constexpr unsigned char kBmp[]   = {'B', 'M'};
constexpr unsigned char kMz[]    = {'M', 'Z'};
constexpr unsigned char kGzip[]  = {0x1F, 0x8B};
constexpr unsigned char k7z[]    = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};
constexpr unsigned char kRar[]   = {'R', 'a', 'r', '!'};
constexpr unsigned char kElf[]   = {0x7F, 'E', 'L', 'F'};
constexpr unsigned char kSqlite[]= {'S', 'Q', 'L', 'i', 't', 'e', ' ', 'f'};
constexpr unsigned char kOgg[]   = {'O', 'g', 'g', 'S'};
constexpr unsigned char kRiff[]  = {'R', 'I', 'F', 'F'};
constexpr unsigned char kIcc[]   = {0x00, 0x01, 0x00, 0x00};  // ttf/otf

constexpr BinarySignature kBinarySignatures[] = {
    {kPng, sizeof(kPng)},   {kJpeg, sizeof(kJpeg)}, {kGif, sizeof(kGif)},
    {kBmp, sizeof(kBmp)},   {kMz, sizeof(kMz)},     {kGzip, sizeof(kGzip)},
    {k7z, sizeof(k7z)},     {kRar, sizeof(kRar)},   {kElf, sizeof(kElf)},
    {kSqlite, sizeof(kSqlite)}, {kOgg, sizeof(kOgg)}, {kRiff, sizeof(kRiff)},
    {kIcc, sizeof(kIcc)},
};

// Attributes marking a file that is not materialized on disk.
// RECALL_ON_DATA_ACCESS is the one OneDrive uses for "files on demand".
constexpr uint32_t kCloudAttributes =
    FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN |
    FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;

// How many bytes to look at to decide whether a file is text. More is
// pointless: if the first 8 KB are clean text, the rest almost certainly is.
constexpr size_t kSniffBytes = 8192;

}  // namespace

bool isCloudPlaceholder(uint32_t attributes) {
    return (attributes & kCloudAttributes) != 0;
}

bool readFileFacts(const wchar_t* path, FileFacts* facts) {
    // GetFileAttributesEx queries the directory entry's metadata: it does not
    // open the file, so it does not trigger a recall from the cloud.
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data)) return false;

    facts->attributes = data.dwFileAttributes;
    facts->size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    facts->mtime = (static_cast<int64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                   data.ftLastWriteTime.dwLowDateTime;
    return true;
}

uint64_t sizeLimitFor(ContentKind kind) {
    switch (kind) {
        // A source file or a log over 8 MB was written by a machine: indexing
        // it costs a lot and helps nobody.
        case ContentKind::PlainText:
        case ContentKind::Code:
        case ContentKind::Markup:
            return 8ull << 20;
        // A large PDF or DOCX, on the other hand, is plausibly a real manual.
        default:
            return 64ull << 20;
    }
}

ContentKind detectByMagic(const char* data, size_t size, ContentKind fromExtension) {
    if (size < 4) return size == 0 ? ContentKind::Skip : fromExtension;

    if (startsWith(data, size, kPdf, sizeof(kPdf))) return ContentKind::Pdf;
    if (startsWith(data, size, kRtf, sizeof(kRtf))) return ContentKind::RichText;

    // A CFB container with a modern extension almost always means a
    // password-protected OOXML: the ZIP is inside an encrypted stream and we
    // will never read it. Recognizing it here saves wasting a parser on it.
    if (startsWith(data, size, kCfb, sizeof(kCfb))) return ContentKind::OfficeLegacy;

    if (startsWith(data, size, kZip, sizeof(kZip)) ||
        startsWith(data, size, kZipEmpty, sizeof(kZipEmpty))) {
        // Every container format is a ZIP: here the extension is what tells
        // them apart, because the leading bytes are identical.
        switch (fromExtension) {
            case ContentKind::OfficeXml:
            case ContentKind::OpenDocument:
            case ContentKind::Ebook:
                return fromExtension;
            default:
                return ContentKind::Skip;  // just some zip: not our business
        }
    }

    for (const BinarySignature& signature : kBinarySignatures) {
        if (startsWith(data, size, signature.bytes, signature.length)) {
            return ContentKind::Skip;
        }
    }

    return fromExtension;
}

TextEncoding detectEncoding(const char* data, size_t size) {
    if (size == 0) return TextEncoding::Unknown;

    const auto* bytes = reinterpret_cast<const unsigned char*>(data);

    // 1. The explicit signature, when present, settles it.
    if (size >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        return TextEncoding::Utf8;
    }
    if (size >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) return TextEncoding::Utf16LE;
    if (size >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) return TextEncoding::Utf16BE;

    const size_t look = size < kSniffBytes ? size : kSniffBytes;

    // 2. UTF-16 without a signature: recognized by the null bytes in alternate
    //    positions. Latin text in UTF-16 is made of pairs like 'a',0x00.
    size_t zeroEven = 0, zeroOdd = 0;
    for (size_t i = 0; i < look; ++i) {
        if (bytes[i] == 0) {
            if (i & 1) ++zeroOdd; else ++zeroEven;
        }
    }
    const size_t zeros = zeroEven + zeroOdd;
    if (zeros > look / 8) {
        if (zeroOdd > zeroEven * 8) return TextEncoding::Utf16LE;
        if (zeroEven > zeroOdd * 8) return TextEncoding::Utf16BE;
        return TextEncoding::Binary;  // many scattered zeros: not text
    }
    if (zeros > 0) return TextEncoding::Binary;  // a single NUL rules out text

    // 3. Control characters. Tab, newline and form feed are legitimate; the
    //    rest below 0x20 are not. If they abound, the file is binary.
    size_t control = 0, high = 0;
    for (size_t i = 0; i < look; ++i) {
        const unsigned char c = bytes[i];
        if (c < 0x20 && c != '\t' && c != '\n' && c != '\r' && c != '\f') ++control;
        if (c >= 0x80) ++high;
    }
    if (control * 100 > look * 2) return TextEncoding::Binary;  // over 2%

    if (high == 0) return TextEncoding::Ascii;

    // 4. Real UTF-8 validation. A latin-1 file with accents passes the checks
    //    above but fails here, and gets handed to the statistical detector.
    switch (checkUtf8(data, look)) {
        case Utf8Check::Valid:
            return TextEncoding::Utf8;
        case Utf8Check::Invalid:
            return TextEncoding::Legacy;
        case Utf8Check::Truncated:
            // If the inspection window cut the file short, the incomplete
            // sequence is our own artefact and says nothing. If the file really
            // ends there, the sequence is genuinely broken: almost always a
            // latin-1 text ending on an accented letter.
            return look < size ? TextEncoding::Utf8 : TextEncoding::Legacy;
    }
    return TextEncoding::Utf8;
}

Utf8Check checkUtf8(const char* data, size_t size) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(data);
    size_t i = 0;
    while (i < size) {
        const unsigned char c = bytes[i];
        size_t extra = 0;
        unsigned int codepoint = 0;

        if (c < 0x80) { ++i; continue; }
        else if ((c & 0xE0) == 0xC0) { extra = 1; codepoint = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; codepoint = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; codepoint = c & 0x07; }
        else return Utf8Check::Invalid;   // stray continuation byte

        if (i + extra >= size) return Utf8Check::Truncated;

        for (size_t k = 1; k <= extra; ++k) {
            if ((bytes[i + k] & 0xC0) != 0x80) return Utf8Check::Invalid;
            codepoint = (codepoint << 6) | (bytes[i + k] & 0x3F);
        }
        // Overlong encodings and surrogates: formally UTF-8 but not valid, and
        // the classic way of slipping past a filter.
        if ((extra == 1 && codepoint < 0x80) ||
            (extra == 2 && codepoint < 0x800) ||
            (extra == 3 && codepoint < 0x10000) ||
            (codepoint >= 0xD800 && codepoint <= 0xDFFF) ||
            codepoint > 0x10FFFF) {
            return Utf8Check::Invalid;
        }
        i += extra + 1;
    }
    return Utf8Check::Valid;
}

bool isValidUtf8(const char* data, size_t size) {
    return checkUtf8(data, size) == Utf8Check::Valid;
}

size_t bomLength(TextEncoding encoding, const char* data, size_t size) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(data);
    if (encoding == TextEncoding::Utf8 && size >= 3 && bytes[0] == 0xEF &&
        bytes[1] == 0xBB && bytes[2] == 0xBF) {
        return 3;
    }
    if ((encoding == TextEncoding::Utf16LE || encoding == TextEncoding::Utf16BE) &&
        size >= 2 && ((bytes[0] == 0xFF && bytes[1] == 0xFE) ||
                      (bytes[0] == 0xFE && bytes[1] == 0xFF))) {
        return 2;
    }
    return 0;
}

const wchar_t* encodingName(TextEncoding encoding) {
    switch (encoding) {
        case TextEncoding::Unknown:  return L"unknown";
        case TextEncoding::Ascii:    return L"ascii";
        case TextEncoding::Utf8:     return L"utf-8";
        case TextEncoding::Utf16LE:  return L"utf-16 le";
        case TextEncoding::Utf16BE:  return L"utf-16 be";
        case TextEncoding::Legacy:   return L"legacy (to be detected)";
        case TextEncoding::Binary:   return L"binary";
    }
    return L"?";
}

const wchar_t* verdictName(GateVerdict verdict) {
    switch (verdict) {
        case GateVerdict::Pass:             return L"passed";
        case GateVerdict::CloudPlaceholder: return L"cloud placeholder (not downloaded)";
        case GateVerdict::TooBig:           return L"over the size limit";
        case GateVerdict::Empty:            return L"empty";
        case GateVerdict::Unreadable:       return L"unreadable";
        case GateVerdict::BinaryContent:    return L"binary content";
    }
    return L"?";
}

}  // namespace filo
