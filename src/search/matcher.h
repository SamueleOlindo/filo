#pragma once

#include <cstddef>
#include <cwctype>
#include <string>

namespace filo {

// Lowercase without allocating, and without going through the locale for ASCII.
//
// towlower() is correct but slow: it consults the current locale's tables on
// every call. Nearly every file name is ASCII, so that case is handled with a
// subtraction and towlower is left to accents and non-Latin scripts, where it
// genuinely earns its cost.
inline wchar_t lowerFast(wchar_t c) {
    if (c < 128) return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + 32) : c;
    return static_cast<wchar_t>(towlower(c));
}

// Searches for `lowerNeedle` (already lowercase) inside [text, text+length),
// ignoring case and building no temporary string.
//
// It takes a pointer and a length rather than an std::wstring because the names
// live in one contiguous pool: there is no string object to start from, only a
// range of characters.
inline bool containsCaseInsensitive(const wchar_t* text, size_t length,
                                    const wchar_t* needle, size_t needleLength) {
    if (needleLength == 0) return true;
    if (needleLength > length) return false;

    const wchar_t firstChar = needle[0];
    const size_t lastStart = length - needleLength;

    for (size_t i = 0; i <= lastStart; ++i) {
        // First-character filter: discards the vast majority of positions with
        // a single comparison, before entering the inner loop.
        if (lowerFast(text[i]) != firstChar) continue;

        size_t j = 1;
        while (j < needleLength && lowerFast(text[i + j]) == needle[j]) ++j;
        if (j == needleLength) return true;
    }
    return false;
}

inline std::wstring toLowerCopy(std::wstring text) {
    for (wchar_t& c : text) c = lowerFast(c);
    return text;
}

}  // namespace filo
