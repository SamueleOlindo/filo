#include "content/chunker.h"

#include <algorithm>

namespace filo {
namespace {

// How much a chunk may hold, counted in TOKENS rather than bytes.
//
// The embedding model reads 512 tokens and drops the rest without a word. The
// old target was 2048 BYTES, written down as "roughly 512 tokens", and the
// profiler measured what that guess really cost: 92% of chunks came out longer
// than the window, so for nine chunks in ten the model saw the opening and
// never the end. Those tails stayed findable by word and were invisible to
// search by meaning.
//
// 448 keeps 64 tokens spare under the window, and they are all spoken for.
// llama_tokenize is called with add_special=true, so the model's own markers
// come out of the same 512; embedPassage prepends the passage prefix a model
// may ask for, which is text the chunker never sees; and the estimator below
// can be wrong. None of the three is visible from here, so the room for them
// has to be left unconditionally.
constexpr size_t kTargetTokens = 448;

// Tokens are ESTIMATED from the bytes, never counted.
//
// The chunker runs in the extraction pass, where no model is loaded. Asking
// llama to tokenize here would tie the chunk offsets written to disk to one
// model file: swap the model and every boundary moves, so an index could no
// longer be compared with a fresh chunking of the same text.
//
// The estimate is carried in sixteenths of a token per character, in integers.
// Chunk offsets are written to disk, so where a file splits must not depend on
// how a compiler rounded: with floats and two sets of flags, a boundary landing
// exactly on the budget could go either way.
//
// Every weight below is chosen to come out HIGH. Estimating too many tokens
// makes chunks shorter than they had to be and costs indexing time; estimating
// too few loses the tail of a chunk, which is the bug this replaces.
//
// MEASURED, twice, and the second measurement is the one that matters.
//
// --selftest <model> chunks four samples here and tokenizes them there. Longest
// chunk each, out of the 512 the model reads: Italian prose 287, a table of
// figures 280, code 447, accented and mixed 288. Nothing truncated.
//
// Then the whole disk: 579,190 chunks embedded, 413 tokens on average, 512 at
// the longest, and **104,654 of them — 18% — still truncated**.
//
// So the four samples were not the corpus. Real content includes minified
// JavaScript, base64 blobs, dense JSON and log lines, and a byte-level BPE
// spends far more tokens on those than on any prose. The table below charges
// symbols 12/16 of a token each; byte fallback can cost a whole token per byte.
//
// Version 2 truncated 92% of chunks, so this is five times better and still not
// right. Do not read the "nothing truncated" from the four samples as a
// property of the chunker: it is a property of those four samples.
//
// THE REAL FIX IS NOT HERE.
//
// Lowering kTargetTokens trades quality on every chunk against a tail that
// only some chunks have, and cannot reach zero without making prose chunks
// absurdly small — the estimator would have to assume the worst case for text
// that is nothing like it. The fix belongs in the embedding pass, which knows
// the TRUE count the moment it tokenizes: a chunk that comes out over the
// window should be embedded as two vectors rather than have its tail dropped.
// See the task list.
//
// What version 3 does buy, and it is not nothing: 92% down to 18%, and smaller
// chunks put a vector nearer to one idea. What it costs: 218,210 vectors became
// 579,190, so roughly three times the embedding time and a vector file that
// grew from 81 MB to 231 MB.
//
// If this is ever retuned, kTargetTokens is the single number to move, and the
// figure to watch is the truncated count from a full --embed run — not the four
// samples, and certainly not reasoning about bytes per token, which is how the
// 2048-byte target came to be believed in the first place.
constexpr size_t kUnitsPerToken = 16;
// Latin prose against a multilingual vocabulary of this size runs 3.5 to 4
// bytes a token. Five sixteenths charges as if it were 3.2.
constexpr size_t kUnitsLetter = 5;
// Numbers are split into groups of one to three digits, and vocabularies that
// give every single digit its own token are common enough not to bet against.
constexpr size_t kUnitsDigit = 12;
// Punctuation is usually a token on its own; only pairs like "**" or "://"
// merge into one.
constexpr size_t kUnitsSymbol = 12;
// A single space almost always rides along with the word after it and costs
// nothing. A RUN of them — indentation, a table drawn with spaces — is about a
// token every three or four, and that is the case worth being right about.
constexpr size_t kUnitsSpace = 4;
// Two-byte characters: accented Latin, Greek, Cyrillic. Inside a common word
// they ride along with it, alone they are a token each, and on a script the
// vocabulary covers thinly they fall back to one token per BYTE.
constexpr size_t kUnitsTwoByte = 16;
// Three-byte characters: CJK, where a character is normally one token and the
// rare ones fall back to their three bytes.
constexpr size_t kUnitsThreeByte = 20;
// Four-byte characters: emoji and the rarer planes, one to three tokens each.
constexpr size_t kUnitsFourByte = 24;

// Overlap between consecutive chunks: a sentence straddling the cut has to
// survive whole in at least one of them, or it becomes unfindable.
//
// Still measured in bytes. What the token budget changed is how much text a
// chunk holds, not how long a sentence is.
constexpr size_t kOverlapBytes = 256;
// Below this threshold we stop looking for an elegant cut point.
constexpr size_t kMinBytes = 64;

bool isContinuationByte(char c) {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

// Backs up to the start of a UTF-8 character. Cutting mid-sequence would
// produce invalid bytes that SQLite either rejects or stores corrupted.
//
// The walk back is capped at three steps, the longest tail a valid UTF-8
// sequence can have. Without the cap, a long run of continuation bytes — a
// binary blob appended to a log, or badly encoded text — made it scan backwards
// for megabytes on every chunk: quadratic cost, hours on a single file, and an
// index filled with one-byte chunks.
size_t alignToCharacter(const std::string& text, size_t at) {
    const size_t original = at;
    for (int step = 0; step < 3 && at > 0 && at < text.size() &&
                       isContinuationByte(text[at]); ++step) {
        --at;
    }
    if (at < text.size() && isContinuationByte(text[at])) return original;
    return at;
}

// The next lead byte at or after `at`. This is the fallback when no elegant cut
// exists: it guarantees both forward progress and a character boundary, so an
// unreadable stretch becomes ONE skipped chunk instead of thousands of
// one-byte ones.
size_t nextCharacterStart(const std::string& text, size_t at) {
    while (at < text.size() && isContinuationByte(text[at])) ++at;
    return at;
}

size_t unitsForAscii(unsigned char c) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
        return kUnitsSpace;
    }
    if (c >= '0' && c <= '9') return kUnitsDigit;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return kUnitsLetter;
    return kUnitsSymbol;
}

// Walks forward from `start` until the text behind it is worth `budget` tokens
// by the table above, and returns where it stopped. Returns text.size() when
// everything from `start` to the end of the text fits in the budget.
//
// The walk costs the length of one chunk, not the length of the file: it
// returns as soon as the budget is met.
size_t advanceByTokens(const std::string& text, size_t start, size_t budget) {
    const size_t wanted = budget * kUnitsPerToken;
    size_t units = 0;
    size_t at = start;

    while (at < text.size()) {
        const size_t here = at;
        const auto lead = static_cast<unsigned char>(text[here]);
        size_t width = 1;

        if (lead < 0x80) {
            units += unitsForAscii(lead);
        } else if (lead < 0xC0) {
            // A continuation byte with no lead byte in front of it: broken
            // UTF-8. Charged as a character of its own so the walk keeps
            // moving instead of stalling on it.
            units += kUnitsTwoByte;
        } else if (lead < 0xE0) {
            units += kUnitsTwoByte;
            width = 2;
        } else if (lead < 0xF0) {
            units += kUnitsThreeByte;
            width = 3;
        } else {
            units += kUnitsFourByte;
            width = 4;
        }

        // Step over the continuation bytes this character claims and no
        // further. A sequence cut short by a missing byte must not swallow the
        // lead byte of the next character: from there on the walk would be out
        // of step with the text and charge every character the wrong weight.
        const size_t end = here + width;
        at = here + 1;
        while (at < text.size() && at < end && isContinuationByte(text[at])) ++at;

        if (units >= wanted) return at;
    }
    return text.size();
}

// Finds the best cut point in [low, high], in decreasing order of preference:
// paragraph boundary, end of line, end of sentence, space.
size_t findCut(const std::string& text, size_t low, size_t high) {
    if (high > text.size()) high = text.size();
    if (low >= high) return high;

    for (size_t i = high; i > low; --i) {
        if (text[i - 1] == '\n' && i >= 2 && text[i - 2] == '\n') return i;
    }
    for (size_t i = high; i > low; --i) {
        if (text[i - 1] == '\n') return i;
    }
    for (size_t i = high; i > low; --i) {
        const char c = text[i - 1];
        if ((c == '.' || c == '!' || c == '?' || c == ';') && i < text.size() &&
            (text[i] == ' ' || text[i] == '\n')) {
            return i;
        }
    }
    for (size_t i = high; i > low; --i) {
        if (text[i - 1] == ' ') return i;
    }
    return high;
}

}  // namespace

std::vector<ChunkRecord> chunkText(const std::string& text) {
    std::vector<ChunkRecord> chunks;
    if (text.empty()) return chunks;

    // How far the model could read starting from the beginning. This replaces a
    // plain comparison against a byte length: 1500 bytes of Italian fit the
    // window while 1500 bytes of CJK or of figures do not, and a byte count
    // cannot tell the two apart.
    size_t limit = advanceByTokens(text, 0, kTargetTokens);
    if (limit >= text.size()) {
        chunks.push_back({0, static_cast<uint32_t>(text.size())});
        return chunks;
    }

    // Bytes per chunk is no longer a constant, so the reservation comes from
    // what this document itself measured rather than from a number here.
    chunks.reserve(text.size() / limit + 2);

    size_t start = 0;
    while (start < text.size()) {
        if (limit >= text.size()) {
            chunks.push_back({static_cast<uint32_t>(start),
                              static_cast<uint32_t>(text.size() - start)});
            break;
        }

        // Search window for the cut: from 60% of what the model can read up to
        // all of it. Taking the fraction in bytes rather than walking the text
        // again for 60% of the tokens is close enough for a lower bound — it
        // only decides how far back a seam may be looked for.
        const size_t low = start + (limit - start) * 6 / 10;
        size_t cut = alignToCharacter(text, findCut(text, low, limit));
        if (cut <= start + kMinBytes) cut = alignToCharacter(text, limit);
        if (cut <= start) {
            // No valid cut point: advance to the next character start. Never
            // "start + 1", which would cut mid-sequence.
            cut = nextCharacterStart(text, start + 1);
            if (cut <= start) cut = text.size();
        }

        chunks.push_back({static_cast<uint32_t>(start),
                          static_cast<uint32_t>(cut - start)});

        // The next chunk starts a little further back, so the text straddling
        // the cut appears whole somewhere.
        //
        // Never more than a quarter of the chunk, and on prose that cap never
        // binds. A chunk is bounded in tokens now, and text that tokenizes
        // badly — a page of figures, a table of codes — fills the budget in
        // well under half the bytes prose needs: there a fixed 256 would store
        // nearly half of every chunk a second time.
        const size_t overlap = std::min(kOverlapBytes, (cut - start) / 4);
        size_t next = alignToCharacter(text, cut - overlap);
        if (next <= start) next = cut;  // no infinite loops
        start = next;
        limit = advanceByTokens(text, start, kTargetTokens);
    }

    return chunks;
}

}  // namespace filo
