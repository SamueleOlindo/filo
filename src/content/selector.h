#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "index/file_index.h"

namespace filo {

// What kind of content we expect, based on the extension.
//
// The category is not cosmetic: it decides which extractor gets invoked and how
// much opening the file costs. A .txt is read and that is the end of it; a .pdf
// needs a parser that can take hundreds of milliseconds.
enum class ContentKind : uint8_t {
    Skip = 0,      // not indexable (binaries, media, archives)
    PlainText,     // txt, md, csv, log, ini...
    Code,          // sources: treated like text, but worth telling apart
    Markup,        // html, xml, svg: text inside tags
    Pdf,
    OfficeXml,     // docx, xlsx, pptx: zip + xml
    OfficeLegacy,  // doc, xls, ppt: compound file binary
    OpenDocument,  // odt, ods, odp
    RichText,      // rtf
    Ebook,         // epub
};

// Why a file was discarded. This exists to make selection inspectable rather
// than a black box: if the index cannot find a document, we want to be able to
// answer "it was excluded, and here is why".
enum class SkipReason : uint8_t {
    None = 0,
    IsDirectory,
    Deleted,
    NoExtension,
    UnknownExtension,
    ExcludedDirectory,  // node_modules, .git, assorted caches
    SystemOrHidden,
};

struct SelectionStats {
    uint64_t total = 0;
    uint64_t candidates = 0;
    uint64_t byKind[16] = {};
    uint64_t byReason[8] = {};
    double   elapsedSeconds = 0.0;
};

// Classifies a file name. Never touches the disk: it only looks at the
// extension.
ContentKind classifyByName(std::wstring_view name);

// True for formats whose text cannot be recovered by reading the file again.
//
// For .txt and source code the file on disk IS the text, so re-reading costs
// milliseconds and keeping a copy would be waste. A PDF is the opposite: the
// text only exists after a parser has run, in a separate process, for hundreds
// of milliseconds. That difference decides who gets their extracted text
// stored — and stored text is what lets snippets work for those formats and
// stops the embedding pass from parsing every document a second time.
bool needsExtractor(ContentKind kind);

// Walks the index and produces the list of files worth opening.
//
// Excluding by folder (node_modules, .git, build...) is the expensive part: it
// requires knowing whether a file descends from an excluded folder, and it has
// to be done without walking up the tree once per file.
std::vector<uint32_t> selectCandidates(const FileIndex& index, SelectionStats* stats);

// Rank penalty for MACHINE-GENERATED content.
//
// Logs, transcripts and run journals are legitimate text and searching them is
// sometimes useful, but they have a property that ruins ranking: they contain
// every term that ever passed through. So they match nearly every search, and
// without a correction they permanently occupy the top places, burying the
// documents the user actually wrote.
//
// The penalty pushes them below real documents without removing them: they
// still surface when they genuinely are the best answer.
double contentRankPenalty(const std::wstring& path);

const wchar_t* kindName(ContentKind kind);
const wchar_t* reasonName(SkipReason reason);

}  // namespace filo
