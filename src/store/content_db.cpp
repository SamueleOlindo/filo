#include "store/content_db.h"

#include <windows.h>
#include <sqlite3.h>

#include <string>
#include <vector>

#include "content/selector.h"
#include "store/tokenizer.h"

namespace filo {
namespace {

// How the body column is encoded.
//
// Only raw is written today. The vendored libdeflate is the decompression half
// only — DOCX and friends just need reading — and pulling in the compressor to
// shrink the stored text is not a trade worth making blind: measure what the
// text actually costs on a real disk first, and add a codec then. The column
// exists so that day does not need a schema change.
constexpr int kCodecRaw = 0;

// Versions: when one of these changes, content indexed with the previous one is
// no longer comparable and has to be regenerated. Keeping them in the database
// rather than in the code is what makes that noticeable.
constexpr int64_t kSchemaVersion = 2;   // added doc.rank_penalty
// Raised alongside the tokenizer fixes (isLetter, subwords, combining marks,
// y-diaeresis) and the chunker: they change the tokens produced and the chunk
// offsets, so an index built earlier is no longer compatible.
constexpr int64_t kTokenizerVersion = 2;
// kChunkerVersion now lives in the header: the vector store needs it too, and
// two copies of a number that must agree is exactly how they stop agreeing.

std::wstring toWide(const char* utf8) {
    if (!utf8) return L"unknown error";
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (size <= 0) return L"undecodable error";
    std::wstring out(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), size);
    return out;
}

// Decodes a stored body. Unknown codecs fail loudly rather than handing back
// compressed bytes as if they were text: a wrong snippet reads as a bug in the
// search, and nobody would think to look here.
bool decodeText(const void* body, size_t bodyBytes, int codec, std::string* out) {
    if (codec != kCodecRaw) return false;
    out->assign(static_cast<const char*>(body), bodyBytes);
    return true;
}

std::string toUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), size, nullptr, nullptr);
    return out;
}

// Commit thresholds. Transactions that are too small pay an fsync for nothing;
// too large and the WAL keeps growing and readers see the work later.
constexpr uint64_t kBatchDocuments = 500;
constexpr uint64_t kBatchBytes = 32ull << 20;

}  // namespace

// ---------------------------------------------------------------- ContentDb

ContentDb::~ContentDb() { close(); }

void ContentDb::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool ContentDb::exec(const char* sql, std::wstring* error) {
    char* message = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &message) != SQLITE_OK) {
        *error = toWide(message);
        sqlite3_free(message);
        return false;
    }
    return true;
}

bool ContentDb::open(const std::wstring& path, bool readOnly, std::wstring* error) {
    close();
    readOnly_ = readOnly;

    const std::string utf8Path = toUtf8(path);
    const int flags = readOnly ? SQLITE_OPEN_READONLY
                               : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);

    if (sqlite3_open_v2(utf8Path.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
        *error = db_ ? toWide(sqlite3_errmsg(db_)) : L"open failed";
        close();
        return false;
    }

    sqlite3_busy_timeout(db_, 5000);

    // The tokenizer has to be registered BEFORE touching the FTS5 table, and on
    // every connection: FTS5 resolves it when the table loads, and where it is
    // missing the table simply will not open.
    if (!registerTokenizer(db_, error)) {
        close();
        return false;
    }

    if (!applyPragmas(error)) return false;
    if (!readOnly && !applySchema(error)) return false;
    return true;
}

bool ContentDb::applyPragmas(std::wstring* error) {
    if (readOnly_) {
        // Readers must not be able to write even by accident, and they map the
        // file into memory: search queries touch scattered index pages, and the
        // mapping avoids one copy per page.
        return exec(
            "PRAGMA query_only = 1;"
            "PRAGMA mmap_size = 2147483648;"
            "PRAGMA cache_size = -32768;"
            "PRAGMA temp_store = MEMORY;",
            error);
    }

    // page_size has to be decided BEFORE the file exists: afterwards it needs a
    // VACUUM. 8 KB is the right size for FTS5 segments and for text blobs.
    return exec(
        "PRAGMA page_size = 8192;"
        "PRAGMA journal_mode = WAL;"
        "PRAGMA synchronous = NORMAL;"
        "PRAGMA wal_autocheckpoint = 0;"   // the writer decides the checkpoint
        "PRAGMA temp_store = MEMORY;"
        "PRAGMA foreign_keys = ON;"
        "PRAGMA cache_size = -262144;",    // 256 MB
        error);
}

bool ContentDb::applySchema(std::wstring* error) {
    // The version table has to be created and queried BEFORE the others: it is
    // what says whether the existing ones are still usable.
    if (!exec("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value INTEGER NOT NULL);",
              error)) {
        return false;
    }

    // The version number alone is not enough: if an earlier run was interrupted
    // halfway through a migration, it can say "current" while the tables are
    // still the old ones. The columns are what tell the truth.
    bool schemaLooksCurrent = true;
    {
        sqlite3_stmt* info = nullptr;
        if (sqlite3_prepare_v2(db_, "PRAGMA table_info(doc)", -1, &info, nullptr) == SQLITE_OK) {
            bool tableExists = false, hasPenalty = false;
            while (sqlite3_step(info) == SQLITE_ROW) {
                tableExists = true;
                const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(info, 1));
                if (name && std::string(name) == "rank_penalty") hasPenalty = true;
            }
            sqlite3_finalize(info);
            if (tableExists && !hasPenalty) schemaLooksCurrent = false;
        }
    }

    const int64_t existingSchema = metaInt("schema_ver", 0);
    if (!schemaLooksCurrent || (existingSchema != 0 && existingSchema != kSchemaVersion)) {
        // SCHEMA change: emptying the rows is not enough, because the columns
        // may have changed. The tables have to be dropped and rebuilt.
        if (!exec("DROP TABLE IF EXISTS ft; DROP TABLE IF EXISTS chunk;"
                  "DROP TABLE IF EXISTS doc_text; DROP TABLE IF EXISTS doc;",
                  error)) {
            return false;
        }
        staleIndexRebuilt_ = true;
    }

    static const char* kSchema = R"sql(

CREATE TABLE IF NOT EXISTS doc (
  doc_id            INTEGER PRIMARY KEY,
  volume_id         INTEGER NOT NULL,
  frn               INTEGER NOT NULL,
  usn               INTEGER NOT NULL DEFAULT 0,
  size              INTEGER NOT NULL DEFAULT 0,
  mtime             INTEGER NOT NULL DEFAULT 0,
  content_hash_lo   INTEGER NOT NULL DEFAULT 0,
  content_hash_hi   INTEGER NOT NULL DEFAULT 0,
  kind              INTEGER NOT NULL DEFAULT 0,
  state             INTEGER NOT NULL DEFAULT 0,
  extractor_ver     INTEGER NOT NULL DEFAULT 0,
  chunk_count       INTEGER NOT NULL DEFAULT 0,
  -- Score penalty: separates text a person wrote from text a machine
  -- generated, which matches nearly every query.
  rank_penalty      REAL NOT NULL DEFAULT 0
);

-- The key is the FULL FRN, sequence number included: when NTFS reuses an MFT
-- slot the FRN changes, so a new file cannot inherit the row of a deleted one.
-- It is the same trap already met in layer 1.
CREATE UNIQUE INDEX IF NOT EXISTS doc_by_frn ON doc(volume_id, frn);

CREATE TABLE IF NOT EXISTS chunk (
  chunk_id INTEGER PRIMARY KEY,   -- == ft.rowid, and tomorrow == vec.rowid
  doc_id   INTEGER NOT NULL REFERENCES doc(doc_id) ON DELETE CASCADE,
  boff     INTEGER NOT NULL,
  blen     INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS chunk_by_doc ON chunk(doc_id);

-- Text kept ONLY for formats that are expensive to re-parse (pdf, office...).
-- For txt/code/markup the file on disk already IS the text: reopening it costs
-- milliseconds, and keeping a copy would be pure waste.
CREATE TABLE IF NOT EXISTS doc_text (
  doc_id  INTEGER PRIMARY KEY REFERENCES doc(doc_id) ON DELETE CASCADE,
  codec   INTEGER NOT NULL DEFAULT 0,   -- 0 = raw, 1 = zstd
  raw_len INTEGER NOT NULL,
  body    BLOB NOT NULL
);
)sql";

    if (!exec(kSchema, error)) return false;

    // The FTS5 table has to be created separately: CREATE VIRTUAL TABLE IF NOT
    // EXISTS refuses to sit in a script with other statements on some versions.
    //
    // contentless (content='') because the text is not kept here;
    // contentless_delete=1 because the USN gives us the FRN and not the old
    // text, so deletion has to work by rowid alone;
    // detail=full because without it phrase search does not exist — "la penale"
    // as a sequence rather than as scattered words.
    // tokenize='filo': our own tokenizer. On top of lowercasing and stripping
    // accents it emits, at the SAME position, the Italian and English stems and
    // the parts of identifiers. That is what finds "noleggi" when you search
    // for "noleggio".
    static const char* kFts = R"sql(
CREATE VIRTUAL TABLE IF NOT EXISTS ft USING fts5(
  body,
  content='',
  contentless_delete=1,
  detail=full,
  tokenize='filo'
);
)sql";
    if (!exec(kFts, error)) return false;

    if (existingSchema == 0 || staleIndexRebuilt_) {
        if (!setMetaInt("schema_ver", kSchemaVersion, error)) return false;
        if (!setMetaInt("tok_ver", kTokenizerVersion, error)) return false;
        if (!setMetaInt("chunker_ver", kChunkerVersion, error)) return false;
        if (!setMetaInt("content_gen", metaInt("content_gen", 0) + 1, error)) return false;
        return true;
    }

    // The versions used to be written and never read again: an index built with
    // one tokenizer would have been queried with another, and queries and
    // posting lists would have stopped meeting without a single error message.
    // Here the stale index is emptied and rebuilt: expensive, but the
    // alternative is a search engine that finds nothing and never says why.
    const bool stale = metaInt("tok_ver", 0) != kTokenizerVersion ||
                       metaInt("chunker_ver", 0) != kChunkerVersion ||
                       metaInt("schema_ver", 0) != kSchemaVersion;
    if (stale) {
        if (!exec("DELETE FROM ft; DELETE FROM chunk; DELETE FROM doc_text; DELETE FROM doc;",
                  error)) {
            return false;
        }
        if (!setMetaInt("schema_ver", kSchemaVersion, error)) return false;
        if (!setMetaInt("tok_ver", kTokenizerVersion, error)) return false;
        if (!setMetaInt("chunker_ver", kChunkerVersion, error)) return false;
        if (!setMetaInt("content_gen", metaInt("content_gen", 1) + 1, error)) return false;
        staleIndexRebuilt_ = true;
    }
    return true;
}

bool ContentDb::isStaleForReading() const {
    return metaInt("tok_ver", 0) != kTokenizerVersion ||
           metaInt("chunker_ver", 0) != kChunkerVersion;
}

int64_t ContentDb::metaInt(const char* key, int64_t fallback) const {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT value FROM meta WHERE key = ?", -1,
                           &statement, nullptr) != SQLITE_OK) {
        return fallback;
    }
    sqlite3_bind_text(statement, 1, key, -1, SQLITE_STATIC);
    int64_t value = fallback;
    if (sqlite3_step(statement) == SQLITE_ROW) value = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    return value;
}

bool ContentDb::setMetaInt(const char* key, int64_t value, std::wstring* error) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "INSERT INTO meta(key,value) VALUES(?,?) "
                           "ON CONFLICT(key) DO UPDATE SET value = excluded.value",
                           -1, &statement, nullptr) != SQLITE_OK) {
        *error = toWide(sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_text(statement, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_int64(statement, 2, value);
    const bool ok = sqlite3_step(statement) == SQLITE_DONE;
    if (!ok) *error = toWide(sqlite3_errmsg(db_));
    sqlite3_finalize(statement);
    return ok;
}

int64_t ContentDb::pageCount() const {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db_, "PRAGMA page_count", -1, &statement, nullptr) != SQLITE_OK) {
        return 0;
    }
    int64_t pages = 0;
    if (sqlite3_step(statement) == SQLITE_ROW) pages = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    return pages;
}

int64_t ContentDb::fileBytes() const { return pageCount() * 8192; }

void ContentDb::checkpoint(bool truncate) {
    if (!db_ || readOnly_) return;
    std::wstring ignored;
    exec(truncate ? "PRAGMA wal_checkpoint(TRUNCATE)" : "PRAGMA wal_checkpoint(PASSIVE)",
         &ignored);
}

// ------------------------------------------------------------ ContentWriter

ContentWriter::ContentWriter(ContentDb& db) : db_(db) {}

ContentWriter::~ContentWriter() {
    // ROLLBACK, not COMMIT. Arriving here with a batch still open means the
    // caller is bailing out on an error: making permanent the very state it was
    // discarding — a half-inserted document, say — would leave the database
    // inconsistent with no diagnostic at all.
    // Good work is made permanent by an explicit commitBatch().
    if (inBatch_) {
        inBatch_ = false;
        std::wstring ignored;
        db_.exec("ROLLBACK", &ignored);
    }
    finalizeStatements();
}

void ContentWriter::finalizeStatements() {
    for (sqlite3_stmt** statement : {&insertDoc_, &insertChunk_, &insertFt_, &deleteFt_,
                                     &selectChunks_, &deleteChunks_, &deleteDoc_, &findDoc_,
                                     &insertText_}) {
        if (*statement) {
            sqlite3_finalize(*statement);
            *statement = nullptr;
        }
    }
}

bool ContentWriter::prepare(std::wstring* error) {
    struct Preparation {
        sqlite3_stmt** target;
        const char* sql;
    };
    const Preparation preparations[] = {
        {&insertDoc_,
         "INSERT INTO doc(volume_id,frn,usn,size,mtime,content_hash_lo,content_hash_hi,"
         "kind,state,extractor_ver,chunk_count,rank_penalty) "
         "VALUES(?,?,?,?,?,?,?,?,?,?,?,?)"},
        {&insertChunk_, "INSERT INTO chunk(doc_id,boff,blen) VALUES(?,?,?)"},
        {&insertText_,
         "INSERT OR REPLACE INTO doc_text(doc_id,codec,raw_len,body) VALUES(?,?,?,?)"},
        {&insertFt_, "INSERT INTO ft(rowid,body) VALUES(?,?)"},
        {&deleteFt_, "DELETE FROM ft WHERE rowid = ?"},
        {&selectChunks_, "SELECT chunk_id FROM chunk WHERE doc_id = ?"},
        {&deleteChunks_, "DELETE FROM chunk WHERE doc_id = ?"},
        {&deleteDoc_, "DELETE FROM doc WHERE doc_id = ?"},
        {&findDoc_,
         "SELECT doc_id,size,mtime,content_hash_lo,content_hash_hi,extractor_ver "
         "FROM doc WHERE volume_id = ? AND frn = ?"},
    };

    for (const Preparation& preparation : preparations) {
        if (sqlite3_prepare_v2(db_.handle(), preparation.sql, -1, preparation.target,
                               nullptr) != SQLITE_OK) {
            *error = toWide(sqlite3_errmsg(db_.handle()));
            finalizeStatements();
            return false;
        }
    }
    return true;
}

bool ContentWriter::beginBatch(std::wstring* error) {
    if (inBatch_) return true;
    // IMMEDIATE takes the write lock straight away: with a single writer there
    // is no contention, but it avoids discovering mid-transaction that the
    // database is busy.
    if (!db_.exec("BEGIN IMMEDIATE", error)) return false;
    inBatch_ = true;
    pendingDocuments_ = 0;
    pendingBytes_ = 0;
    return true;
}

bool ContentWriter::commitBatch(std::wstring* error) {
    if (!inBatch_) return true;
    inBatch_ = false;
    pendingDocuments_ = 0;
    pendingBytes_ = 0;
    if (!db_.exec("COMMIT", error)) return false;

    // Automatic checkpointing is disabled on purpose, but somebody still has to
    // do it: otherwise the entire indexing run stays in the -wal file.
    if (++committedBatches_ % 16 == 0) db_.checkpoint(false);
    return true;
}

bool ContentWriter::maybeCommit(std::wstring* error) {
    if (!inBatch_) return true;
    if (pendingDocuments_ < kBatchDocuments && pendingBytes_ < kBatchBytes) return true;
    return commitBatch(error) && beginBatch(error);
}

bool ContentWriter::findDocument(uint32_t volumeId, uint64_t frn, ExistingDoc* existing,
                                 std::wstring* error) const {
    *existing = ExistingDoc{};

    sqlite3_reset(findDoc_);
    sqlite3_bind_int64(findDoc_, 1, static_cast<int64_t>(volumeId));
    sqlite3_bind_int64(findDoc_, 2, static_cast<int64_t>(frn));

    const int result = sqlite3_step(findDoc_);
    if (result == SQLITE_ROW) {
        existing->docId = sqlite3_column_int64(findDoc_, 0);
        existing->size = static_cast<uint64_t>(sqlite3_column_int64(findDoc_, 1));
        existing->mtime = sqlite3_column_int64(findDoc_, 2);
        existing->contentHashLow = static_cast<uint64_t>(sqlite3_column_int64(findDoc_, 3));
        existing->contentHashHigh = static_cast<uint64_t>(sqlite3_column_int64(findDoc_, 4));
        existing->extractorVersion = static_cast<uint32_t>(sqlite3_column_int64(findDoc_, 5));
    } else if (result != SQLITE_DONE) {
        *error = toWide(sqlite3_errmsg(db_.handle()));
    }

    // The reset must ALWAYS happen, including after SQLITE_ROW: a statement
    // left in the "row" state keeps a read open for the whole session, the WAL
    // can no longer be folded back, and every later write pays for it.
    sqlite3_reset(findDoc_);
    return result == SQLITE_ROW || result == SQLITE_DONE;
}

bool ContentWriter::addDocument(const DocumentRecord& document, const std::string& text,
                                const std::vector<ChunkRecord>& chunks, int64_t* docId,
                                std::wstring* error) {
    if (!beginBatch(error)) return false;

    sqlite3_reset(insertDoc_);
    int column = 1;
    sqlite3_bind_int64(insertDoc_, column++, static_cast<int64_t>(document.volumeId));
    sqlite3_bind_int64(insertDoc_, column++, static_cast<int64_t>(document.frn));
    sqlite3_bind_int64(insertDoc_, column++, static_cast<int64_t>(document.usn));
    sqlite3_bind_int64(insertDoc_, column++, static_cast<int64_t>(document.size));
    sqlite3_bind_int64(insertDoc_, column++, document.mtime);
    sqlite3_bind_int64(insertDoc_, column++, static_cast<int64_t>(document.contentHashLow));
    sqlite3_bind_int64(insertDoc_, column++, static_cast<int64_t>(document.contentHashHigh));
    sqlite3_bind_int(insertDoc_, column++, document.kind);
    sqlite3_bind_int(insertDoc_, column++, static_cast<int>(document.state));
    sqlite3_bind_int64(insertDoc_, column++, static_cast<int64_t>(document.extractorVersion));
    sqlite3_bind_int64(insertDoc_, column++, static_cast<int64_t>(chunks.size()));
    sqlite3_bind_double(insertDoc_, column++, document.rankPenalty);

    if (sqlite3_step(insertDoc_) != SQLITE_DONE) {
        *error = toWide(sqlite3_errmsg(db_.handle()));
        return false;
    }
    const int64_t newDocId = sqlite3_last_insert_rowid(db_.handle());

    for (const ChunkRecord& chunk : chunks) {
        sqlite3_reset(insertChunk_);
        sqlite3_bind_int64(insertChunk_, 1, newDocId);
        sqlite3_bind_int(insertChunk_, 2, static_cast<int>(chunk.byteOffset));
        sqlite3_bind_int(insertChunk_, 3, static_cast<int>(chunk.byteLength));
        if (sqlite3_step(insertChunk_) != SQLITE_DONE) {
            *error = toWide(sqlite3_errmsg(db_.handle()));
            return false;
        }
        const int64_t chunkId = sqlite3_last_insert_rowid(db_.handle());

        // ft.rowid IS chunk.chunk_id. That is the invariant layer 4 rests on:
        // merging lexical and vector results becomes an operation on integers
        // in one space, with no mapping table in between.
        const size_t end = static_cast<size_t>(chunk.byteOffset) + chunk.byteLength;
        if (end > text.size()) {
            *error = L"chunk out of the text's bounds";
            return false;
        }

        sqlite3_reset(insertFt_);
        sqlite3_bind_int64(insertFt_, 1, chunkId);
        sqlite3_bind_text(insertFt_, 2, text.data() + chunk.byteOffset,
                          static_cast<int>(chunk.byteLength), SQLITE_STATIC);
        if (sqlite3_step(insertFt_) != SQLITE_DONE) {
            *error = toWide(sqlite3_errmsg(db_.handle()));
            return false;
        }

        pendingBytes_ += chunk.byteLength;
    }

    // Keep the extracted text for the formats we cannot cheaply re-read.
    //
    // Without this a snippet for a PDF meant re-running the parser — so the UI
    // simply showed none — and the embedding pass paid the extraction cost a
    // second time for every document on the disk.
    if (needsExtractor(static_cast<ContentKind>(document.kind)) && !text.empty()) {
        sqlite3_reset(insertText_);
        sqlite3_bind_int64(insertText_, 1, newDocId);
        sqlite3_bind_int(insertText_, 2, kCodecRaw);
        sqlite3_bind_int64(insertText_, 3, static_cast<int64_t>(text.size()));
        sqlite3_bind_blob(insertText_, 4, text.data(), static_cast<int>(text.size()),
                          SQLITE_STATIC);
        if (sqlite3_step(insertText_) != SQLITE_DONE) {
            *error = toWide(sqlite3_errmsg(db_.handle()));
            return false;
        }
        pendingBytes_ += text.size();
    }

    ++pendingDocuments_;
    if (docId) *docId = newDocId;
    return maybeCommit(error);
}

bool ContentWriter::deleteDocument(int64_t docId, std::wstring* error) {
    if (!beginBatch(error)) return false;

    // The FTS5 rows have to be removed explicitly: the table is contentless and
    // takes no part in foreign keys, so ON DELETE CASCADE never reaches it.
    sqlite3_reset(selectChunks_);
    sqlite3_bind_int64(selectChunks_, 1, docId);

    std::vector<int64_t> chunkIds;
    while (sqlite3_step(selectChunks_) == SQLITE_ROW) {
        chunkIds.push_back(sqlite3_column_int64(selectChunks_, 0));
    }

    for (const int64_t chunkId : chunkIds) {
        sqlite3_reset(deleteFt_);
        sqlite3_bind_int64(deleteFt_, 1, chunkId);
        if (sqlite3_step(deleteFt_) != SQLITE_DONE) {
            *error = toWide(sqlite3_errmsg(db_.handle()));
            return false;
        }
    }

    for (sqlite3_stmt* statement : {deleteChunks_, deleteDoc_}) {
        sqlite3_reset(statement);
        sqlite3_bind_int64(statement, 1, docId);
        if (sqlite3_step(statement) != SQLITE_DONE) {
            *error = toWide(sqlite3_errmsg(db_.handle()));
            return false;
        }
    }

    ++pendingDocuments_;
    return maybeCommit(error);
}

bool readDocumentText(ContentDb& db, int64_t docId, std::string* out) {
    out->clear();
    if (!db.handle() || docId < 0) return false;

    // Prepared per call rather than cached: this runs once per snippet, a few
    // times per keystroke at most, and a cached statement would have to be
    // per-connection to stay legal under SQLITE_THREADSAFE=2.
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db.handle(),
                           "SELECT codec, body FROM doc_text WHERE doc_id = ?", -1,
                           &statement, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(statement, 1, docId);

    bool ok = false;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        const int codec = sqlite3_column_int(statement, 0);
        const void* body = sqlite3_column_blob(statement, 1);
        const auto bodyBytes = static_cast<size_t>(sqlite3_column_bytes(statement, 1));
        if (body && bodyBytes > 0) ok = decodeText(body, bodyBytes, codec, out);
    }
    sqlite3_finalize(statement);
    return ok;
}

}  // namespace filo
