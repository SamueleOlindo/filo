#include "search/query_plan.h"

#include <windows.h>

#include <algorithm>

#include "search/matcher.h"

namespace filo {
namespace {

constexpr int64_t kTicksPerSecond = 10'000'000LL;
constexpr int64_t kTicksPerDay = 86'400LL * kTicksPerSecond;

FILETIME toFileTime(int64_t ticks) {
    FILETIME file{};
    file.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFull);
    file.dwHighDateTime = static_cast<DWORD>(static_cast<uint64_t>(ticks) >> 32);
    return file;
}

int64_t fromFileTime(const FILETIME& file) {
    return (static_cast<int64_t>(file.dwHighDateTime) << 32) | file.dwLowDateTime;
}

// EVERY BOUNDARY IS COMPUTED IN LOCAL TIME AND HANDED BACK IN UTC.
//
// File timestamps are UTC and so is the clock, but "today" is a thing that
// happens in the room the user is sitting in. Computed in UTC, midnight in
// Italy falls at 01:00 or 02:00 UTC, so everything touched in those first hours
// of the day was reported as yesterday's — every day, for one or two hours,
// silently.
int64_t toLocal(int64_t ticks) {
    const FILETIME utc = toFileTime(ticks);
    FILETIME local{};
    if (!FileTimeToLocalFileTime(&utc, &local)) return ticks;
    return fromFileTime(local);
}

int64_t toUtc(int64_t localTicks) {
    const FILETIME local = toFileTime(localTicks);
    FILETIME utc{};
    if (!LocalFileTimeToFileTime(&local, &utc)) return localTicks;
    return fromFileTime(utc);
}

// Interprets ticks that are already in local time.
bool toLocalParts(int64_t ticks, SYSTEMTIME* out) {
    const FILETIME local = toFileTime(toLocal(ticks));
    return FileTimeToSystemTime(&local, out) != 0;
}

// Turns local calendar parts back into a UTC instant.
int64_t fromLocalParts(const SYSTEMTIME& parts) {
    FILETIME local{};
    if (!SystemTimeToFileTime(&parts, &local)) return 0;
    return toUtc(fromFileTime(local));
}

int64_t startOfDay(int64_t ticks) {
    SYSTEMTIME parts{};
    if (!toLocalParts(ticks, &parts)) return 0;
    parts.wHour = parts.wMinute = parts.wSecond = parts.wMilliseconds = 0;
    return fromLocalParts(parts);
}

// First instant of a month, and of the month after it. Local months.
void monthRange(int year, int month, int64_t* begin, int64_t* end) {
    SYSTEMTIME start{};
    start.wYear = static_cast<WORD>(year);
    start.wMonth = static_cast<WORD>(month);
    start.wDay = 1;
    start.wDayOfWeek = 0;
    *begin = fromLocalParts(start);

    SYSTEMTIME next = start;
    if (month == 12) {
        next.wYear = static_cast<WORD>(year + 1);
        next.wMonth = 1;
    } else {
        next.wMonth = static_cast<WORD>(month + 1);
    }
    *end = fromLocalParts(next);
}

void yearRange(int year, int64_t* begin, int64_t* end) {
    SYSTEMTIME start{};
    start.wYear = static_cast<WORD>(year);
    start.wMonth = 1;
    start.wDay = 1;
    *begin = fromLocalParts(start);
    SYSTEMTIME next = start;
    next.wYear = static_cast<WORD>(year + 1);
    *end = fromLocalParts(next);
}

// Monday of the week `ticks` falls in. SYSTEMTIME counts Sunday as 0, and a
// week that starts on Sunday would put "last week" a day out for most of
// Europe.
int64_t startOfWeek(int64_t ticks) {
    SYSTEMTIME parts{};
    if (!toLocalParts(ticks, &parts)) return startOfDay(ticks);
    const int sinceMonday = (parts.wDayOfWeek + 6) % 7;
    return startOfDay(ticks) - static_cast<int64_t>(sinceMonday) * kTicksPerDay;
}

// --- the vocabulary ---------------------------------------------------------
//
// Italian and English together, because the interface is English and the user
// is not. Someone searching their own disk types in whichever language the
// thought arrived in, and often mixes them inside one query.

struct FormatWord {
    const wchar_t* word;
    const wchar_t* extensions;   // space separated
};

const FormatWord kFormats[] = {
    {L"pdf", L"pdf"},
    {L"word", L"doc docx odt rtf"},
    {L"documento", L"doc docx odt rtf pdf"},
    {L"documenti", L"doc docx odt rtf pdf"},
    {L"doc", L"doc docx odt"},
    {L"excel", L"xls xlsx ods csv"},
    {L"foglio", L"xls xlsx ods csv"},
    {L"spreadsheet", L"xls xlsx ods csv"},
    {L"powerpoint", L"ppt pptx odp"},
    {L"presentazione", L"ppt pptx odp"},
    {L"slide", L"ppt pptx odp"},
    {L"immagine", L"jpg jpeg png gif webp heic bmp"},
    {L"immagini", L"jpg jpeg png gif webp heic bmp"},
    {L"foto", L"jpg jpeg png gif webp heic bmp"},
    {L"image", L"jpg jpeg png gif webp heic bmp"},
    {L"picture", L"jpg jpeg png gif webp heic bmp"},
    {L"screenshot", L"png jpg jpeg"},
    {L"video", L"mp4 mkv avi mov wmv webm"},
    {L"audio", L"mp3 flac wav m4a ogg"},
    {L"musica", L"mp3 flac wav m4a ogg"},
    {L"archivio", L"zip rar 7z tar gz"},
    {L"archive", L"zip rar 7z tar gz"},
    {L"zip", L"zip rar 7z"},
    {L"ebook", L"epub mobi azw3"},
    {L"testo", L"txt md"},
    {L"appunti", L"txt md"},
};

struct FolderWord {
    const wchar_t* word;
    const wchar_t* fragment;
};

const FolderWord kFolders[] = {
    {L"download", L"\\downloads\\"},
    {L"downloads", L"\\downloads\\"},
    {L"scaricati", L"\\downloads\\"},
    {L"desktop", L"\\desktop\\"},
    {L"scrivania", L"\\desktop\\"},
    {L"documenti", L"\\documents\\"},
    {L"documents", L"\\documents\\"},
    {L"immagini", L"\\pictures\\"},
    {L"pictures", L"\\pictures\\"},
};

const wchar_t* const kMonths[12][2] = {
    {L"gennaio", L"january"},   {L"febbraio", L"february"}, {L"marzo", L"march"},
    {L"aprile", L"april"},      {L"maggio", L"may"},        {L"giugno", L"june"},
    {L"luglio", L"july"},       {L"agosto", L"august"},     {L"settembre", L"september"},
    {L"ottobre", L"october"},   {L"novembre", L"november"}, {L"dicembre", L"december"},
};

// Articles and prepositions that carry no search meaning of their own.
//
// They are only ever dropped when they sit NEXT TO a word that turned out to be
// a filter — "il pdf del contratto" loses "il" and "del" because "pdf" was
// lifted out, while "il mondo al contrario" keeps every word, because nothing
// in it was a filter. Dropping them unconditionally would quietly break
// searching for a title.
bool isFillerWord(const std::wstring& word) {
    static const wchar_t* const kFillers[] = {
        L"il",    L"lo",    L"la",    L"i",     L"gli",   L"le",    L"un",
        L"uno",   L"una",   L"nei",   L"nel",   L"nella", L"nelle", L"nello",
        L"negli", L"in",    L"su",    L"sul",   L"sui",   L"sulle", L"sugli",
        L"dal",   L"dei",   L"del",   L"della", L"delle", L"degli", L"dalla",
        L"dalle", L"dagli", L"dai",   L"di",    L"da",    L"a",     L"al",
        L"allo",  L"alla",  L"alle",  L"agli",  L"ai",    L"coi",   L"col",
        L"the",   L"my",    L"of",    L"on",    L"from",  L"last",  L"this",
        L"these", L"those", L"some",  L"any",   L"all"};
    for (const wchar_t* filler : kFillers) {
        if (word == filler) return true;
    }
    return false;
}

// Words that carry no search meaning: the grammar of a question rather than
// its subject. Every filler is one of these, plus the interrogatives and the
// auxiliaries that only ever appear in a sentence someone spoke.
//
// Dropped ONLY from queries of three words or more, and never all of them: on a
// short query these words are usually the whole point ("the who", "la casa"),
// and on a long one they are what stops the AND-joined content search from
// matching anything at all.
bool isStopWord(const std::wstring& word) {
    if (isFillerWord(word)) return true;
    static const wchar_t* const kStopWords[] = {
        // Italian: interrogatives, auxiliaries, common connectives
        L"che", L"chi", L"cui", L"dove", L"quando", L"quanto", L"quanta", L"quanti",
        L"come", L"perche", L"quale", L"quali", L"quel", L"quella", L"quello",
        L"quelli", L"quelle", L"questo", L"questa", L"ho", L"hai", L"ha", L"abbiamo",
        L"avete", L"hanno", L"sono", L"sei", L"siamo", L"siete", L"mi", L"ti", L"si",
        L"ci", L"vi", L"ne", L"per", L"con", L"e", L"o", L"ma", L"se", L"non",
        L"sta", L"stanno", L"c'e", L"era", L"erano", L"al", L"alla", L"allo",
        L"agli", L"alle", L"ai", L"col", L"sul", L"sulla",
        // English
        L"where", L"what", L"which", L"who", L"when", L"how", L"why", L"is", L"are",
        L"was", L"were", L"be", L"been", L"do", L"does", L"did", L"have", L"has",
        L"had", L"i", L"you", L"it", L"that", L"with", L"for", L"and", L"or",
        L"but", L"not", L"me", L"about", L"there", L"here", L"can", L"could",
        L"would", L"should", L"much", L"many",
    };
    for (const wchar_t* stop : kStopWords) {
        if (word == stop) return true;
    }
    return false;
}

// Punctuation that clings to the outside of a word and means nothing to the
// search. Trimmed from the ends only: the colon in "tipo:pdf" and the dot in
// "v1.2" are load-bearing, while the question mark in "ieri?" stopped it being
// recognized as a date at all.
bool isEdgePunctuation(wchar_t c) {
    switch (c) {
        case L'?': case L'!': case L'.': case L',': case L';': case L':':
        case L'"': case L'\'': case L'(': case L')': case L'[': case L']':
        case L'{': case L'}': case L'<': case L'>': case L'-': case L'–':
        case L'—': case L'«': case L'»': case L'“':
        case L'”': case L'…':
            return true;
        default:
            return false;
    }
}

std::wstring trimPunctuation(std::wstring word) {
    size_t begin = 0;
    size_t end = word.size();
    while (begin < end && isEdgePunctuation(word[begin])) ++begin;
    while (end > begin && isEdgePunctuation(word[end - 1])) --end;
    // A word made entirely of punctuation keeps itself: it is not a word, but
    // deleting it would silently change what the user asked for.
    if (begin >= end) return word;
    return word.substr(begin, end - begin);
}

std::vector<std::wstring> splitWords(const std::wstring& text) {
    std::vector<std::wstring> words;
    std::wstring current;
    for (const wchar_t c : text) {
        if (c == L' ' || c == L'\t' || c == L'\n') {
            if (!current.empty()) words.push_back(trimPunctuation(current));
            current.clear();
        } else {
            current.push_back(lowerFast(c));
        }
    }
    if (!current.empty()) words.push_back(trimPunctuation(current));
    return words;
}

void addExtensions(const wchar_t* list, std::vector<std::wstring>* out) {
    std::wstring current;
    for (const wchar_t* c = list;; ++c) {
        if (*c == L' ' || *c == L'\0') {
            if (!current.empty() &&
                std::find(out->begin(), out->end(), current) == out->end()) {
                out->push_back(current);
            }
            current.clear();
            if (*c == L'\0') break;
        } else {
            current.push_back(*c);
        }
    }
}

bool allDigits(const std::wstring& word) {
    if (word.empty()) return false;
    for (const wchar_t c : word) {
        if (c < L'0' || c > L'9') return false;
    }
    return true;
}

}  // namespace

int64_t nowFileTime() {
    FILETIME file{};
    GetSystemTimeAsFileTime(&file);
    return (static_cast<int64_t>(file.dwHighDateTime) << 32) | file.dwLowDateTime;
}

QueryPlan planQuery(const std::wstring& rawQuery, int64_t now) {
    QueryPlan plan;
    plan.originalText = rawQuery;
    plan.text = rawQuery;

    const std::vector<std::wstring> words = splitWords(rawQuery);
    if (words.empty()) return plan;

    std::vector<bool> consumed(words.size(), false);

    // Local calendar parts: "marzo" means the user's March, and "questo mese"
    // the month they are living in, not the one Greenwich is.
    SYSTEMTIME nowParts{};
    const bool haveNow = toLocalParts(now, &nowParts);
    const int64_t today = startOfDay(now);

    for (size_t i = 0; i < words.size(); ++i) {
        const std::wstring& word = words[i];
        const std::wstring next = (i + 1 < words.size()) ? words[i + 1] : L"";

        // --- explicit operators: tipo:pdf, type:pdf, ext:docx ---------------
        const size_t colon = word.find(L':');
        if (colon != std::wstring::npos && colon + 1 < word.size()) {
            const std::wstring key = word.substr(0, colon);
            const std::wstring value = word.substr(colon + 1);
            if (key == L"tipo" || key == L"type" || key == L"ext" ||
                key == L"estensione") {
                addExtensions(value.c_str(), &plan.extensions);
                consumed[i] = true;
                continue;
            }
        }

        // --- two-word time phrases ------------------------------------------
        if (!next.empty() && haveNow) {
            const std::wstring pair = word + L" " + next;
            bool matched = true;
            if (pair == L"settimana scorsa" || pair == L"last week") {
                // The Monday-to-Sunday block BEFORE the current one. It used to
                // mean "the last seven days", which is a different set and
                // never the one someone means by "last week".
                const int64_t thisMonday = startOfWeek(now);
                plan.after = thisMonday - 7 * kTicksPerDay;
                plan.before = thisMonday;
            } else if (pair == L"questa settimana" || pair == L"this week") {
                plan.after = startOfWeek(now);
            } else if (pair == L"mese scorso" || pair == L"last month") {
                int year = nowParts.wYear, month = nowParts.wMonth - 1;
                if (month == 0) { month = 12; --year; }
                monthRange(year, month, &plan.after, &plan.before);
            } else if (pair == L"questo mese" || pair == L"this month") {
                monthRange(nowParts.wYear, nowParts.wMonth, &plan.after, &plan.before);
            } else if (pair == L"anno scorso" || pair == L"l'anno scorso" ||
                       pair == L"last year") {
                yearRange(nowParts.wYear - 1, &plan.after, &plan.before);
            } else if (pair == L"quest'anno" || pair == L"questo anno" ||
                       pair == L"this year") {
                yearRange(nowParts.wYear, &plan.after, &plan.before);
            } else {
                matched = false;
            }
            if (matched) {
                consumed[i] = consumed[i + 1] = true;
                ++i;
                continue;
            }
        }

        // --- one-word time --------------------------------------------------
        if (haveNow) {
            bool matched = true;
            if (word == L"oggi" || word == L"today") {
                plan.after = today;
            } else if (word == L"ieri" || word == L"yesterday") {
                plan.after = today - kTicksPerDay;
                plan.before = today;
            } else if (word == L"recente" || word == L"recenti" || word == L"recent" ||
                       word == L"recently") {
                plan.after = today - 30 * kTicksPerDay;
            } else {
                matched = false;
            }
            if (!matched) {
                for (int m = 0; m < 12 && !matched; ++m) {
                    if (word != kMonths[m][0] && word != kMonths[m][1]) continue;
                    // A month that has not arrived yet must mean last year's.
                    int year = nowParts.wYear;
                    if (m + 1 > nowParts.wMonth) --year;
                    monthRange(year, m + 1, &plan.after, &plan.before);
                    matched = true;
                }
            }
            if (!matched && word.size() == 4 && allDigits(word)) {
                const int year = _wtoi(word.c_str());
                if (year >= 1990 && year <= 2099) {
                    yearRange(year, &plan.after, &plan.before);
                    matched = true;
                }
            }
            if (matched) {
                consumed[i] = true;
                continue;
            }
        }

        // --- folders ---------------------------------------------------------
        // Only when a preposition points at them. "download" on its own is far
        // more often part of a filename than a place.
        if (i > 0 && isFillerWord(words[i - 1])) {
            bool matched = false;
            for (const FolderWord& folder : kFolders) {
                if (word != folder.word) continue;
                plan.folder = folder.fragment;
                consumed[i] = consumed[i - 1] = true;
                matched = true;
                break;
            }
            if (matched) continue;
        }

        // --- formats ----------------------------------------------------------
        for (const FormatWord& format : kFormats) {
            if (word != format.word) continue;
            addExtensions(format.extensions, &plan.extensions);
            consumed[i] = true;
            break;
        }
    }

    // Articles and prepositions touching a filter go with it. Repeated until
    // nothing more changes, so a chain like "il pdf del contratto" collapses
    // from both sides of "pdf" in one call rather than needing the loop unrolled
    // by hand.
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t i = 0; i < words.size(); ++i) {
            if (consumed[i] || !isFillerWord(words[i])) continue;
            const bool touchesFilter = (i > 0 && consumed[i - 1]) ||
                                       (i + 1 < words.size() && consumed[i + 1]);
            if (!touchesFilter) continue;
            consumed[i] = true;
            changed = true;
        }
    }

    // Function words go too, once the query is long enough to be a sentence.
    //
    // This is not cosmetic. The content search joins every term with AND, so
    // "where is the spreadsheet with the invoices" would demand a document
    // containing "where", "is", "with" and "the" — and find nothing. A question
    // asked in prose is unsearchable until its grammar is removed.
    //
    // THE THRESHOLD COUNTS WHAT THE USER TYPED, not what survived the filters.
    // A query of three words or fewer is a name or a title, and its function
    // words are the point: "This Is Us" collapsed to "us", and "la casa" would
    // have become "casa". Four words or more is a sentence, and there the
    // grammar is noise no matter how much of it the filters already took.
    if (words.size() >= 4) {
        size_t surviving = 0;
        for (size_t i = 0; i < words.size(); ++i) {
            if (!consumed[i]) ++surviving;
        }
        for (size_t i = 0; i < words.size() && surviving > 1; ++i) {
            if (consumed[i] || !isStopWord(words[i])) continue;
            consumed[i] = true;
            --surviving;
        }
    }

    // Rebuild the text from what nobody claimed.
    std::wstring remaining;
    for (size_t i = 0; i < words.size(); ++i) {
        if (consumed[i]) continue;
        if (!remaining.empty()) remaining += L' ';
        remaining += words[i];
    }

    // Everything was a filter: "foto" alone, or "pdf di marzo". Stripping it all
    // would leave nothing to search for, so the words go back in AND the filters
    // stay — the query then means "find this word, and prefer these files",
    // which is the safer of the two readings.
    plan.text = remaining.empty() ? rawQuery : remaining;
    return plan;
}

int QueryPlan::satisfiedBy(std::wstring_view lowerPath, int64_t mtime) const {
    int satisfied = 0;

    if (!extensions.empty()) {
        const size_t dot = lowerPath.find_last_of(L'.');
        const size_t slash = lowerPath.find_last_of(L'\\');
        if (dot != std::wstring_view::npos &&
            (slash == std::wstring_view::npos || dot > slash)) {
            const std::wstring_view actual = lowerPath.substr(dot + 1);
            for (const std::wstring& wanted : extensions) {
                if (actual == wanted) { ++satisfied; break; }
            }
        }
    }

    if (!folder.empty() && lowerPath.find(folder) != std::wstring_view::npos) ++satisfied;

    // An unknown time earns nothing — it does not lose anything either.
    //
    // This used to award the full bonus, on the reasoning that a fact we failed
    // to read must not cost a document its place. But awarding is much stronger
    // than not penalizing: it put files we know nothing about level with files
    // we know match, and above files we know do not. Silence is not agreement.
    if ((after != 0 || before != 0) && mtime != 0) {
        const bool afterOk = after == 0 || mtime >= after;
        const bool beforeOk = before == 0 || mtime < before;
        if (afterOk && beforeOk) ++satisfied;
    }

    return satisfied;
}

}  // namespace filo
