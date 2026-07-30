#include "extract/html.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace filo {
namespace {

char lowerAsciiChar(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

bool equalsIgnoreCase(const char* a, size_t aLength, const char* b) {
    const size_t bLength = std::strlen(b);
    if (aLength != bLength) return false;
    for (size_t i = 0; i < aLength; ++i) {
        if (lowerAsciiChar(a[i]) != b[i]) return false;
    }
    return true;
}

// Looks for `needle` (already lowercase) inside the first `limit` bytes,
// ignoring case.
size_t findIgnoreCase(const char* data, size_t size, const char* needle, size_t from = 0) {
    const size_t length = std::strlen(needle);
    if (length == 0 || size < length) return std::string::npos;
    for (size_t i = from; i + length <= size; ++i) {
        size_t k = 0;
        while (k < length && lowerAsciiChar(data[i + k]) == needle[k]) ++k;
        if (k == length) return i;
    }
    return std::string::npos;
}

// The encoding the document declares. The HTML spec requires the declaration
// to sit within the first 1024 bytes: past that it is too late, the browser
// has already started parsing.
UINT sniffCharset(const char* data, size_t size) {
    const size_t look = std::min<size_t>(size, 1024);

    size_t at = findIgnoreCase(data, look, "charset");
    if (at == std::string::npos) return CP_UTF8;
    at += 7;
    while (at < look && (data[at] == ' ' || data[at] == '=' || data[at] == '"' ||
                         data[at] == '\'')) {
        ++at;
    }
    size_t end = at;
    while (end < look && data[end] != '"' && data[end] != '\'' && data[end] != '>' &&
           data[end] != ' ' && data[end] != ';' && data[end] != '\r' && data[end] != '\n') {
        ++end;
    }
    const size_t length = end - at;
    const char* name = data + at;

    if (equalsIgnoreCase(name, length, "utf-8") || equalsIgnoreCase(name, length, "utf8")) {
        return CP_UTF8;
    }
    if (equalsIgnoreCase(name, length, "iso-8859-1") ||
        equalsIgnoreCase(name, length, "latin1")) {
        return 28591;
    }
    if (equalsIgnoreCase(name, length, "windows-1252") ||
        equalsIgnoreCase(name, length, "cp1252")) {
        return 1252;
    }
    if (equalsIgnoreCase(name, length, "windows-1251")) return 1251;
    if (equalsIgnoreCase(name, length, "iso-8859-15")) return 28605;
    if (equalsIgnoreCase(name, length, "utf-16") ||
        equalsIgnoreCase(name, length, "utf-16le")) {
        return 1200;
    }
    return CP_UTF8;
}

bool convertToUtf8(const char* data, size_t size, UINT codepage, std::string* out) {
    if (codepage == CP_UTF8) {
        out->assign(data, size);
        return true;
    }
    if (codepage == 1200) {   // UTF-16 LE
        const size_t count = size / sizeof(wchar_t);
        if (count == 0) return false;
        const int bytes = WideCharToMultiByte(CP_UTF8, 0,
                                              reinterpret_cast<const wchar_t*>(data),
                                              static_cast<int>(count), nullptr, 0,
                                              nullptr, nullptr);
        if (bytes <= 0) return false;
        out->resize(static_cast<size_t>(bytes));
        WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(data),
                            static_cast<int>(count), out->data(), bytes, nullptr, nullptr);
        return true;
    }

    const int count = MultiByteToWideChar(codepage, 0, data, static_cast<int>(size),
                                          nullptr, 0);
    if (count <= 0) return false;
    std::vector<wchar_t> wide(static_cast<size_t>(count));
    MultiByteToWideChar(codepage, 0, data, static_cast<int>(size), wide.data(), count);

    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(), count, nullptr, 0,
                                          nullptr, nullptr);
    if (bytes <= 0) return false;
    out->resize(static_cast<size_t>(bytes));
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), count, out->data(), bytes, nullptr,
                        nullptr);
    return true;
}

void appendCodepoint(unsigned int cp, std::string* out) {
    if (cp == 0 || cp > 0x10FFFF) return;
    if (cp == 0xA0) { out->push_back(' '); return; }   // non-breaking space
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

struct NamedEntity { const char* name; unsigned int cp; };

// The entities that actually turn up in Italian and European documents. The
// full HTML5 table has over two thousand of them, nearly all math and Greek
// symbols that are never seen in a document worth indexing.
constexpr NamedEntity kEntities[] = {
    {"amp", '&'},   {"lt", '<'},    {"gt", '>'},    {"quot", '"'},  {"apos", '\''},
    {"nbsp", 0xA0}, {"agrave", 0xE0}, {"egrave", 0xE8}, {"eacute", 0xE9},
    {"igrave", 0xEC}, {"ograve", 0xF2}, {"ugrave", 0xF9}, {"Agrave", 0xC0},
    {"Egrave", 0xC8}, {"Eacute", 0xC9}, {"Igrave", 0xCC}, {"Ograve", 0xD2},
    {"Ugrave", 0xD9}, {"agr", 0xE0}, {"aacute", 0xE1}, {"acirc", 0xE2},
    {"auml", 0xE4},  {"aring", 0xE5}, {"ccedil", 0xE7}, {"ecirc", 0xEA},
    {"euml", 0xEB},  {"iacute", 0xED}, {"icirc", 0xEE}, {"iuml", 0xEF},
    {"ntilde", 0xF1}, {"oacute", 0xF3}, {"ocirc", 0xF4}, {"otilde", 0xF5},
    {"ouml", 0xF6},  {"uacute", 0xFA}, {"ucirc", 0xFB}, {"uuml", 0xFC},
    {"szlig", 0xDF}, {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122},
    {"euro", 0x20AC}, {"pound", 0xA3}, {"deg", 0xB0}, {"middot", 0xB7},
    {"laquo", 0xAB}, {"raquo", 0xBB}, {"ldquo", 0x201C}, {"rdquo", 0x201D},
    {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ndash", 0x2013}, {"mdash", 0x2014},
    {"hellip", 0x2026}, {"bull", 0x2022}, {"dagger", 0x2020}, {"permil", 0x2030},
    {"sect", 0xA7}, {"para", 0xB6}, {"plusmn", 0xB1}, {"times", 0xD7},
    {"divide", 0xF7}, {"frac12", 0xBD}, {"frac14", 0xBC}, {"sup2", 0xB2},
    {"sup3", 0xB3}, {"ordm", 0xBA}, {"ordf", 0xAA}, {"iexcl", 0xA1},
    {"iquest", 0xBF}, {"cent", 0xA2}, {"yen", 0xA5}, {"curren", 0xA4},
};

void appendEntity(const char* data, size_t size, size_t* at, std::string* out) {
    const size_t start = *at + 1;
    size_t end = start;
    while (end < size && end < start + 32 && data[end] != ';' && data[end] != ' ' &&
           data[end] != '<' && data[end] != '&') {
        ++end;
    }
    if (end >= size || data[end] != ';' || end == start) {
        out->push_back('&');
        ++*at;
        return;
    }

    const char* name = data + start;
    const size_t length = end - start;

    if (name[0] == '#') {
        unsigned int cp = 0;
        if (length > 1 && (name[1] == 'x' || name[1] == 'X')) {
            for (size_t i = 2; i < length; ++i) {
                const char c = name[i];
                if (c >= '0' && c <= '9') cp = cp * 16 + (c - '0');
                else if (c >= 'a' && c <= 'f') cp = cp * 16 + (c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') cp = cp * 16 + (c - 'A' + 10);
                else { cp = 0; break; }
            }
        } else {
            for (size_t i = 1; i < length; ++i) {
                if (name[i] < '0' || name[i] > '9') { cp = 0; break; }
                cp = cp * 10 + static_cast<unsigned int>(name[i] - '0');
            }
        }
        appendCodepoint(cp, out);
        *at = end + 1;
        return;
    }

    for (const NamedEntity& entity : kEntities) {
        if (std::strlen(entity.name) == length &&
            std::memcmp(entity.name, name, length) == 0) {
            appendCodepoint(entity.cp, out);
            *at = end + 1;
            return;
        }
    }
    out->push_back(' ');   // unknown entity: separator, not text
    *at = end + 1;
}

bool tagNameIs(const char* data, size_t size, size_t at, const char* name) {
    size_t k = 0;
    while (name[k]) {
        if (at + k >= size || lowerAsciiChar(data[at + k]) != name[k]) return false;
        ++k;
    }
    if (at + k >= size) return true;
    const char after = data[at + k];
    return after == '>' || after == '/' || after == ' ' || after == '\t' ||
           after == '\n' || after == '\r';
}

// Elements that introduce a visual break: without a newline, the end of one
// paragraph and the start of the next weld into a word that does not exist.
bool isBlockTag(const char* data, size_t size, size_t at) {
    static const char* const kBlocks[] = {
        "p", "div", "br", "li", "tr", "td", "th", "h1", "h2", "h3", "h4", "h5", "h6",
        "section", "article", "header", "footer", "blockquote", "pre", "table",
        "ul", "ol", "dl", "dt", "dd", "hr", "form", "nav", "aside", "main", nullptr};
    for (size_t i = 0; kBlocks[i]; ++i) {
        if (tagNameIs(data, size, at, kBlocks[i])) return true;
    }
    return false;
}

}  // namespace

void stripHtmlToText(const char* data, size_t size, std::string* out) {
    out->reserve(out->size() + size / 3);

    size_t at = 0;
    while (at < size) {
        if (data[at] != '<') {
            if (data[at] == '&') {
                appendEntity(data, size, &at, out);
                continue;
            }
            out->push_back(data[at]);
            ++at;
            continue;
        }

        // Comments and declarations.
        if (at + 3 < size && data[at + 1] == '!' && data[at + 2] == '-' && data[at + 3] == '-') {
            size_t end = at + 4;
            while (end + 2 < size &&
                   !(data[end] == '-' && data[end + 1] == '-' && data[end + 2] == '>')) {
                ++end;
            }
            at = (end + 2 < size) ? end + 3 : size;
            continue;
        }

        const bool closing = at + 1 < size && data[at + 1] == '/';
        const size_t nameAt = at + (closing ? 2 : 1);

        // Elements whose CONTENT must go: code and styles, not prose.
        if (!closing) {
            static const char* const kOpaque[] = {"script", "style", "noscript",
                                                  "template", "svg", nullptr};
            bool handled = false;
            for (size_t i = 0; kOpaque[i]; ++i) {
                if (!tagNameIs(data, size, nameAt, kOpaque[i])) continue;

                // Look for the closing tag ignoring case. If there is
                // none, only the opening tag is dropped: throwing away the
                // rest of the document would be much worse.
                const std::string closeTag = std::string("</") + kOpaque[i];
                const size_t end = findIgnoreCase(data, size, closeTag.c_str(), at);
                if (end == std::string::npos) {
                    while (at < size && data[at] != '>') ++at;
                    if (at < size) ++at;
                } else {
                    at = end;
                    while (at < size && data[at] != '>') ++at;
                    if (at < size) ++at;
                }
                if (!out->empty() && out->back() != ' ' && out->back() != '\n') {
                    out->push_back(' ');
                }
                handled = true;
                break;
            }
            if (handled) continue;
        }

        const bool block = isBlockTag(data, size, nameAt);
        while (at < size && data[at] != '>') ++at;
        if (at < size) ++at;

        if (block) {
            if (!out->empty() && out->back() != '\n') out->push_back('\n');
        } else if (!out->empty() && out->back() != ' ' && out->back() != '\n') {
            out->push_back(' ');
        }
    }
}

namespace {

// --- MHTML ------------------------------------------------------------------
// A web archive is a multipart MIME message: the page, the stylesheets and the
// images all live in the same file, split by a boundary and encoded in
// quoted-printable or base64.

void decodeQuotedPrintable(const std::string& source, std::string* out) {
    out->reserve(source.size());
    for (size_t i = 0; i < source.size(); ++i) {
        if (source[i] != '=') { out->push_back(source[i]); continue; }
        if (i + 1 < source.size() && (source[i + 1] == '\r' || source[i + 1] == '\n')) {
            // "=" at end of line marks a split line: stitch it back.
            if (source[i + 1] == '\r' && i + 2 < source.size() && source[i + 2] == '\n') ++i;
            ++i;
            continue;
        }
        if (i + 2 < source.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(source[i + 1]), lo = hex(source[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out->push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out->push_back('=');
    }
}

void decodeBase64(const std::string& source, std::string* out) {
    static const int kInvalid = -1;
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return kInvalid;
    };
    out->reserve(source.size() * 3 / 4);
    int buffer = 0, bits = 0;
    for (const char c : source) {
        const int v = value(c);
        if (v == kInvalid) continue;
        buffer = (buffer << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
}

bool extractMhtml(const char* data, size_t size, std::string* out) {
    const size_t boundaryAt = findIgnoreCase(data, std::min<size_t>(size, 4096), "boundary=");
    if (boundaryAt == std::string::npos) return false;

    size_t at = boundaryAt + 9;
    if (at < size && (data[at] == '"' || data[at] == '\'')) ++at;
    size_t end = at;
    while (end < size && data[end] != '"' && data[end] != '\'' && data[end] != '\r' &&
           data[end] != '\n' && data[end] != ';' && data[end] != ' ') {
        ++end;
    }
    if (end == at) return false;
    const std::string boundary = "--" + std::string(data + at, end - at);

    size_t cursor = 0;
    bool found = false;
    while (cursor < size) {
        const size_t partStart = findIgnoreCase(data, size, boundary.c_str(), cursor);
        if (partStart == std::string::npos) break;
        size_t headerStart = partStart + boundary.size();

        const size_t nextPart = findIgnoreCase(data, size, boundary.c_str(), headerStart);
        const size_t partEnd = (nextPart == std::string::npos) ? size : nextPart;
        cursor = partEnd;

        // End of the part's headers: the first empty line.
        size_t bodyStart = headerStart;
        while (bodyStart + 3 < partEnd) {
            if (data[bodyStart] == '\r' && data[bodyStart + 1] == '\n' &&
                data[bodyStart + 2] == '\r' && data[bodyStart + 3] == '\n') {
                bodyStart += 4;
                break;
            }
            ++bodyStart;
        }
        if (bodyStart >= partEnd) continue;

        const size_t headerLength = bodyStart - headerStart;
        const bool isHtml =
            findIgnoreCase(data + headerStart, headerLength, "text/html") != std::string::npos;
        if (!isHtml) continue;

        const bool base64 =
            findIgnoreCase(data + headerStart, headerLength, "base64") != std::string::npos;
        const bool quoted = findIgnoreCase(data + headerStart, headerLength,
                                           "quoted-printable") != std::string::npos;

        std::string body(data + bodyStart, partEnd - bodyStart);
        std::string decoded;
        if (base64) decodeBase64(body, &decoded);
        else if (quoted) decodeQuotedPrintable(body, &decoded);
        else decoded = std::move(body);

        std::string utf8;
        const UINT codepage = sniffCharset(decoded.data(), decoded.size());
        if (!convertToUtf8(decoded.data(), decoded.size(), codepage, &utf8)) continue;

        stripHtmlToText(utf8.data(), utf8.size(), out);
        out->push_back('\n');
        found = true;
    }
    return found;
}

}  // namespace

bool extractHtml(const char* data, size_t size, std::string* out) {
    out->clear();
    if (size == 0) return false;

    // A web archive is told apart by the MIME headers at the top.
    const size_t look = std::min<size_t>(size, 2048);
    if (findIgnoreCase(data, look, "mime-version:") != std::string::npos &&
        findIgnoreCase(data, look, "multipart/") != std::string::npos) {
        if (extractMhtml(data, size, out)) return !out->empty();
    }

    std::string utf8;
    const UINT codepage = sniffCharset(data, size);
    if (!convertToUtf8(data, size, codepage, &utf8)) return false;

    stripHtmlToText(utf8.data(), utf8.size(), out);
    return !out->empty();
}

}  // namespace filo
