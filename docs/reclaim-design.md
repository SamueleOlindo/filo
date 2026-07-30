# Reclaiming disk space

A design for the one part of Filo that destroys things.

## Why this belongs in Filo and not in a disk cleaner

WinDirStat, TreeSize and CCleaner show you a map of bytes. They know how much a
folder weighs; they do not know what it is. Filo already knows:

- every file on the volume, from the MFT, without walking directories
- which text a person wrote and which a machine generated (`rank_penalty`)
- the XXH3-128 content hash of every indexed document
- what documents are *about*, as vectors

That last pair is the interesting part. A cleaner can tell you that four PDFs in
Downloads take 340 MB. Only Filo can tell you they are the same contract.

Measured on the development machine: **13.3% of all indexed text chunks are byte
identical**, and Downloads contains

    Contratto d'ingaggio Acme srl (6).pdf
    Contratto d'ingaggio Acme srl (7) (2).pdf
    Contratto d'ingaggio Acme srl (7) (2) (2).pdf
    Contratto d'ingaggio Acme srl (7) (2) (3).pdf

## The rule everything hangs on

**Filo never decides. It groups, explains, and waits.**

This is the same rule as the rest of the program — query filters boost and never
exclude, `rank_penalty` demotes and never removes — applied where the stakes are
irreversible. A search that hides the right answer wastes a minute. A cleanup
that hides the right answer loses a file.

Three consequences, and they are not negotiable:

1. **No scalar "safe to delete" score.** A number invites automation, and
   automation is precisely the thing that must not happen here. Categories with
   stated reasons invite judgment instead.
2. **Nothing is ever pre-selected.** The user ticks boxes. An empty selection is
   the only correct default.
3. **Recycle Bin only.** `SHFileOperationW` with `FOF_ALLOWUNDO`. Never
   `DeleteFile`.

## What is missing today

`FileIndex` holds name, FRN, parent and attributes. It has **no size and no
modification time**, and the biggest space consumers — videos, ISOs,
`node_modules` — are not content-indexed at all, so the `doc` table does not
have them either.

So phase 0 is a parallel metadata pass: `GetFileAttributesExW` over every
record, which returns size, mtime and attributes in one call. Cost has to be
measured, not guessed; the expectation is seconds, since the incremental index
pass already does 40,000 of these inside a 2.8 second run.

Memory: 16 bytes per record for size and mtime, about **38 MB on 2.4 million
files**, taking the index from 180 MB to roughly 218 MB. That is the price of
the whole feature and it should be paid consciously.

Note that candidate selection here is the **opposite** of indexing.
`selectCandidates()` excludes `node_modules`, `build`, `.gradle` — exactly the
folders this feature exists to find. It must not be reused.

## Categories, in order of confidence

Confidence decides presentation, not a weight in a formula.

### 0. Only inside the user's own area — learned the hard way

This rule was not in the first version of this document. It came out of running
the thing on a real disk, three times, and watching it propose a different
disaster each time.

Run one offered to delete objects inside `.git/lfs`, files inside the pnpm
content store, and copies inside `node_modules`. All byte-identical; every one
of those deletions breaks something, because a package store keeps one copy per
hash *on purpose* and a git object *is* the repository.

Run two, with those excluded, offered Edge profile data, a vector database's
storage segments, a game launcher's depots — and files belonging to **another
user of the same machine**.

Every run found another marker to add, which is the signature of a rule stated
backwards. Turned around it is short and it holds:

> A copy is safe to remove only when the person deleting it is the one who put
> it there.

So: the current user's profile, minus AppData, minus `.cache`, `.config`,
`.local/share`, minus the package stores. Not another user's profile. Not
Program Files. Not ProgramData.

It costs real findings — duplicated installers under AppData are genuine waste
this will not offer — and that is the right price. Those belong to categories
with their own rules, not to "identical, therefore removable".

Measured on 375 GB: 21.6 GB found with no rule, 15.5 GB with the blocklist,
**5.1 GB inside the user's own area**. The last number is the one where every
single result is a file the user put there themselves.

### 1. Exact duplicates — provable

Same bytes, more than one path. Keeping one loses nothing.

Finding them without reading the disk:

1. group by **size** — free once phase 0 is done, and most sizes are unique
2. within groups of two or more, fingerprint **first 64 KB + last 64 KB + size**
3. full hash only when fingerprints collide

Indexed documents already carry a whole-file XXH3-128 in the `doc` table and
skip straight to step 3.

Three traps:

- **Hardlinks.** Two names for one MFT record share an FRN. Deleting one frees
  nothing. Filter on FRN before anything else.
- **Cloud placeholders.** `FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS` means reading
  the file downloads it. `isCloudPlaceholder()` already knows; honour it.
- **Which copy to keep.** Never the build artifact over the source it was made
  from — the first run kept `build/static/media/video-1.a1061371.mp4` and
  offered to bin `src/media/.../video-1.mp4`, because the build path happened to
  be shallower. Then prefer the one not in Downloads or Temp, then the shortest
  path, then the oldest. Always show which one, and why.

### 2. Regenerable output — regenerable by definition

`node_modules`, `build/intermediates`, `target/`, `.gradle/caches`,
`__pycache__`, `.next`, `dist/assets`. Filo already lists these twice — in
`kExcluded` for indexing and in `contentRankPenalty` for ranking.

A folder named `build` full of source code is not build output, so a **marker**
raises this from a guess to a fact: `node_modules` next to a `package.json`,
`target` next to a `Cargo.toml`, `__pycache__` next to `.py` files. No marker,
no claim.

### 3. Stale installers — probably

`.exe`, `.msi`, `.zip` in Downloads, older than some months. Reasonable, not
certain: presented as "probably finished with these", never as safe.

### 4. Near-duplicate documents — the user decides

The four contracts above are not byte identical. Their **vectors are nearly
identical**, and those vectors already exist.

This is the only category where Filo can say something no other tool can, and it
is also the one where the recommendation is never "delete". It is "these two
look like the same document — here is how they differ".

## Where the model earns its place

Most explanation is a template. "node_modules holds the libraries your project
downloads; they are listed in package.json and reinstalled with one command" is
a fixed string, and writing it with a 3B model would be a party trick.

The model earns its place in exactly one spot: **telling the user what two
near-duplicate documents actually are, so they can choose between them without
opening both.**

> Both are the Acme rental contract. The newer one adds article 8 on late-payment
> penalties; the other stops at article 7.

That needs the document text — which is now stored for PDFs and Office files —
and it needs a model. It is also the only case where the user genuinely cannot
decide alone, which is what makes it worth a second of inference.

Constraints: on demand only, never in bulk, only when a group is expanded. The
model is already loaded lazily on the search thread; this reuses it.

## What the screen says

Never individual files at the top level. A person can judge "12 copies of one
PDF, 340 MB". Nobody can judge forty thousand rows.

```
Exact copies                1.2 GB   847 files in 203 groups
Build output                4.7 GB   12 folders, regenerable
Installers already used     890 MB   14 files, downloaded over 6 months ago
Documents that look alike   210 MB   38 groups — worth a look
```

Expanding a group shows the files, which one would be kept, and why.

## Acting

`SHFileOperationW`, `FOF_ALLOWUNDO`, and two things that are easy to get wrong:

- **Verify immediately before acting.** The index can be stale — on a machine
  without administrator rights it is *guaranteed* to be stale, because the USN
  journal cannot be read. Every path is re-checked at the moment of deletion,
  and a mismatch skips the file rather than guessing.
- **The Recycle Bin has a capacity.** A file larger than it is deleted
  permanently, and Windows asks first. That prompt must never be suppressed;
  better still, check the size beforehand and refuse, offering to move the file
  instead.

Afterwards, write a log of what went where. The Recycle Bin is the undo, but
only if the user can find what to undo.

## Phases

| | | |
|---|---|---|
| 0 | Size and mtime in the index | half a day |
| 1 | Exact duplicates, grouping, Recycle Bin | 2 days |
| 2 | Regenerable output with marker checks | 1 day |
| 3 | Stale installers and versioned names | half a day |
| 4 | Near-duplicates from the vectors | 1 day |
| 5 | Model-written descriptions, on demand | half a day |

Phases 0 and 1 are two and a half days and carry most of the value. They need
neither the embeddings nor the model.

## The risk that matters

It is not a crash. It is a false positive the user accepts because the interface
sounded confident.

Every mitigation above — categories instead of scores, nothing pre-selected,
always showing what is kept, Recycle Bin only, verify before acting — exists for
that one failure, because it is the only one that cannot be undone by fixing the
code.
