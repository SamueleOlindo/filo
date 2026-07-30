#include "store/tokenizer.h"

#include <sqlite3.h>
#include <fts5.h>

#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "snowball/runtime/api.h"
#include "snowball/src_c/stem_UTF_8_english.h"
#include "snowball/src_c/stem_UTF_8_italian.h"
}

namespace filo {
namespace {

// --- character folding ------------------------------------------------------
//
// Two jobs in one: lowercasing and stripping diacritics. The second matters as
// much as the first in Italian, because people write "perche'", "perché" and
// "perche" interchangeably: without normalizing them those are three different
// words.

// Latin Extended-A (U+0100-U+017F) reduced to the base letter. One character of
// this string per codepoint in the range, in order.
constexpr const char* kLatinExtendedA =
    "aaaaaa"            // Ā ā Ă ă Ą ą
    "cccccccc"          // Ć ... č
    "dddd"              // Ď ď Đ đ
    "eeeeeeeeee"        // Ē ... ě
    "gggggggg"          // Ĝ ... ģ
    "hhhh"              // Ĥ ĥ Ħ ħ
    "iiiiiiiiii"        // Ĩ ... ı
    "ii"                // Ĳ ĳ
    "jj"                // Ĵ ĵ
    "kkk"               // Ķ ķ ĸ
    "llllllllll"        // Ĺ ... ł
    "nnnnnnnnn"         // Ń ... ŋ
    "oooooo"            // Ō ... ő
    "oo"                // Œ œ
    "rrrrrr"            // Ŕ ... ř
    "ssssssss"          // Ś ... š
    "tttttt"            // Ţ ... ŧ
    "uuuuuuuuuuuu"      // Ũ ... ų
    "ww"                // Ŵ ŵ
    "yyy"               // Ŷ ŷ Ÿ
    "zzzzzz"            // Ź ... ž
    "s";                // ſ

// Returns the folded form of a codepoint, written as UTF-8 into `out`. It can
// produce more than one character: ß becomes "ss", æ becomes "ae".
void foldCodepoint(unsigned int cp, std::string* out) {
    if (cp < 0x80) {
        out->push_back(static_cast<char>(cp >= 'A' && cp <= 'Z' ? cp + 32 : cp));
        return;
    }

    // Combining marks: they vanish without writing anything and WITHOUT ending
    // the token. Text in decomposed form writes "e" followed by a separate
    // accent; this way it folds exactly like the precomposed letter, whereas
    // treating them as separators would break the word in half.
    if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
        (cp >= 0x20D0 && cp <= 0x20F0) || (cp >= 0xFE20 && cp <= 0xFE2F)) {
        return;
    }

    // Latin-1 Supplement: the accented letters in everyday use.
    if (cp >= 0xC0 && cp <= 0xFF) {
        // 0xFF (y with diaeresis) has to be kept out of the subtraction: it
        // would give 0xDF, the eszett case, and the word would end in "ss"
        // instead of "y" — while the matching capital folds correctly, so the
        // same word would produce different tokens.
        const unsigned int base = (cp >= 0xE0 && cp != 0xFF) ? cp - 0x20 : cp;
        switch (base) {
            case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5:
                out->push_back('a'); return;
            case 0xC6: *out += "ae"; return;
            case 0xC7: out->push_back('c'); return;
            case 0xC8: case 0xC9: case 0xCA: case 0xCB: out->push_back('e'); return;
            case 0xCC: case 0xCD: case 0xCE: case 0xCF: out->push_back('i'); return;
            case 0xD0: out->push_back('d'); return;
            case 0xD1: out->push_back('n'); return;
            case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD8:
                out->push_back('o'); return;
            case 0xD9: case 0xDA: case 0xDB: case 0xDC: out->push_back('u'); return;
            case 0xDD: out->push_back('y'); return;
            case 0xDE: *out += "th"; return;
            case 0xDF: *out += "ss"; return;   // ß
            default: break;
        }
        if (cp == 0xFF) { out->push_back('y'); return; }
    }

    if (cp >= 0x100 && cp <= 0x17F) {
        out->push_back(kLatinExtendedA[cp - 0x100]);
        return;
    }

    // Greek and Cyrillic: lowercased only, not reduced.
    if (cp >= 0x391 && cp <= 0x3A9) cp += 0x20;
    else if (cp >= 0x410 && cp <= 0x42F) cp += 0x20;
    else if (cp >= 0x400 && cp <= 0x40F) cp += 0x50;

    // Write it back out as UTF-8.
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

// Decodes one UTF-8 codepoint. Returns how many bytes it consumed.
int decodeUtf8(const char* text, int available, unsigned int* cp) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(text);
    const unsigned char c = bytes[0];
    // The following bytes have to be checked as continuations, not merely
    // counted: a stray lead byte followed by a space would swallow the space
    // into a character that does not exist, welding two words into one.
    const auto isContinuation = [&](int k) { return (bytes[k] & 0xC0) == 0x80; };

    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0 && available >= 2 && isContinuation(1)) {
        *cp = ((c & 0x1F) << 6) | (bytes[1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && available >= 3 && isContinuation(1) && isContinuation(2)) {
        *cp = ((c & 0x0F) << 12) | ((bytes[1] & 0x3F) << 6) | (bytes[2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && available >= 4 && isContinuation(1) && isContinuation(2) &&
        isContinuation(3)) {
        *cp = ((c & 0x07) << 18) | ((bytes[1] & 0x3F) << 12) |
              ((bytes[2] & 0x3F) << 6) | (bytes[3] & 0x3F);
        return 4;
    }
    *cp = c;   // invalid byte: treated as a separator
    return 1;
}

bool isLetter(unsigned int cp) {
    if (cp < 0x80) return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');

    // Above ASCII the assumption is "letter", but the exceptions matter more
    // than the rule. Italian text copied off the web or exported from Word is
    // full of non-breaking spaces, guillemets and soft hyphens: treating those
    // as letters welds words into a single token no search can reach.
    // Guillemets are the worst case, because they always enclose the word:
    // «penale» would become unfindable by searching for penale.
    if (cp >= 0x0080 && cp <= 0x00BF) return false;  // NBSP, guillemets, ·, ¬, soft hyphen
    if (cp == 0x00D7 || cp == 0x00F7) return false;  // multiplication and division signs
    if (cp >= 0x02B0 && cp <= 0x02FF) return false;  // modifier letters
    if (cp >= 0x2000 && cp <= 0x206F) return false;  // general punctuation
    if (cp >= 0x2070 && cp <= 0x209F) return false;  // superscripts and subscripts
    if (cp >= 0x20A0 && cp <= 0x2BFF) return false;  // currency, arrows, symbols
    if (cp >= 0x2E00 && cp <= 0x2E7F) return false;  // supplemental punctuation
    if (cp >= 0x3000 && cp <= 0x303F) return false;  // CJK punctuation
    if (cp >= 0xFE00 && cp <= 0xFE0F) return false;  // variation selectors
    if (cp == 0xFEFF) return false;                  // byte order mark mid-file
    if (cp >= 0xFF01 && cp <= 0xFF20) return false;  // fullwidth punctuation

    // Combining marks DELIBERATELY stay letters: they have to disappear inside
    // foldCodepoint, not break the word they are accenting.
    return true;
}

bool isDigit(unsigned int cp) { return cp >= '0' && cp <= '9'; }
bool isUpperAscii(unsigned int cp) { return cp >= 'A' && cp <= 'Z'; }
bool isLowerAscii(unsigned int cp) { return cp >= 'a' && cp <= 'z'; }

// Part of a token? The underscore is: "user_profile" has to stay searchable
// whole, as well as in its parts.
bool isTokenChar(unsigned int cp) {
    return isLetter(cp) || isDigit(cp) || cp == '_';
}

// --- the tokenizer ----------------------------------------------------------

// Fts5Tokenizer is an OPAQUE type: fts5.h declares it without defining it,
// because the contents are the implementation's business. You do not inherit
// from it or embed it: you hand FTS5 your own pointer in disguise, and undo the
// disguise on every call.
struct FiloTokenizer {
    struct SN_env* italian = nullptr;
    struct SN_env* english = nullptr;
};

struct Subword {
    int start;   // offset within the folded token
    int length;
};

int stemInto(struct SN_env* env, int (*stem)(struct SN_env*), const std::string& word,
             std::string* out) {
    if (!env) return 0;

    // On failure the state has to be zeroed before returning. When the Snowball
    // runtime cannot allocate it leaves the buffer null but the length of the
    // PREVIOUS token: the next call would compute a negative size and pass it
    // to memmove as an enormous size_t.
    if (SN_set_current(env, static_cast<int>(word.size()),
                       reinterpret_cast<const symbol*>(word.data())) < 0) {
        env->l = 0;
        env->c = 0;
        return 0;
    }
    env->l = static_cast<int>(word.size());
    if (stem(env) < 0) {
        env->l = 0;
        env->c = 0;
        return 0;
    }
    if (!env->p || env->l < 0) return 0;
    out->assign(reinterpret_cast<const char*>(env->p), static_cast<size_t>(env->l));
    return 1;
}

int tokenizeImpl(Fts5Tokenizer* tokenizer, void* context, int flags,
                 const char* text, int textLength, const char* /*locale*/,
                 int /*localeLength*/,
                 int (*emit)(void*, int, const char*, int, int, int)) {
    auto* self = reinterpret_cast<FiloTokenizer*>(tokenizer);

    // Indexing and querying are NOT symmetric, and confusing them is the
    // classic mistake of tokenizers that emit variants.
    //
    // A colocated token, in a query, becomes an OR ALTERNATIVE. For stems that
    // is exactly what is wanted: "contratti" should also catch documents that
    // say "contratto". For the parts of an identifier it is ruinous:
    // "ItemBlocking" would become "itemblocking OR item OR blocking", and a
    // precise search for a symbol would return every file that mentions "item".
    //
    // So: identifier parts are emitted only while INDEXING. Anyone searching
    // for "blocking" still finds them, because they are in the index.
    const bool isQuery = (flags & FTS5_TOKENIZE_QUERY) != 0;

    std::string folded;
    std::string stem;
    std::vector<Subword> subwords;

    int i = 0;
    while (i < textLength) {
        unsigned int cp = 0;
        int width = decodeUtf8(text + i, textLength - i, &cp);
        if (!isTokenChar(cp)) { i += width; continue; }

        const int tokenStart = i;
        folded.clear();
        subwords.clear();
        int currentStart = 0;
        bool previousLower = false;
        bool hasDigit = false;
        bool hasSeparator = false;

        while (i < textLength) {
            width = decodeUtf8(text + i, textLength - i, &cp);
            if (!isTokenChar(cp)) break;

            if (cp == '_') {
                // Explicit boundary: it ends the subword in progress.
                hasSeparator = true;
                if (static_cast<int>(folded.size()) > currentStart) {
                    subwords.push_back({currentStart,
                                        static_cast<int>(folded.size()) - currentStart});
                }
                folded.push_back('_');
                currentStart = static_cast<int>(folded.size());
                previousLower = false;
                i += width;
                continue;
            }

            // Lower-to-upper transition: that is a camelCase boundary.
            if (previousLower && isUpperAscii(cp)) {
                hasSeparator = true;
                if (static_cast<int>(folded.size()) > currentStart) {
                    subwords.push_back({currentStart,
                                        static_cast<int>(folded.size()) - currentStart});
                }
                currentStart = static_cast<int>(folded.size());
            }

            if (isDigit(cp)) hasDigit = true;
            previousLower = isLowerAscii(cp) || isDigit(cp);
            foldCodepoint(cp, &folded);
            i += width;
        }

        if (folded.empty()) continue;
        if (static_cast<int>(folded.size()) > currentStart) {
            subwords.push_back({currentStart,
                                static_cast<int>(folded.size()) - currentStart});
        }

        const int tokenEnd = i;

        // 1. The normalized form: the main token, the one that makes literal
        //    search and phrase queries possible.
        int rc = emit(context, 0, folded.data(), static_cast<int>(folded.size()),
                      tokenStart, tokenEnd);
        if (rc != SQLITE_OK) return rc;

        // 2. The parts of an identifier, COLOCATED: same position, so they
        //    shift nothing and do not break phrases. Searching "profile" finds
        //    "user_profile"; searching "user_profile" still finds it too.
        // The earlier condition, "(hasSeparator || subwords.size() > 1) &&
        // subwords.size() > 1", reduced algebraically to subwords.size() > 1:
        // hasSeparator had no effect at all. When the separators sit only at
        // the start or the end — Markdown's "_importante_", Python's
        // "__init__", Google-style "count_" — there is exactly ONE useful
        // subword and it was thrown away: those documents could not be found by
        // searching "importante", "init" or "count".
        const bool isIdentifier = hasSeparator || subwords.size() > 1;

        if (isIdentifier && !isQuery) {
            for (const Subword& part : subwords) {
                if (part.length < 2) continue;
                // No point repeating the main token as a variant.
                if (part.start == 0 && part.length == static_cast<int>(folded.size())) {
                    continue;
                }
                rc = emit(context, FTS5_TOKEN_COLOCATED, folded.data() + part.start,
                          part.length, tokenStart, tokenEnd);
                if (rc != SQLITE_OK) return rc;
            }
        }
        // An identifier is not a word in any language: stemming it produces
        // noise. The rule holds on both sides, otherwise query and index would
        // stop meeting.
        if (isIdentifier) continue;

        if (hasDigit || folded.size() < 4) continue;

        // 3. The stems, Italian AND English, always both.
        //
        //    Emitting both avoids having to guess the language of the QUERY,
        //    which on a single word is impossible and is exactly where
        //    "language-aware" schemes fall apart.
        //    They are emitted only when DIFFERENT from the surface form,
        //    otherwise the index would double for nothing.
        if (stemInto(self->italian, italian_UTF_8_stem, folded, &stem) &&
            stem != folded && stem.size() >= 2) {
            rc = emit(context, FTS5_TOKEN_COLOCATED, stem.data(),
                      static_cast<int>(stem.size()), tokenStart, tokenEnd);
            if (rc != SQLITE_OK) return rc;
        }
        std::string englishStem;
        if (stemInto(self->english, english_UTF_8_stem, folded, &englishStem) &&
            englishStem != folded && englishStem != stem && englishStem.size() >= 2) {
            rc = emit(context, FTS5_TOKEN_COLOCATED, englishStem.data(),
                      static_cast<int>(englishStem.size()), tokenStart, tokenEnd);
            if (rc != SQLITE_OK) return rc;
        }
    }
    return SQLITE_OK;
}

int createImpl(void* /*context*/, const char** /*argv*/, int /*argc*/,
               Fts5Tokenizer** out) {
    auto* tokenizer = new (std::nothrow) FiloTokenizer();
    if (!tokenizer) return SQLITE_NOMEM;
    // One stemmer per instance: the library is not reentrant, and instances are
    // per connection, which is exactly our threading model.
    tokenizer->italian = italian_UTF_8_create_env();
    tokenizer->english = english_UTF_8_create_env();
    if (!tokenizer->italian || !tokenizer->english) {
        if (tokenizer->italian) italian_UTF_8_close_env(tokenizer->italian);
        if (tokenizer->english) english_UTF_8_close_env(tokenizer->english);
        delete tokenizer;
        return SQLITE_NOMEM;
    }
    *out = reinterpret_cast<Fts5Tokenizer*>(tokenizer);
    return SQLITE_OK;
}

void deleteImpl(Fts5Tokenizer* tokenizer) {
    auto* self = reinterpret_cast<FiloTokenizer*>(tokenizer);
    if (!self) return;
    if (self->italian) italian_UTF_8_close_env(self->italian);
    if (self->english) english_UTF_8_close_env(self->english);
    delete self;
}

// The only documented way to obtain the FTS5 API: a fake SELECT whose argument
// is a pointer the extension fills in.
fts5_api* fetchFts5Api(sqlite3* db) {
    fts5_api* api = nullptr;
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT fts5(?1)", -1, &statement, nullptr) != SQLITE_OK) {
        return nullptr;
    }
    sqlite3_bind_pointer(statement, 1, &api, "fts5_api_ptr", nullptr);
    sqlite3_step(statement);
    sqlite3_finalize(statement);
    return api;
}

}  // namespace

bool registerTokenizer(sqlite3* db, std::wstring* error) {
    fts5_api* api = fetchFts5Api(db);
    if (!api) {
        *error = L"FTS5 is not available on this connection";
        return false;
    }

    static fts5_tokenizer_v2 tokenizer = {
        2,             // iVersion
        createImpl,
        deleteImpl,
        tokenizeImpl,
    };

    if (api->xCreateTokenizer_v2(api, kTokenizerName, nullptr, &tokenizer, nullptr) !=
        SQLITE_OK) {
        *error = L"the tokenizer could not be registered";
        return false;
    }
    return true;
}

}  // namespace filo
