#pragma once

#include <string>

namespace filo {

// Is this path inside that folder?
//
// The obvious version of this is a prefix compare, and the obvious version is
// wrong. Both the classifier and the duplicate finder used one, and both agreed
// that C:\Users\alice-old sits inside C:\Users\alice — because the prefix does
// match, and nothing checked what came next. Two accounts whose names share a
// prefix, or a folder renamed with a suffix, and the ownership rule that the
// whole deletion path rests on quietly stops holding.
//
// So: the prefix has to match AND the next character has to be a separator.
// Strictly inside, too — a path equal to the folder is the folder itself, not a
// file in it, and every caller here is asking about a file.
//
// Both arguments must already be lowercase. Case folding belongs to the caller,
// which does it once per path rather than once per comparison.
inline bool insideFolder(const std::wstring& lowerPath, const std::wstring& lowerFolder) {
    // A trailing separator on the folder would otherwise make the comparison
    // come out right by accident, and wrong for the caller that omits it.
    size_t length = lowerFolder.size();
    while (length > 0 && lowerFolder[length - 1] == L'\\') --length;
    if (length == 0) return false;

    if (lowerPath.size() <= length) return false;
    if (lowerPath.compare(0, length, lowerFolder, 0, length) != 0) return false;
    return lowerPath[length] == L'\\';
}

}  // namespace filo
