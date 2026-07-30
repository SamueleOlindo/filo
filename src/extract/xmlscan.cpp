#include "extract/xmlscan.h"

#include <cstring>

namespace filo {
namespace {

bool matchesAny(const char* const* list, const char* name, size_t nameLength) {
    if (!list) return false;
    for (size_t i = 0; list[i]; ++i) {
        const size_t length = std::strlen(list[i]);
        if (length == nameLength && std::memcmp(list[i], name, length) == 0) return true;
    }
    return false;
}

// Only XML's predefined entities plus the numeric references. CUSTOM entities
// are deliberately not expanded: that is the choice that makes us immune to
// the exponential recursion of "billion laughs".
void appendEntity(const char* data, size_t size, size_t* at, std::string* out) {
    const size_t start = *at + 1;
    size_t end = start;
    while (end < size && end < start + 12 && data[end] != ';') ++end;
    if (end >= size || data[end] != ';') {
        out->push_back('&');
        ++*at;
        return;
    }

    const size_t length = end - start;
    const char* name = data + start;

    if (length == 3 && std::memcmp(name, "amp", 3) == 0)        out->push_back('&');
    else if (length == 2 && std::memcmp(name, "lt", 2) == 0)    out->push_back('<');
    else if (length == 2 && std::memcmp(name, "gt", 2) == 0)    out->push_back('>');
    else if (length == 4 && std::memcmp(name, "quot", 4) == 0)  out->push_back('"');
    else if (length == 4 && std::memcmp(name, "apos", 4) == 0)  out->push_back('\'');
    else if (length >= 2 && name[0] == '#') {
        unsigned int cp = 0;
        if (name[1] == 'x' || name[1] == 'X') {
            for (size_t i = 2; i < length; ++i) {
                const char c = name[i];
                cp = cp * 16 + (c >= '0' && c <= '9'   ? c - '0'
                                : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                       : 0);
            }
        } else {
            for (size_t i = 1; i < length; ++i) {
                if (name[i] < '0' || name[i] > '9') { cp = 0; break; }
                cp = cp * 10 + static_cast<unsigned int>(name[i] - '0');
            }
        }
        if (cp == 0 || cp > 0x10FFFF) {
            out->push_back(' ');
        } else if (cp < 0x80) {
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
    } else {
        out->push_back(' ');   // unknown entity: separator, not text
    }
    *at = end + 1;
}

}  // namespace

void scanXmlText(const char* data, size_t size, const XmlTextRules& rules,
                 std::string* out) {
    out->reserve(out->size() + size / 8);

    int textDepth = 0;   // how many "keep" elements deep we are
    int skipDepth = 0;   // how many "drop" elements deep we are

    size_t at = 0;
    while (at < size) {
        if (data[at] != '<') {
            if (textDepth > 0 && skipDepth == 0) {
                if (data[at] == '&') {
                    appendEntity(data, size, &at, out);
                    continue;
                }
                out->push_back(data[at]);
            }
            ++at;
            continue;
        }

        // Comments, CDATA sections and processing instructions.
        if (at + 3 < size && data[at + 1] == '!' && data[at + 2] == '-' && data[at + 3] == '-') {
            const char* end = static_cast<const char*>(
                std::memchr(data + at, '>', size - at));
            while (end && end + 1 <= data + size &&
                   !(end - data >= 2 && end[-1] == '-' && end[-2] == '-')) {
                const size_t next = static_cast<size_t>(end - data) + 1;
                if (next >= size) { end = nullptr; break; }
                end = static_cast<const char*>(std::memchr(data + next, '>', size - next));
            }
            at = end ? static_cast<size_t>(end - data) + 1 : size;
            continue;
        }
        if (at + 8 < size && std::memcmp(data + at, "<![CDATA[", 9) == 0) {
            const size_t start = at + 9;
            size_t end = start;
            while (end + 2 < size && !(data[end] == ']' && data[end + 1] == ']' &&
                                       data[end + 2] == '>')) {
                ++end;
            }
            if (textDepth > 0 && skipDepth == 0) out->append(data + start, end - start);
            at = (end + 2 < size) ? end + 3 : size;
            continue;
        }
        if (at + 1 < size && (data[at + 1] == '?' || data[at + 1] == '!')) {
            const char* end = static_cast<const char*>(std::memchr(data + at, '>', size - at));
            at = end ? static_cast<size_t>(end - data) + 1 : size;
            continue;
        }

        // An actual tag.
        const bool closing = at + 1 < size && data[at + 1] == '/';
        const size_t nameStart = at + (closing ? 2 : 1);
        size_t nameEnd = nameStart;
        while (nameEnd < size && data[nameEnd] != '>' && data[nameEnd] != ' ' &&
               data[nameEnd] != '\t' && data[nameEnd] != '\n' && data[nameEnd] != '\r' &&
               data[nameEnd] != '/') {
            ++nameEnd;
        }

        const char* tagEnd = static_cast<const char*>(std::memchr(data + at, '>', size - at));
        const size_t after = tagEnd ? static_cast<size_t>(tagEnd - data) + 1 : size;
        const bool selfClosing = tagEnd && tagEnd > data + at && tagEnd[-1] == '/';

        const char* name = data + nameStart;
        const size_t nameLength = nameEnd - nameStart;

        if (matchesAny(rules.breakTags, name, nameLength) && (closing || selfClosing)) {
            // Without this separator the last word of one paragraph and the
            // first of the next weld into a word that does not exist.
            if (!out->empty() && out->back() != '\n') out->push_back('\n');
        }

        if (!selfClosing) {
            if (matchesAny(rules.skipTags, name, nameLength)) {
                skipDepth += closing ? -1 : 1;
                if (skipDepth < 0) skipDepth = 0;
            } else if (matchesAny(rules.textTags, name, nameLength)) {
                textDepth += closing ? -1 : 1;
                if (textDepth < 0) textDepth = 0;
            }
        }

        at = after;
    }
}

}  // namespace filo
