#include "extract/rtf.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "extract/html.h"

namespace filo {
namespace {

constexpr size_t kMaxOutputBytes = 8u << 20;
constexpr size_t kMaxDepth = 256;

// Destinations whose content is NOT text for the user: font and color tables,
// style definitions, images, embedded objects, revision metadata. They are
// most of the file in an RTF written by Word.
bool isSkippedDestination(const char* word, size_t length) {
    static const char* const kSkip[] = {
        "fonttbl", "colortbl", "stylesheet", "listtable", "listoverridetable",
        "rsidtbl", "generator", "filetbl", "revtbl", "themedata", "datastore",
        "latentstyles", "info", "pict", "object", "objdata", "xmlnstbl",
        "mmathPr", "wgrffmtfilter", "pnseclvl", "colorschememapping",
        "template", "operator", "company", "userprops", "svb", "bkmkstart",
        "bkmkend", "field", "falt", "panose", "atnid", "atnauthor", nullptr};
    for (size_t i = 0; kSkip[i]; ++i) {
        const size_t candidate = std::strlen(kSkip[i]);
        if (candidate == length && std::memcmp(kSkip[i], word, length) == 0) return true;
    }
    return false;
}

void appendCodepoint(unsigned int cp, std::string* out) {
    if (cp == 0 || cp > 0x10FFFF) return;
    if (cp < 0x80) {
        out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

struct GroupState {
    bool skip = false;
    int unicodeSkip = 1;   // how many fallback chars follow each \uN
};

}  // namespace

bool extractRtf(const char* data, size_t size, std::string* out) {
    out->clear();
    if (size < 5 || std::memcmp(data, "{\\rtf", 5) != 0) return false;

    std::vector<GroupState> stack;
    stack.reserve(32);
    stack.push_back(GroupState{});

    UINT codepage = 1252;          // until \ansicpg says otherwise
    bool encapsulatedHtml = false; // \fromhtml1: HTML inside, not prose
    int pendingSkip = 0;           // fallback chars to swallow after \uN
    std::string raw;
    raw.reserve(size / 4);

    // The "raw" bytes in the current codepage pile up here and are converted
    // in one block: converting them one at a time would break the multi-byte
    // sequences of the Asian codepages.
    std::string pending;
    auto flushPending = [&]() {
        if (pending.empty()) return;
        const int count = MultiByteToWideChar(codepage, 0, pending.data(),
                                              static_cast<int>(pending.size()), nullptr, 0);
        if (count > 0) {
            std::vector<wchar_t> wide(static_cast<size_t>(count));
            MultiByteToWideChar(codepage, 0, pending.data(),
                                static_cast<int>(pending.size()), wide.data(), count);
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(), count, nullptr,
                                                  0, nullptr, nullptr);
            if (bytes > 0) {
                const size_t at = raw.size();
                raw.resize(at + static_cast<size_t>(bytes));
                WideCharToMultiByte(CP_UTF8, 0, wide.data(), count, raw.data() + at, bytes,
                                    nullptr, nullptr);
            }
        }
        pending.clear();
    };

    size_t at = 0;
    while (at < size && raw.size() + pending.size() < kMaxOutputBytes) {
        const char c = data[at];

        if (c == '{') {
            if (stack.size() >= kMaxDepth) { ++at; continue; }
            stack.push_back(stack.back());
            ++at;
            continue;
        }
        if (c == '}') {
            flushPending();
            if (stack.size() > 1) stack.pop_back();
            ++at;
            continue;
        }
        if (c == '\r' || c == '\n') { ++at; continue; }   // ignored: only \par counts

        if (c != '\\') {
            if (!stack.back().skip) {
                if (pendingSkip > 0) --pendingSkip;
                else pending.push_back(c);
            }
            ++at;
            continue;
        }

        // --- control sequence ---
        ++at;
        if (at >= size) break;

        // Optional destination: if we do not know it, skip the whole thing.
        // Treating them all as skippable loses the tags of the encapsulated
        // HTML but KEEPS the visible text, which is what we are after.
        if (data[at] == '*') {
            stack.back().skip = true;
            ++at;
            continue;
        }

        // Escaped characters: \\ \{ \} are literals.
        if (data[at] == '\\' || data[at] == '{' || data[at] == '}') {
            if (!stack.back().skip) {
                if (pendingSkip > 0) --pendingSkip;
                else pending.push_back(data[at]);
            }
            ++at;
            continue;
        }

        // Control symbols: a backslash followed by a character that is not a
        // letter. They are special spaces and dashes. Left untranslated they
        // would land in the text as tildes and underscores in the middle of
        // words, making up terms nobody will ever search for.
        if (data[at] == '~' || data[at] == '-' || data[at] == '_') {
            if (!stack.back().skip) {
                if (pendingSkip > 0) {
                    --pendingSkip;
                } else if (data[at] == '~') {
                    pending.push_back(' ');   // non-breaking space
                } else if (data[at] == '_') {
                    pending.push_back('-');   // non-breaking hyphen
                }
                // '-' is the optional hyphenation break: not written out
            }
            ++at;
            continue;
        }

        // Hex byte in the current codepage.
        if (data[at] == '\'' && at + 2 < size) {
            auto hex = [](char h) -> int {
                if (h >= '0' && h <= '9') return h - '0';
                if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                return -1;
            };
            const int hi = hex(data[at + 1]), lo = hex(data[at + 2]);
            if (hi >= 0 && lo >= 0 && !stack.back().skip) {
                if (pendingSkip > 0) --pendingSkip;
                else pending.push_back(static_cast<char>(hi * 16 + lo));
            }
            at += 3;
            continue;
        }

        // Control word: letters, then a number with an optional sign.
        const size_t wordStart = at;
        while (at < size && ((data[at] >= 'a' && data[at] <= 'z') ||
                             (data[at] >= 'A' && data[at] <= 'Z'))) {
            ++at;
        }
        const size_t wordLength = at - wordStart;
        const char* word = data + wordStart;

        bool hasNumber = false;
        long long number = 0;
        int sign = 1;
        if (at < size && data[at] == '-') { sign = -1; ++at; }
        while (at < size && data[at] >= '0' && data[at] <= '9') {
            hasNumber = true;
            if (number < 100000000) number = number * 10 + (data[at] - '0');
            ++at;
        }
        number *= sign;
        // A space right after is the word delimiter, not text.
        if (at < size && data[at] == ' ') ++at;

        if (wordLength == 0) continue;

        auto is = [&](const char* name) {
            return std::strlen(name) == wordLength &&
                   std::memcmp(name, word, wordLength) == 0;
        };

        if (is("ansicpg") && hasNumber) {
            flushPending();
            codepage = static_cast<UINT>(number);
            continue;
        }
        if (is("fromhtml")) { encapsulatedHtml = true; continue; }
        if (is("uc") && hasNumber) {
            stack.back().unicodeSkip = static_cast<int>(std::max<long long>(0, number));
            continue;
        }
        if (is("u") && hasNumber) {
            if (!stack.back().skip) {
                flushPending();
                // The value is a signed 16-bit integer: codepoints above
                // 32767 arrive as negative numbers.
                unsigned int cp = number < 0 ? static_cast<unsigned int>(number + 65536)
                                             : static_cast<unsigned int>(number);
                appendCodepoint(cp, &raw);
            }
            pendingSkip = stack.back().unicodeSkip;
            continue;
        }
        if (is("par") || is("line") || is("sect") || is("page") || is("row")) {
            if (!stack.back().skip) { flushPending(); raw.push_back('\n'); }
            continue;
        }
        if (is("tab") || is("cell")) {
            if (!stack.back().skip) { flushPending(); raw.push_back(' '); }
            continue;
        }
        if (isSkippedDestination(word, wordLength)) {
            stack.back().skip = true;
            continue;
        }
        // Every other control word is formatting: ignored, but it cancels any
        // fallback count in progress.
        pendingSkip = 0;
    }

    flushPending();
    if (raw.empty()) return false;

    // Mail attachments wrap HTML inside RTF: miss it and the index gets the
    // tags instead of the text.
    if (encapsulatedHtml) {
        std::string text;
        stripHtmlToText(raw.data(), raw.size(), &text);
        out->swap(text);
    } else {
        out->swap(raw);
    }
    return !out->empty();
}

}  // namespace filo
