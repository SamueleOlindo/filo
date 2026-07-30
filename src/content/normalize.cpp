#include "content/normalize.h"

#include <windows.h>

#include <vector>

namespace filo {
namespace {

// Code page 1252 as the fallback for text that is not Unicode.
//
// Not a wild guess: 1252 is the superset of latin-1 that Windows used for
// decades in western Europe, and it is what an Italian Notepad produces. A
// real statistical detector (CED) comes later; this already covers the vast
// majority of the 134 files affected.
constexpr UINT kLegacyCodepage = 1252;

// Strips the tags out of markup.
//
// Not a parser: a state machine that skips whatever sits between < and >, plus
// the script/style blocks whose content is code, not text. Good enough for
// indexing, and it has no attack surface.
char lowerAsciiChar(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

// The tag name starting at `at`, compared case-insensitively and requiring a
// delimiter after it: without the delimiter "<style>" and "<styling>" would be
// mistaken for each other.
bool tagNameIs(const std::string& source, size_t at, const char* name) {
    size_t k = 0;
    while (name[k]) {
        if (at + k >= source.size()) return false;
        if (lowerAsciiChar(source[at + k]) != name[k]) return false;
        ++k;
    }
    if (at + k >= source.size()) return true;
    const char after = source[at + k];
    return after == '>' || after == '/' || after == ' ' || after == '\t' ||
           after == '\n' || after == '\r';
}

// Looks for "</name" ignoring case: pages saved by Word and by old editors mix
// <script> and </SCRIPT> quite happily.
size_t findClosingTag(const std::string& source, size_t from, const char* name) {
    for (size_t i = from; i + 2 < source.size(); ++i) {
        if (source[i] != '<' || source[i + 1] != '/') continue;
        if (tagNameIs(source, i + 2, name)) return i;
    }
    return std::string::npos;
}

void stripMarkup(const std::string& source, std::string* out) {
    out->reserve(source.size() / 2);

    size_t i = 0;
    while (i < source.size()) {
        if (source[i] == '<') {
            // A tag is a word separator whatever else it is: without this
            // space "<b>Fattura</b>2026" would become "Fattura2026", and that
            // holds for ANY tag, including the blocks we skip whole.
            if (!out->empty() && out->back() != ' ' && out->back() != '\n') {
                out->push_back(' ');
            }

            // The blocks whose CONTENT must go, not just the tag.
            const char* skippable[] = {"script", "style", "svg"};
            bool handled = false;
            for (const char* name : skippable) {
                if (!tagNameIs(source, i + 1, name)) continue;
                const size_t end = findClosingTag(source, i, name);
                if (end == std::string::npos) {
                    // Closing tag never found: skipping to end of file would
                    // throw the whole document away. Drop the opening tag
                    // alone and keep reading.
                    const size_t close = source.find('>', i);
                    i = (close == std::string::npos) ? source.size() : close + 1;
                } else {
                    const size_t close = source.find('>', end);
                    i = (close == std::string::npos) ? source.size() : close + 1;
                }
                handled = true;
                break;
            }
            if (handled) continue;

            const size_t close = source.find('>', i);
            i = (close == std::string::npos) ? source.size() : close + 1;
            continue;
        }

        if (source[i] == '&') {
            // Only the entities that really matter for the text.
            struct Entity { const char* name; const char* value; };
            static const Entity kEntities[] = {
                {"&amp;", "&"},  {"&lt;", "<"},   {"&gt;", ">"},
                {"&quot;", "\""}, {"&apos;", "'"}, {"&nbsp;", " "},
            };
            bool matched = false;
            for (const Entity& entity : kEntities) {
                const size_t length = std::char_traits<char>::length(entity.name);
                if (source.compare(i, length, entity.name) == 0) {
                    *out += entity.value;
                    i += length;
                    matched = true;
                    break;
                }
            }
            if (matched) continue;
        }

        out->push_back(source[i]);
        ++i;
    }
}

// Cleans up the text: normalizes line endings, drops control characters,
// collapses repeated spaces but KEEPS the blank line, which marks a paragraph.
void tidy(const std::string& source, std::string* out) {
    out->clear();
    out->reserve(source.size());

    int pendingNewlines = 0;
    bool pendingSpace = false;
    bool started = false;

    for (size_t i = 0; i < source.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(source[i]);

        if (c == '\r') continue;  // CRLF becomes LF
        if (c == '\n') { ++pendingNewlines; continue; }
        if (c == ' ' || c == '\t' || c == '\v' || c == '\f') { pendingSpace = true; continue; }
        // Leftover control character: not text, no use to anyone.
        if (c < 0x20) continue;

        // Non-breaking space (U+00A0) and an encoding signature that ended up
        // mid-file (U+FEFF): on screen they are a space or nothing, but in the
        // bytes they are just some sequence, and without translating them they
        // weld words into a single token no search can ever reach.
        if (c == 0xC2 && i + 1 < source.size() &&
            static_cast<unsigned char>(source[i + 1]) == 0xA0) {
            pendingSpace = true;
            ++i;
            continue;
        }
        if (c == 0xEF && i + 2 < source.size() &&
            static_cast<unsigned char>(source[i + 1]) == 0xBB &&
            static_cast<unsigned char>(source[i + 2]) == 0xBF) {
            i += 2;
            continue;
        }

        if (started) {
            if (pendingNewlines >= 2) {
                *out += "\n\n";        // paragraph break: the chunker cuts here
            } else if (pendingNewlines == 1) {
                *out += '\n';
            } else if (pendingSpace) {
                *out += ' ';
            }
        }
        pendingNewlines = 0;
        pendingSpace = false;
        started = true;

        out->push_back(static_cast<char>(c));
    }
}

// From UTF-16 (already in memory as wchar_t) to UTF-8.
bool wideToUtf8(const wchar_t* text, size_t count, std::string* out) {
    if (count == 0) return false;
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(count),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return false;
    out->resize(static_cast<size_t>(bytes));
    WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(count), out->data(), bytes,
                        nullptr, nullptr);
    return true;
}

}  // namespace

bool normalizeToUtf8(const char* data, size_t size, TextEncoding encoding,
                     ContentKind kind, std::string* out) {
    out->clear();
    if (size == 0) return false;

    const size_t skip = bomLength(encoding, data, size);
    data += skip;
    size -= skip;
    if (size == 0) return false;

    std::string decoded;

    // The encoding comes from inspecting the first 8 KB. Before trusting it
    // for the WHOLE file we verify it over everything: if it fails, fall back
    // to CP-1252, which cannot fail and always produces valid UTF-8. Without
    // this, the accented tail of a mixed-encoding file ends up corrupted in
    // the index — and undecodable bytes further down degrade the chunker too.
    TextEncoding effective = encoding;
    if (effective == TextEncoding::Ascii || effective == TextEncoding::Utf8 ||
        effective == TextEncoding::Unknown) {
        if (!isValidUtf8(data, size)) effective = TextEncoding::Legacy;
    }

    switch (effective) {
        case TextEncoding::Ascii:
        case TextEncoding::Utf8:
        case TextEncoding::Unknown:
            decoded.assign(data, size);
            break;

        case TextEncoding::Utf16LE: {
            std::vector<wchar_t> wide(size / sizeof(wchar_t));
            std::memcpy(wide.data(), data, wide.size() * sizeof(wchar_t));
            if (!wideToUtf8(wide.data(), wide.size(), &decoded)) return false;
            break;
        }

        case TextEncoding::Utf16BE: {
            // Bytes in the opposite order to the native one: they have to be
            // swapped in pairs before they can be treated as wchar_t.
            std::vector<wchar_t> wide(size / sizeof(wchar_t));
            const auto* bytes = reinterpret_cast<const unsigned char*>(data);
            for (size_t i = 0; i < wide.size(); ++i) {
                wide[i] = static_cast<wchar_t>((bytes[i * 2] << 8) | bytes[i * 2 + 1]);
            }
            if (!wideToUtf8(wide.data(), wide.size(), &decoded)) return false;
            break;
        }

        case TextEncoding::Legacy: {
            const int count = MultiByteToWideChar(kLegacyCodepage, 0, data,
                                                  static_cast<int>(size), nullptr, 0);
            if (count <= 0) return false;
            std::vector<wchar_t> wide(static_cast<size_t>(count));
            MultiByteToWideChar(kLegacyCodepage, 0, data, static_cast<int>(size),
                                wide.data(), count);
            if (!wideToUtf8(wide.data(), wide.size(), &decoded)) return false;
            break;
        }

        case TextEncoding::Binary:
            return false;
    }

    if (kind == ContentKind::Markup) {
        std::string stripped;
        stripMarkup(decoded, &stripped);
        tidy(stripped, out);
    } else {
        tidy(decoded, out);
    }
    return !out->empty();
}

}  // namespace filo
