#include "reclaim/classify.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <unordered_set>

#include "reclaim/paths.h"
#include "search/matcher.h"

namespace filo {
namespace {

constexpr int64_t kTicksPerDay = 86'400LL * 10'000'000LL;

// An installer that has been sitting for this long has done its job or never
// will. Six months is long enough that "I might still need to run it" has
// stopped being true for almost everything.
constexpr int64_t kInstallerAgeDays = 180;

// Something in Downloads untouched for this long was consumed or forgotten.
constexpr int64_t kDownloadAgeDays = 120;

// A generated tree is offered only when nothing else in the project has moved
// for this long. Below it the project is live, and taking its build away costs
// the next working day a reinstall for nothing.
//
// The measurement errs towards keeping, which is the direction to err in. A git
// clone stamps every file with the time of the clone, so a project pulled down
// yesterday and abandoned for ten years reads as live and is not offered. That
// mistake costs disk space; the opposite one costs a working tree.
constexpr int64_t kProjectDormantDays = 180;

bool contains(const std::wstring& haystack, const wchar_t* needle) {
    return haystack.find(needle) != std::wstring::npos;
}

bool endsWith(const std::wstring& text, const wchar_t* suffix) {
    const std::wstring tail = suffix;
    return text.size() >= tail.size() &&
           text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

bool hasExtension(const std::wstring& lowerPath, std::initializer_list<const wchar_t*> list) {
    for (const wchar_t* extension : list) {
        if (endsWith(lowerPath, extension)) return true;
    }
    return false;
}

int64_t ageInDays(int64_t mtime, int64_t now) {
    if (mtime == 0 || now <= mtime) return 0;
    return (now - mtime) / kTicksPerDay;
}

// "45 days", "14 months". A number of days stops telling a reader anything once
// there are a lot of them.
std::wstring spanOfDays(int64_t days) {
    if (days <= 0) return L"less than a day";
    if (days < 60) return std::to_wstring(days) + L" days";
    return std::to_wstring(days / 30) + L" months";
}

// 41203 and 412030 look the same at a glance, and the file count is the whole
// argument for offering a tree rather than a file.
std::wstring grouped(uint64_t value) {
    std::wstring digits = std::to_wstring(value);
    for (size_t at = digits.size(); at > 3;) {
        at -= 3;
        digits.insert(at, 1, L',');
    }
    return digits;
}

// The folder a rule is really about, for saying it back to the user. "inside
// node_modules" is a reason; the full path is not.
std::wstring folderMentioned(const std::wstring& lowerPath,
                             std::initializer_list<const wchar_t*> markers) {
    for (const wchar_t* marker : markers) {
        if (contains(lowerPath, marker)) {
            std::wstring name = marker;
            while (!name.empty() && name.front() == L'\\') name.erase(name.begin());
            while (!name.empty() && name.back() == L'\\') name.pop_back();
            return name;
        }
    }
    return {};
}

// --- caches the whole machine shares ----------------------------------------
//
// These used to sit in the same list as build output, and they are not the same
// thing. A pnpm store is a farm of hardlinks that every pnpm project on the
// disk points into: removing it does not free one project's output, it breaks
// all of them at once and costs a full re-download. The same holds for a Cargo
// registry, a Maven repository and the rest.
std::wstring sharedCacheMentioned(const std::wstring& lowerPath) {
    return folderMentioned(lowerPath,
                           {L"\\.pnpm-store\\", L"\\.cargo\\registry\\",
                            L"\\.m2\\repository\\", L"\\.nuget\\packages\\",
                            L"\\.gradle\\caches\\", L"\\.yarn\\cache\\",
                            L"\\pip\\cache\\", L"\\ms-playwright\\"});
}

// The tool's own command for emptying its own cache, where it has one. Naming
// it is the difference between "not here" and "here is where".
//
// Gradle, Maven and Playwright are absent because they have no prune command of
// their own, and inventing one for the table would send people to a shell to
// run something that does not exist.
const wchar_t* pruneCommand(const std::wstring& cache) {
    if (cache == L".pnpm-store")      return L"pnpm store prune";
    if (cache == L".cargo\\registry") return L"cargo cache";
    if (cache == L".yarn\\cache")     return L"yarn cache clean";
    if (cache == L"pip\\cache")       return L"pip cache purge";
    if (cache == L".nuget\\packages") return L"dotnet nuget locals all --clear";
    return nullptr;
}

void describeSharedCache(const std::wstring& cache, Classified& item) {
    item.kind = SpaceClass::SharedCache;
    const wchar_t* prune = pruneCommand(cache);
    if (prune != nullptr) {
        item.reason = L"every project here points into it — empty it with ";
        item.reason += prune;
    } else {
        item.reason = L"every project here points into it — empty it from the tool, "
                      L"not from here";
    }
}

// --- what a tool generated --------------------------------------------------
//
// A directory a tool fills in, and how far above it the project starts.
//
// The distance differs per pattern, and that is why this is a table and not a
// list of names. node_modules sits directly in the project, so the project is
// one level up. target\debug is two levels down, and stopping at target would
// call one Rust crate two separate projects and then test the wrong folder for
// dormancy.
struct GeneratedPattern {
    const wchar_t* leaf;      // the directory's own name
    const wchar_t* under;     // the name its parent must have, or nullptr
    unsigned       levelsUp;  // from the leaf to the project root
};

constexpr GeneratedPattern kGeneratedPatterns[] = {
    {L"node_modules",  nullptr,   1},
    {L"__pycache__",   nullptr,   1},
    {L".next",         nullptr,   1},
    {L"debug",         L"target", 2},
    {L"release",       L"target", 2},
    {L"intermediates", L"build",  2},
    {L"outputs",       L"build",  2},
    {L"assets",        L"dist",   2},
};

constexpr size_t kGeneratedPatternCount =
    sizeof(kGeneratedPatterns) / sizeof(kGeneratedPatterns[0]);

bool nameIs(const FileIndex& index, uint32_t slot, const wchar_t* wanted) {
    const std::wstring_view name = index.name(slot);
    size_t i = 0;
    for (; i < name.size(); ++i) {
        if (wanted[i] == L'\0') return false;
        if (lowerFast(name[i]) != lowerFast(wanted[i])) return false;
    }
    return wanted[i] == L'\0';
}

// Which pattern this directory matches, as 1 + its position, or 0 for none.
// Kept to a byte because there is one of these per record on the volume.
uint8_t patternOf(const FileIndex& index, uint32_t slot) {
    const size_t count = index.size();
    const uint32_t up = index.parent(slot);
    for (size_t p = 0; p < kGeneratedPatternCount; ++p) {
        const GeneratedPattern& pattern = kGeneratedPatterns[p];
        if (!nameIs(index, slot, pattern.leaf)) continue;
        if (pattern.under == nullptr) return static_cast<uint8_t>(p + 1);
        if (up == kNoParent || up >= count) continue;
        if (nameIs(index, up, pattern.under)) return static_cast<uint8_t>(p + 1);
    }
    return 0;
}

// One generated tree, added up.
struct GeneratedTree {
    uint32_t slot = 0;          // the generated directory: the thing to remove
    uint64_t bytes = 0;
    uint32_t files = 0;
    int64_t  lastTouched = 0;   // newest mtime in the project, this tree aside
};

struct GeneratedTrees {
    std::vector<GeneratedTree> trees;

    // Per record: 1 + the pattern it matches, 0 for everything else. The file
    // loop reads it to answer "is this file already inside a tree we reported",
    // which is what keeps a single binary out of node_modules' place in the
    // list.
    std::vector<uint8_t> patternAt;
};

// Every outermost generated directory on the volume, with its size, its file
// count, and when the project around it was last worked on.
//
// This reads no disk. collectFileFacts already measured every file on the
// volume and buildChildIndex already has the shape of the tree, so all of it is
// a walk over memory that is sitting there anyway.
GeneratedTrees findGeneratedTrees(const FileIndex& index,
                                  const std::vector<FileFact>& facts) {
    const size_t count = index.size();
    GeneratedTrees found;
    found.patternAt.assign(count, 0);
    if (count == 0) return found;

    const ChildIndex children = buildChildIndex(index);

    // slot -> position in `facts`, so a subtree walk can read a size and an
    // mtime without asking the file system anything.
    std::vector<uint32_t> factAt(count, kNoParent);
    for (size_t i = 0; i < facts.size(); ++i) {
        if (facts[i].slot < count) factAt[facts[i].slot] = static_cast<uint32_t>(i);
    }

    for (size_t i = 0; i < count; ++i) {
        if (index.isDeleted(i)) continue;
        const uint32_t attributes = index.attributes(i);
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        found.patternAt[i] = patternOf(index, static_cast<uint32_t>(i));
    }

    // Sums a subtree out of the child index. An explicit stack rather than
    // recursion: node_modules nests as deep as the dependency graph does.
    std::vector<uint32_t> stack;
    auto walk = [&](uint32_t from, bool skipGenerated, uint64_t* bytes,
                    uint32_t* files, int64_t* newest) {
        stack.clear();
        stack.push_back(from);
        while (!stack.empty()) {
            const uint32_t dir = stack.back();
            stack.pop_back();
            for (uint32_t k = children.offset[dir]; k < children.offset[dir + 1]; ++k) {
                const uint32_t child = children.child[k];
                if (index.isDeleted(child)) continue;
                const uint32_t attributes = index.attributes(child);
                if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
                    // A junction is a second name for a folder counted
                    // elsewhere. Descending into one inflates every total.
                    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                    if (skipGenerated && found.patternAt[child] != 0) continue;
                    stack.push_back(child);
                    continue;
                }
                const uint32_t at = factAt[child];
                if (at == kNoParent) continue;   // unmeasured: cloud, gone, denied
                if (bytes != nullptr) *bytes += facts[at].size;
                if (files != nullptr) ++*files;
                if (newest != nullptr && facts[at].mtime > *newest) {
                    *newest = facts[at].mtime;
                }
            }
        }
    };

    // Two trees often share one project — target\debug beside target\release, a
    // .next beside a node_modules — and the dormancy walk is the expensive
    // half. Once per project root is enough.
    std::unordered_map<uint32_t, int64_t> touchedByRoot;

    for (size_t i = 0; i < count; ++i) {
        if (found.patternAt[i] == 0) continue;

        // Only the outermost one. A dist\assets inside node_modules, or a crate
        // vendored into one, is already covered by the tree above it: reporting
        // both would offer the same bytes twice and offer half a tree.
        bool nested = false;
        for (uint32_t up = index.parent(i); up != kNoParent && up < count;
             up = index.parent(up)) {
            if (found.patternAt[up] != 0) { nested = true; break; }
        }
        if (nested) continue;

        const GeneratedPattern& rule = kGeneratedPatterns[found.patternAt[i] - 1];
        uint32_t root = static_cast<uint32_t>(i);
        for (unsigned step = 0; step < rule.levelsUp; ++step) {
            const uint32_t up = index.parent(root);
            // A tree can sit closer to the volume root than its pattern is
            // deep. Stopping short still produces a row, and a row that reads
            // as live is the safe end of that: with no row at all, every file
            // inside the tree goes back to being offered one at a time.
            if (up == kNoParent || up >= count) break;
            root = up;
        }

        GeneratedTree tree;
        tree.slot = static_cast<uint32_t>(i);
        walk(tree.slot, /*skipGenerated=*/false, &tree.bytes, &tree.files, nullptr);
        if (tree.files == 0) continue;   // nothing measurable under it

        const auto known = touchedByRoot.find(root);
        if (known != touchedByRoot.end()) {
            tree.lastTouched = known->second;
        } else {
            // The project WITHOUT its generated trees. Their own mtimes are the
            // time of the last install, which changes whether or not anybody
            // did any work, so counting them would mark every project on the
            // disk live forever.
            int64_t newest = 0;
            walk(root, /*skipGenerated=*/true, nullptr, nullptr, &newest);
            touchedByRoot.emplace(root, newest);
            tree.lastTouched = newest;
        }

        found.trees.push_back(tree);
    }
    return found;
}

// Emulator scratch, which the AppState gate carves out and the classification
// below then names. Narrow on purpose: see the gate.
bool isEmulatorScratch(const std::wstring& lower) {
    return contains(lower, L"\\.android\\avd\\") &&
           (contains(lower, L"\\snapshots\\") || endsWith(lower, L"ram.img") ||
            endsWith(lower, L"cache.img") || endsWith(lower, L"hardware-qemu.ini") ||
            endsWith(lower, L".img.qcow2"));
}

}  // namespace

const wchar_t* spaceClassName(SpaceClass value) {
    switch (value) {
        case SpaceClass::Regenerable:   return L"Rebuilt by a command";
        case SpaceClass::Duplicate:     return L"You have another copy";
        case SpaceClass::InstallerUsed: return L"Installers you already ran";
        case SpaceClass::MachineOutput: return L"Written by a program";
        case SpaceClass::Downloaded:    return L"Downloaded and left there";
        case SpaceClass::ProjectInUse:  return L"Build output you still use";
        case SpaceClass::SharedCache:   return L"A cache all your projects share";
        case SpaceClass::Media:         return L"Large media";
        case SpaceClass::Personal:      return L"Your own files";
        case SpaceClass::AppState:      return L"An app's own storage";
        case SpaceClass::NotYours:      return L"Another account or Windows";
        default:                        return L"Unclassified";
    }
}

bool isOfferable(SpaceClass value) {
    switch (value) {
        case SpaceClass::Regenerable:
        case SpaceClass::Duplicate:
        case SpaceClass::InstallerUsed:
        case SpaceClass::MachineOutput:
        case SpaceClass::Downloaded:
            return true;
        default:
            // Media is deliberately not offerable. A 4 GB video is not waste
            // because it is big, and a tool that ticks it by default is a tool
            // that eats somebody's wedding footage.
            //
            // ProjectInUse and SharedCache are that same rule applied to build
            // output. The bytes really are regenerable in both cases; what is
            // missing is the right to decide. One belongs to a project somebody
            // is still working on, the other to every project on the machine at
            // once. Shown, so the disk adds up. Never ticked.
            return false;
    }
}

std::vector<Classified> classifyBiggest(const FileIndex& index,
                                        const std::vector<FileFact>& facts,
                                        const std::vector<DuplicateGroup>& duplicates,
                                        size_t topN, int64_t now, ClassifyStats* stats) {
    const auto start = std::chrono::steady_clock::now();
    *stats = ClassifyStats{};

    // Files that already have an identical copy. That is a stronger reason than
    // anything a path can offer, so it wins over every other rule below.
    std::unordered_set<uint32_t> hasCopy;
    for (const DuplicateGroup& group : duplicates) {
        for (size_t k = 1; k < group.slots.size(); ++k) hasCopy.insert(group.slots[k]);
    }

    // Found by walking the tree, not by ranking sizes, because ranking cannot
    // find them: a 1.5 GB node_modules is 40,000 small files and not one of
    // them is big enough to reach a list of the biggest files on the disk. The
    // category Filo is surest about was the one it under-counted most.
    const GeneratedTrees generated = findGeneratedTrees(index, facts);

    std::vector<const FileFact*> biggest;
    biggest.reserve(facts.size());
    for (const FileFact& fact : facts) biggest.push_back(&fact);

    const size_t keep = std::min(topN, biggest.size());
    std::partial_sort(biggest.begin(), biggest.begin() + static_cast<ptrdiff_t>(keep),
                      biggest.end(), [](const FileFact* a, const FileFact* b) {
                          return a->size > b->size;
                      });
    biggest.resize(keep);

    wchar_t profileBuffer[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"USERPROFILE", profileBuffer, MAX_PATH);
    const std::wstring profile =
        (length > 0 && length < MAX_PATH) ? toLowerCopy(profileBuffer) : std::wstring();

    std::vector<Classified> out;
    out.reserve(keep + generated.trees.size());

    // The gates below return early, so the accounting cannot stay at the bottom
    // of the loop: a file that left through a gate would be classified and then
    // never counted, and the totals on the screen would not add up to the disk.
    auto record = [&](Classified& item) {
        const size_t which = static_cast<size_t>(item.kind);
        stats->bytesByClass[which] += item.size;
        ++stats->filesByClass[which];
        if (isOfferable(item.kind)) stats->offerableBytes += item.size;
        ++stats->examined;
        out.push_back(std::move(item));
    };

    // --- the ownership gates ------------------------------------------------
    //
    // Shared by files and by whole trees, because a node_modules under AppData
    // is no more yours to delete than one file inside it is. Returns true when
    // ownership settled the question, in which case the row is already
    // recorded and the caller must stop.
    auto ownershipSettled = [&](const std::wstring& lower, Classified& item) {
        // --- gate one: is it yours at all? ----------------------------------
        //
        // This has to come FIRST, and it did not. Everything after it is a
        // DESCRIPTION of what a file is, and descriptions were being allowed to
        // decide ownership: a .log under C:\Windows\Logs matched "written by a
        // program" three branches before anything asked whose file it was, and
        // came out offerable. Windows logs, service data, another account's
        // documents — all reachable, because one if/else chain was doing two
        // unrelated jobs and the first answer won.
        //
        // Whose file it is is not one opinion among several. It is the question
        // that decides whether the others get asked.
        if (!profile.empty() && !insideFolder(lower, profile)) {
            item.kind = SpaceClass::NotYours;
            item.reason = L"outside your user folder — Windows or another account";
            record(item);
            return true;
        }

        // --- gate two: yours, but not yours to move -------------------------
        //
        // A WSL disk, a Docker volume, a browser profile: your work is in
        // there, but the application decides where it lives and will not find
        // it anywhere else. Removing it belongs to that app, usually through
        // its own uninstall.
        //
        // The one carve-out is emulator scratch, and it is narrow on purpose. A
        // saved RAM snapshot is genuinely rebuilt the next time the emulator
        // starts, and the first run put an 11 GB two-year-old one under "your
        // own files". Note what the carve-out does NOT cover: userdata-qemu.img
        // holds the apps and data installed in the emulator, so it falls
        // through to here and stays where it is.
        if (!isEmulatorScratch(lower) &&
            (contains(lower, L"\\appdata\\") || contains(lower, L"\\.cache\\") ||
             contains(lower, L"\\.local\\share\\") || contains(lower, L"\\.ollama\\") ||
             contains(lower, L"\\.android\\"))) {
            item.kind = SpaceClass::AppState;
            item.reason = L"an application keeps this here — remove it from inside "
                          L"that app, not from the file system";
            record(item);
            return true;
        }
        return false;
    };

    // --- whole trees --------------------------------------------------------
    //
    // Before the files, because the file loop has to know which files are
    // already spoken for.
    for (const GeneratedTree& tree : generated.trees) {
        const std::wstring lower = toLowerCopy(index.fullPath(tree.slot));

        Classified item;
        item.slot = tree.slot;
        item.size = tree.bytes;
        item.mtime = tree.lastTouched;
        item.files = tree.files;

        if (ownershipSettled(lower, item)) continue;

        // A generated directory can sit inside a shared store — a package that
        // vendors its own node_modules, say. The store owns it, not a project.
        const std::wstring cache = sharedCacheMentioned(lower);
        if (!cache.empty()) {
            describeSharedCache(cache, item);
            record(item);
            continue;
        }

        const int64_t idle = ageInDays(tree.lastTouched, now);
        const std::wstring howMany = grouped(tree.files) + L" files";

        // A last-touched of zero means nothing outside the tree could be dated:
        // no evidence the project is finished, so it is not offered. Same
        // direction as every other guess here.
        if (tree.lastTouched != 0 && idle >= kProjectDormantDays) {
            item.kind = SpaceClass::Regenerable;
            // NOT "your tools recreate this", which is what this used to say
            // and is a promise nobody can keep. Getting the tree back needs the
            // lockfile, the network, the versions still being published, the
            // private registry still letting you in, and native modules that
            // still compile against today's compiler. One line cannot list all
            // of that; it can at least stop claiming the opposite.
            item.reason = howMany + L", untouched for " + spanOfDays(idle) +
                          L" — a reinstall needs the lockfile, the network and a "
                          L"working toolchain";
        } else {
            item.kind = SpaceClass::ProjectInUse;
            item.reason = tree.lastTouched == 0
                              ? howMany + L" — nothing beside it carries a date to "
                                          L"judge it by"
                              : howMany + L" — this project was worked on " +
                                    spanOfDays(idle) + L" ago";
        }
        record(item);
    }

    // --- and then the biggest files -----------------------------------------
    const size_t records = index.size();
    auto insideGeneratedTree = [&](uint32_t slot) {
        for (uint32_t up = index.parent(slot); up != kNoParent && up < records;
             up = index.parent(up)) {
            if (generated.patternAt[up] != 0) return true;
        }
        return false;
    };

    for (const FileFact* fact : biggest) {
        // Inside a tree that already went out as one row. Deliberately not
        // classified and NOT counted: its bytes are in that row, and adding
        // them again would have the screen promise more space than the disk
        // has.
        //
        // This is also the rule that stops Filo offering one binary out of
        // node_modules. npm will not repair a package directory that already
        // exists, so removing part of a tree leaves a project that needs a
        // clean reinstall, while removing all of it leaves one that installs.
        if (insideGeneratedTree(fact->slot)) continue;

        const std::wstring lower = toLowerCopy(index.fullPath(fact->slot));

        Classified item;
        item.slot = fact->slot;
        item.size = fact->size;
        item.mtime = fact->mtime;
        const int64_t days = ageInDays(fact->mtime, now);

        if (ownershipSettled(lower, item)) continue;

        // --- now, and only now, what kind of file is it? --------------------
        //
        // Each test below is stronger than the ones after it. The first one to
        // fire wins, so a duplicate inside a shared cache is reported as the
        // cache — the more useful of the two truths.

        const std::wstring cache = sharedCacheMentioned(lower);

        if (!cache.empty()) {
            describeSharedCache(cache, item);
        } else if (isEmulatorScratch(lower)) {
            item.kind = SpaceClass::Regenerable;
            item.reason = L"Android emulator state — recreated when the emulator starts";
        } else if (hasCopy.count(fact->slot)) {
            item.kind = SpaceClass::Duplicate;
            item.reason = L"an identical copy exists elsewhere on this disk";
        } else if (hasExtension(lower, {L".log", L".dmp", L".etl", L".jsonl", L".ndjson"}) ||
                   contains(lower, L"\\logs\\") || contains(lower, L"\\crashdumps\\")) {
            item.kind = SpaceClass::MachineOutput;
            item.reason = L"a log or a crash dump, written by a program";
        } else if (hasExtension(lower, {L".exe", L".msi", L".msix"}) &&
                   contains(lower, L"\\downloads\\") && days >= kInstallerAgeDays) {
            item.kind = SpaceClass::InstallerUsed;
            item.reason = L"an installer downloaded " + std::to_wstring(days / 30) +
                          L" months ago";
        } else if (contains(lower, L"\\downloads\\") && days >= kDownloadAgeDays) {
            item.kind = SpaceClass::Downloaded;
            item.reason = L"in Downloads, untouched for " + std::to_wstring(days / 30) +
                          L" months";
        } else if (hasExtension(lower, {L".mp4", L".mkv", L".mov", L".avi", L".wmv",
                                        L".webm", L".mp3", L".flac", L".wav", L".iso"})) {
            item.kind = SpaceClass::Media;
            item.reason = L"large media — only you know whether you still want it";
        } else {
            item.kind = SpaceClass::Personal;
            item.reason = days > 0 ? L"last changed " + std::to_wstring(days) + L" days ago"
                                   : L"changed recently";
        }

        record(item);
    }

    // The two halves are found by different means — trees by walking, files by
    // ranking — so they arrive in two runs, each sorted only within itself. The
    // screen prints the first rows of a category and stops; without this the
    // biggest tree on the disk could sit below a hundred files a thousandth its
    // size and never be seen at all.
    std::stable_sort(out.begin(), out.end(),
                     [](const Classified& a, const Classified& b) {
                         return a.size > b.size;
                     });

    stats->elapsedSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return out;
}

}  // namespace filo
