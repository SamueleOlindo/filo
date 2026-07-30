#include "content/selector.h"

#include <windows.h>

#include <chrono>
#include <unordered_map>

#include "search/matcher.h"

namespace filo {
namespace {

constexpr size_t kMaxTokenChars = 64;

// Lowercase copy into a stack buffer. No allocation: this is called millions
// of times.
size_t lowerInto(std::wstring_view source, wchar_t* buffer) {
    const size_t length = source.size() < kMaxTokenChars ? source.size() : kMaxTokenChars;
    for (size_t i = 0; i < length; ++i) buffer[i] = lowerFast(source[i]);
    return length;
}

// The extension without the dot. Empty if there is none.
std::wstring_view extensionOf(std::wstring_view name) {
    const size_t dot = name.rfind(L'.');
    if (dot == std::wstring_view::npos || dot + 1 >= name.size()) return {};
    return name.substr(dot + 1);
}

using ExtensionTable = std::unordered_map<std::wstring_view, ContentKind>;

const ExtensionTable& extensionTable() {
    // Built once only. The keys point at static literals, so they stay valid
    // for the whole life of the process.
    static const ExtensionTable table = [] {
        ExtensionTable t;
        auto add = [&t](std::initializer_list<const wchar_t*> extensions, ContentKind kind) {
            for (const wchar_t* extension : extensions) t.emplace(extension, kind);
        };

        add({L"txt", L"text", L"md", L"markdown", L"rst", L"adoc", L"asciidoc",
             L"csv", L"tsv", L"log", L"ini", L"cfg", L"conf", L"toml", L"yaml",
             L"yml", L"json", L"jsonl", L"ndjson", L"properties", L"env",
             L"tex", L"bib", L"srt", L"vtt", L"sbv", L"nfo", L"me", L"1st",
             L"eml", L"msg", L"vcf", L"ics"},
            ContentKind::PlainText);

        add({L"c", L"h", L"cc", L"cpp", L"cxx", L"c++", L"hpp", L"hh", L"hxx",
             L"inl", L"cs", L"java", L"kt", L"kts", L"py", L"pyi", L"pyw",
             L"rb", L"php", L"js", L"jsx", L"mjs", L"cjs", L"ts", L"tsx",
             L"go", L"rs", L"swift", L"m", L"mm", L"scala", L"clj", L"cljs",
             L"lua", L"pl", L"pm", L"sh", L"bash", L"zsh", L"fish", L"ps1",
             L"psm1", L"psd1", L"bat", L"cmd", L"r", L"jl", L"dart", L"vue",
             L"svelte", L"gradle", L"cmake", L"mk", L"make", L"proto",
             L"graphql", L"gql", L"asm", L"s", L"vb", L"vbs", L"f", L"f90",
             L"pas", L"ada", L"erl", L"ex", L"exs", L"hs", L"ml", L"nim",
             L"zig", L"sql", L"sv", L"vhd", L"v"},
            ContentKind::Code);

        add({L"html", L"htm", L"xhtml", L"xml", L"svg", L"xsl", L"xslt",
             L"plist", L"resx", L"csproj", L"vcxproj", L"vbproj", L"xaml",
             L"rss", L"atom", L"opf"},
            ContentKind::Markup);

        add({L"pdf"}, ContentKind::Pdf);
        add({L"docx", L"docm", L"xlsx", L"xlsm", L"pptx", L"pptm"}, ContentKind::OfficeXml);
        add({L"doc", L"xls", L"ppt"}, ContentKind::OfficeLegacy);
        add({L"odt", L"ods", L"odp", L"odg", L"fodt"}, ContentKind::OpenDocument);
        add({L"rtf"}, ContentKind::RichText);
        add({L"epub", L"fb2"}, ContentKind::Ebook);

        return t;
    }();
    return table;
}

// Folders whose content is generated, downloaded or owned by the system:
// indexing it means paying I/O for text the user never wrote and will never
// look for. This is the filter that moves the needle most.
bool isExcludedDirectoryName(const wchar_t* lowered, size_t length) {
    static const wchar_t* const kExcluded[] = {
        // package and dependency managers
        L"node_modules", L"bower_components", L".pnpm-store", L".yarn", L".npm",
        L".nuget", L".cargo", L".rustup", L".m2", L".gradle", L".stack-work",
        L"site-packages", L"dist-packages", L"vendor", L"packages",
        L"typeshed-fallback", L"bundled",
        // editor extensions: hundreds of thousands of third-party files inside
        // the user profile, outside AppData
        L".vscode", L".vscode-server", L".cursor", L".codex", L".windsurf",
        // version control and tool caches
        L".git", L".svn", L".hg", L"__pycache__", L".mypy_cache", L".pytest_cache",
        L".ruff_cache", L".tox", L".venv", L"venv", L".virtualenvs", L".cache",
        L".parcel-cache", L".turbo", L".vs", L".idea", L".gradle-cache",
        // build output
        L"cmake-build-debug", L"cmake-build-release", L".next", L".nuxt",
        L".svelte-kit", L".angular", L"obj",
        // system
        L"$recycle.bin", L"system volume information", L"windows", L"winsxs",
        L"windowsapps", L"$windows.~bt", L"$windows.~ws", L"$sysreset",
        L"recovery", L"perflogs", L"msocache", L"onedrivetemp",
        // Application data. This is the exclusion that weighs the most: this
        // is where browser extensions, editor internals and package manager
        // caches live. Windows Search makes the same choice by default: it
        // indexes the user profile BUT skips AppData.
        L"appdata", L"programdata", L"program files", L"program files (x86)",
        L"application data", L"local settings",
    };

    for (const wchar_t* candidate : kExcluded) {
        size_t candidateLength = 0;
        while (candidate[candidateLength]) ++candidateLength;
        if (candidateLength != length) continue;  // length prefilter
        size_t i = 0;
        while (i < length && candidate[i] == lowered[i]) ++i;
        if (i == length) return true;
    }
    return false;
}

enum : uint8_t { TreeUnknown = 0, TreeIncluded = 1, TreeExcluded = 2 };

}  // namespace

ContentKind classifyByName(std::wstring_view name) {
    const std::wstring_view extension = extensionOf(name);
    if (extension.empty() || extension.size() >= kMaxTokenChars) return ContentKind::Skip;

    wchar_t buffer[kMaxTokenChars];
    const size_t length = lowerInto(extension, buffer);

    const auto& table = extensionTable();
    const auto it = table.find(std::wstring_view(buffer, length));
    return it == table.end() ? ContentKind::Skip : it->second;
}

std::vector<uint32_t> selectCandidates(const FileIndex& index, SelectionStats* stats) {
    const auto start = std::chrono::steady_clock::now();
    const size_t count = index.size();

    stats->total = count;

    // The "descends from an excluded folder" state, memoized.
    //
    // The question has to be asked for every file, but the answer is the same
    // for every file in the same folder. Walking up the tree for each one
    // would cost millions of repeated hops; here each chain is walked once and
    // the verdict is propagated back over the whole chain.
    std::vector<uint8_t> treeState(count, TreeUnknown);
    std::vector<uint32_t> chain;
    chain.reserve(64);

    wchar_t buffer[kMaxTokenChars];

    for (size_t i = 0; i < count; ++i) {
        if (treeState[i] != TreeUnknown) continue;

        chain.clear();
        uint32_t current = static_cast<uint32_t>(i);
        uint8_t resolved = TreeIncluded;

        for (int depth = 0; depth < 512; ++depth) {
            if (treeState[current] != TreeUnknown) {
                resolved = treeState[current];
                break;
            }
            chain.push_back(current);

            const bool isDirectory =
                (index.attributes(current) & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (isDirectory) {
                const size_t length = lowerInto(index.name(current), buffer);
                if (isExcludedDirectoryName(buffer, length)) {
                    resolved = TreeExcluded;
                    break;
                }
            }

            const uint32_t parent = index.parent(current);
            if (parent == kNoParent) {
                resolved = TreeIncluded;
                break;
            }
            current = parent;
        }

        for (const uint32_t slot : chain) treeState[slot] = resolved;
    }

    std::vector<uint32_t> candidates;
    candidates.reserve(count / 8);

    for (size_t i = 0; i < count; ++i) {
        if (index.isDeleted(i)) {
            ++stats->byReason[static_cast<size_t>(SkipReason::Deleted)];
            continue;
        }

        const uint32_t attributes = index.attributes(i);
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
            ++stats->byReason[static_cast<size_t>(SkipReason::IsDirectory)];
            continue;
        }
        if (treeState[i] == TreeExcluded) {
            ++stats->byReason[static_cast<size_t>(SkipReason::ExcludedDirectory)];
            continue;
        }

        const std::wstring_view name = index.name(i);
        if (extensionOf(name).empty()) {
            ++stats->byReason[static_cast<size_t>(SkipReason::NoExtension)];
            continue;
        }

        const ContentKind kind = classifyByName(name);
        if (kind == ContentKind::Skip) {
            ++stats->byReason[static_cast<size_t>(SkipReason::UnknownExtension)];
            continue;
        }

        ++stats->byKind[static_cast<size_t>(kind)];
        candidates.push_back(static_cast<uint32_t>(i));
    }

    stats->candidates = candidates.size();
    stats->elapsedSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return candidates;
}

double contentRankPenalty(const std::wstring& path) {
    // The value is calibrated on the real corpus, where bm25 scores run
    // roughly from -6 to -26: four points are enough to push a log below a
    // relevant document, without putting it out of reach.
    constexpr double kGenerated = 4.0;

    wchar_t lowered[512];
    const size_t length = path.size() < 512 ? path.size() : 511;
    for (size_t i = 0; i < length; ++i) lowered[i] = lowerFast(path[i]);
    const std::wstring_view view(lowered, length);

    // Tool folders holding transcripts and run journals.
    for (const wchar_t* marker : {L"\\.claude\\", L"\\.codex\\", L"\\.cursor\\",
                                  L"\\logs\\", L"\\log\\", L"\\.cache\\"}) {
        if (view.find(marker) != std::wstring_view::npos) return kGenerated;
    }

    // Vendor boilerplate shipped inside applications and games. Legally it is
    // prose, and a similarity search treats it as such: a query about contract
    // penalties retrieved fourteen copies of the same store terms in fourteen
    // languages, because that is genuinely the nearest text on the disk. It is
    // never what the user meant.
    for (const wchar_t* marker : {L"\\xboxgames\\", L"\\steamapps\\",
                                  L"\\content\\data\\store\\", L"\\epic games\\",
                                  L"\\gog galaxy\\", L"\\battle.net\\",
                                  L"\\licenses\\", L"\\third_party\\",
                                  L"\\third-party\\", L"\\thirdparty\\"}) {
        if (view.find(marker) != std::wstring_view::npos) return kGenerated;
    }

    // Build output. The folder exclusion list cannot catch these: a folder
    // named "build" is often a legitimate source directory, so excluding it
    // outright would lose real work. These sub-paths are unambiguous.
    //
    // The semantic search is what made this matter. A copied index.html under
    // build/intermediates has no meaning of its own, but it has enough words to
    // sit 0.6 away from almost any query — and it ranked FIRST for "risarcimento
    // danni" until it started being penalized.
    for (const wchar_t* marker : {L"\\build\\intermediates\\", L"\\build\\outputs\\",
                                  L"\\build\\generated\\", L"\\build\\tmp\\",
                                  L"\\target\\classes\\", L"\\bin\\debug\\",
                                  L"\\bin\\release\\", L"\\obj\\debug\\",
                                  L"\\obj\\release\\", L"\\mergedebugassets\\",
                                  L"\\mergereleaseassets\\", L"\\dist\\assets\\",
                                  L"\\.output\\", L"\\coverage\\", L"\\build\\src\\",
                                  L"\\build\\lib\\", L"\\build\\static\\"}) {
        if (view.find(marker) != std::wstring_view::npos) return kGenerated;
    }

    // Extensions that mean an event stream or a generated artifact, not a
    // document a person wrote.
    for (const wchar_t* suffix : {L".jsonl", L".log", L".ndjson",
                                  L".min.js", L".min.css", L".map"}) {
        const size_t length2 = std::wstring_view(suffix).size();
        if (view.size() >= length2 &&
            view.compare(view.size() - length2, length2, suffix) == 0) {
            return kGenerated;
        }
    }
    return 0.0;
}

bool needsExtractor(ContentKind kind) {
    switch (kind) {
        case ContentKind::Pdf:
        case ContentKind::OfficeXml:
        case ContentKind::OfficeLegacy:
        case ContentKind::OpenDocument:
        case ContentKind::RichText:
        case ContentKind::Ebook:
            return true;
        default:
            // Markup counts as plain enough: stripping tags is a pass over the
            // bytes, not a parser, and the file on disk still holds the text.
            return false;
    }
}

const wchar_t* kindName(ContentKind kind) {
    switch (kind) {
        case ContentKind::Skip:         return L"skipped";
        case ContentKind::PlainText:    return L"text";
        case ContentKind::Code:         return L"code";
        case ContentKind::Markup:       return L"markup";
        case ContentKind::Pdf:          return L"pdf";
        case ContentKind::OfficeXml:    return L"office (ooxml)";
        case ContentKind::OfficeLegacy: return L"office (legacy)";
        case ContentKind::OpenDocument: return L"opendocument";
        case ContentKind::RichText:     return L"rtf";
        case ContentKind::Ebook:        return L"ebook";
    }
    return L"?";
}

const wchar_t* reasonName(SkipReason reason) {
    switch (reason) {
        case SkipReason::None:              return L"-";
        case SkipReason::IsDirectory:       return L"folders";
        case SkipReason::Deleted:           return L"tombstones";
        case SkipReason::NoExtension:       return L"no extension";
        case SkipReason::UnknownExtension:  return L"extension not indexable";
        case SkipReason::ExcludedDirectory: return L"in an excluded folder";
        case SkipReason::SystemOrHidden:    return L"system or hidden";
    }
    return L"?";
}

}  // namespace filo
