#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "index/file_index.h"
#include "reclaim/duplicates.h"
#include "reclaim/scan.h"

namespace filo {

// What something on the disk IS, as far as deciding whether it can go.
//
// Ordered by how confidently it can be offered. The order matters more than the
// names: it is what the screen sorts by, and a category can only ever be
// presented above one it is more certain than.
enum class SpaceClass : uint8_t {
    Regenerable = 0,   // a whole generated tree, in a project nobody still uses
    Duplicate,         // an identical copy exists elsewhere on this disk
    InstallerUsed,     // an installer old enough that it has served its purpose
    MachineOutput,     // logs, dumps, crash reports
    Downloaded,        // sitting in Downloads, untouched for a long time
    ProjectInUse,      // the same generated tree, in a project still being worked on
    SharedCache,       // a package store every project on the machine points into
    Media,             // big video or audio: not waste, just large
    Personal,          // a document someone wrote. Never offered.
    AppState,          // an application's own storage: yours, but not yours to move
    NotYours,          // another user or the system itself
    Count
};

const wchar_t* spaceClassName(SpaceClass value);

// Whether things of this class may be offered for removal at all.
//
// Personal, NotYours, ProjectInUse and SharedCache are listed so the totals add
// up and so the user can see where their disk actually went — never with a
// checkbox. A screen that lets you tick your own thesis is not a cleanup tool,
// and one that offers the build of the project you worked on this morning is
// not much better.
bool isOfferable(SpaceClass value);

// One row on the space screen: usually a file, sometimes a whole tree.
//
// A generated tree arrives here as ONE row whose slot is the generated
// DIRECTORY — node_modules itself, not the project around it and not a file
// inside it. Anything that turns a slot into a path to remove has to keep that
// straight: the project root holds the source, and removing it is the accident
// this whole shape exists to prevent.
struct Classified {
    uint32_t slot = 0;
    uint64_t size = 0;     // for a tree, everything underneath it
    int64_t  mtime = 0;    // for a tree, when the project was last touched
    SpaceClass kind = SpaceClass::Personal;

    // How many files this row stands for. One for a file; for a tree, the count
    // underneath — which is the number the user needs, because 1.5 GB in 41,000
    // files is the case that never reaches a list of biggest files.
    //
    // NOT the same as ClassifyStats::filesByClass, which counts rows.
    uint32_t files = 1;

    // Why, in the user's words. Stated as a fact when it is one ("inside
    // node_modules") and as an observation when it is not ("not opened since
    // March"), because the difference is what tells them how hard to think.
    std::wstring reason;
};

struct ClassifyStats {
    uint64_t examined = 0;
    uint64_t bytesByClass[static_cast<size_t>(SpaceClass::Count)] = {};
    // Rows, not files: a tree counts once here however many files it holds.
    // The screen uses this to decide how many rows it is not showing.
    uint64_t filesByClass[static_cast<size_t>(SpaceClass::Count)] = {};
    uint64_t offerableBytes = 0;
    double   elapsedSeconds = 0.0;
};

// Classifies the biggest files on the volume, plus every generated tree on it.
//
// Only the biggest files, and that is the point: 100 files hold 44% of this
// disk and 10,000 hold 86%, so a ranked list of a few thousand covers nearly
// everything worth deciding about, while a list of two million covers nothing a
// person can read.
//
// Generated trees are the exception, and they are why `topN` is not the whole
// story. A 1.5 GB node_modules is 40,000 small files with no single one big
// enough to reach a top-N list, so ranking by size finds almost none of the
// category Filo is most sure about. They are found by walking the tree instead,
// aggregated whole, and no file inside one is ever returned on its own.
//
// `duplicates` may be empty; when present it is used to mark files that already
// have an identical copy, which is a stronger reason than anything a path can
// say.
std::vector<Classified> classifyBiggest(const FileIndex& index,
                                        const std::vector<FileFact>& facts,
                                        const std::vector<DuplicateGroup>& duplicates,
                                        size_t topN, int64_t now, ClassifyStats* stats);

}  // namespace filo
