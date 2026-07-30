#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace filo {

// What a query asks for BEYOND its words.
//
// "il pdf del contratto di marzo" carries three different things: a subject
// (contract), a format (pdf) and a time (March). Today all three go into the
// text search, so "pdf" and "marzo" are hunted for INSIDE the documents — which
// is the opposite of what was meant. A plan separates them.
//
// THE FILTERS NEVER REMOVE ANYTHING.
//
// This is the rule the whole design hangs on. A filter that excludes is a
// filter that can hide the right answer, and it hides it silently: the user
// sees an empty list and concludes the program is broken, with nothing on
// screen to suggest the format was guessed wrong. So a satisfied filter adds
// score and an unsatisfied one does not subtract. Being sure enough to reorder
// is a much weaker claim than being sure enough to delete, and this parser is
// only ever the former.
struct QueryPlan {
    std::wstring text;           // what actually reaches the search engines
    std::wstring originalText;   // what the user typed, unchanged

    std::vector<std::wstring> extensions;   // lowercase, no leading dot
    std::wstring folder;                    // lowercase fragment the path should hold

    // FILETIME bounds, 0 meaning unbounded.
    int64_t after = 0;
    int64_t before = 0;

    bool hasFilters() const {
        return !extensions.empty() || !folder.empty() || after != 0 || before != 0;
    }

    // How many of the active filters this file satisfies.
    //
    // `mtime` of 0 means "not known" and counts as satisfying the time filter:
    // a fact we failed to read must not cost a document its place.
    int satisfiedBy(std::wstring_view lowerPath, int64_t mtime) const;
};

// Parses a query. `now` is the reference instant for relative dates, passed in
// rather than read from the clock so the result is reproducible and testable.
QueryPlan planQuery(const std::wstring& rawQuery, int64_t now);

// Current time as a FILETIME (100 ns ticks since 1601), the same scale the
// file system reports modification times in.
int64_t nowFileTime();

}  // namespace filo
