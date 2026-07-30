#include "store/snippet.h"

#include <windows.h>

#include <algorithm>
#include <vector>

#include "content/gate.h"
#include "content/normalize.h"
#include "content/selector.h"

namespace filo {
namespace {

// Past this size no preview is built: a huge file would freeze the interface
// for the sake of a cosmetic detail.
constexpr uint64_t kMaxSnippetSourceBytes = 16ull << 20;

bool readWholeFile(const std::wstring& path, std::vector<char>* out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_NO_RECALL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) ||
        static_cast<uint64_t>(size.QuadPart) > kMaxSnippetSourceBytes) {
        CloseHandle(file);
        return false;
    }

    out->resize(static_cast<size_t>(size.QuadPart));
    size_t got = 0;
    while (got < out->size()) {
        DWORD read = 0;
        const DWORD chunk =
            static_cast<DWORD>(std::min<size_t>(out->size() - got, 1u << 24));
        if (!ReadFile(file, out->data() + got, chunk, &read, nullptr) || read == 0) break;
        got += read;
    }
    CloseHandle(file);
    out->resize(got);
    return got > 0;
}

char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

// Case-insensitive comparison that covers accented letters too.
//
// In UTF-8 the accented Latin letters are pairs starting with 0xC3, and the
// upper case ones sit 0x20 below the lower case ones exactly as in ASCII.
// Without this, searching for "Citta'" with the accent in a document that
// writes "citta'" does not centre the preview on the match, and the result
// looks wrong while being right.
bool equalFolded(const std::string& a, size_t ai, const std::string& b, size_t bi,
                 size_t& advance) {
    const auto ca = static_cast<unsigned char>(a[ai]);
    const auto cb = static_cast<unsigned char>(b[bi]);

    if (ca == 0xC3 && cb == 0xC3 && ai + 1 < a.size() && bi + 1 < b.size()) {
        auto na = static_cast<unsigned char>(a[ai + 1]);
        auto nb = static_cast<unsigned char>(b[bi + 1]);
        if (na >= 0x80 && na <= 0x9E) na += 0x20;   // upper case -> lower case
        if (nb >= 0x80 && nb <= 0x9E) nb += 0x20;
        advance = 2;
        return na == nb;
    }
    advance = 1;
    return lowerAscii(a[ai]) == lowerAscii(b[bi]);
}

// `until` limits the scan to the chunk's region. Without it, when the match
// happened by STEM (the user types "fatture", the index holds "fattura") no
// term is found literally, and we used to walk the whole text — up to 16 MB —
// for every term and for each of the twelve previews. It is the most common
// case, and it is exactly the latency the user perceives as "the search has
// hung".
size_t findCaseInsensitive(const std::string& haystack, const std::string& needle,
                           size_t from, size_t until) {
    if (needle.empty() || needle.size() > haystack.size()) return std::string::npos;
    const size_t last = std::min(until, haystack.size());

    for (size_t i = from; i + needle.size() <= last; ++i) {
        size_t hi = i, ni = 0;
        bool equal = true;
        while (ni < needle.size() && hi < last) {
            size_t advance = 1;
            if (!equalFolded(haystack, hi, needle, ni, advance)) { equal = false; break; }
            hi += advance;
            ni += advance;
        }
        if (equal && ni >= needle.size()) return i;
    }
    return std::string::npos;
}

bool isContinuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

// Moves a bound onto a UTF-8 character boundary: cutting a sequence in half
// would produce invalid bytes that JavaScript cannot decode.
size_t alignForward(const std::string& text, size_t at) {
    while (at < text.size() && isContinuation(text[at])) ++at;
    return at;
}
size_t alignBackward(const std::string& text, size_t at) {
    while (at > 0 && at < text.size() && isContinuation(text[at])) --at;
    return at;
}

}  // namespace

std::vector<std::string> splitTerms(const std::string& userQuery) {
    std::vector<std::string> terms;
    std::string current;
    for (const char c : userQuery) {
        if (c == ' ' || c == '\t' || c == '"' || c == '\n') {
            if (current.size() >= 2) terms.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (current.size() >= 2) terms.push_back(current);
    return terms;
}

std::string buildSnippet(const std::wstring& path, const std::string& storedText,
                         uint32_t chunkOffset, uint32_t chunkLength,
                         const std::vector<std::string>& terms, size_t maxChars) {
    std::string text;

    if (!storedText.empty()) {
        // Already extracted at indexing time and kept in the database. Nothing
        // to read, nothing to parse, and it works even if the file has since
        // been moved, renamed, or left on a drive that is not plugged in.
        text = storedText;
    } else {
        std::vector<char> raw;
        if (!readWholeFile(path, &raw)) return {};

        // The name decides how to treat the content: markup gets its tags
        // stripped exactly as at indexing time, otherwise the chunk offsets
        // would not line up.
        const size_t slash = path.find_last_of(L'\\');
        const std::wstring fileName =
            slash == std::wstring::npos ? path : path.substr(slash + 1);
        const ContentKind kind = classifyByName(fileName);

        // Formats that need an extractor cannot be cropped from here: reading
        // them as plain text produces the file's internals — object
        // references, page boxes, font names — which in a preview is worse
        // than nothing. Their text should have arrived in `storedText`; if it
        // did not, show no preview rather than a false one.
        const bool plainReadable = kind == ContentKind::PlainText ||
                                   kind == ContentKind::Code ||
                                   kind == ContentKind::Markup;
        if (!plainReadable) return {};

        const TextEncoding encoding = detectEncoding(raw.data(), raw.size());
        if (!normalizeToUtf8(raw.data(), raw.size(), encoding, kind, &text)) return {};
    }

    // The chunk we were pointed at, if it is still where it was. If the file
    // changed after indexing the offsets no longer hold: better the start of
    // the file than a crop taken at random.
    size_t regionStart = 0;
    size_t regionEnd = text.size();
    if (chunkOffset < text.size()) {
        regionStart = chunkOffset;
        regionEnd = std::min<size_t>(text.size(),
                                     static_cast<size_t>(chunkOffset) + chunkLength);
    }

    // The first query term inside the chunk: that is where the preview has to
    // be centred, not at the start of the chunk.
    size_t hit = std::string::npos;
    for (const std::string& term : terms) {
        const size_t at = findCaseInsensitive(text, term, regionStart, regionEnd);
        if (at != std::string::npos && (hit == std::string::npos || at < hit)) hit = at;
    }
    if (hit == std::string::npos) hit = regionStart;

    const size_t half = maxChars / 2;
    size_t start = hit > regionStart + half ? hit - half : regionStart;
    size_t end = std::min<size_t>(regionEnd, start + maxChars);
    start = alignBackward(text, start);
    end = alignForward(text, end);

    // Do not cut a word in half at the start: it makes the preview look
    // broken. The search for the space is capped at 40 bytes by hand:
    // find_first_of would run to the end of the file on a JSON with no
    // spaces, only to throw the result away for being too far off.
    if (start > regionStart) {
        const size_t scanEnd = std::min(end, start + 40);
        for (size_t i = start; i < scanEnd; ++i) {
            if (text[i] == ' ' || text[i] == '\n') { start = i + 1; break; }
        }
    }

    std::string snippet;
    if (start > regionStart) snippet += "\xE2\x80\xA6 ";  // ellipsis
    for (size_t i = start; i < end && i < text.size(); ++i) {
        snippet.push_back(text[i] == '\n' ? ' ' : text[i]);
    }
    if (end < regionEnd) snippet += " \xE2\x80\xA6";
    return snippet;
}

}  // namespace filo
