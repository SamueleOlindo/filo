#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace filo {

// How the text was split into chunks. Vectors computed against one splitting
// are not comparable with vectors computed against another, so this number
// travels in the vector file header too — see VectorStore::Identity.
//
// Version 3 counts TOKENS instead of bytes. Version 2 aimed at 2048 bytes
// against a 512-token window, and about 92% of chunks came out longer than the
// window: the embedder truncated them silently and never saw their tails. The
// number here is what makes an existing index rebuild rather than quietly
// mixing chunks from both rules, whose vectors are not comparable.
constexpr int64_t kChunkerVersion = 3;

// How the text was produced. Version 3 keeps the extracted text of formats that
// need a parser, so snippets and embeddings stop re-parsing them. Raising this
// is what makes an existing index redo those documents.
constexpr int64_t kExtractorVersion = 3;

// Outcome of the extraction. Not a diagnostic detail: if a PDF is a scan with
// no text and we do not say so, the user searches, finds nothing, and concludes
// the program does not work. Every state other than Ok is worth showing.
enum class DocState : uint8_t {
    Ok = 0,
    Empty = 1,      // extracted fine, but with no useful text
    NeedsOcr = 2,   // image-only PDF
    Encrypted = 3,  // password protected
    TooBig = 4,     // past the limits: indexed only in part
    Failed = 5,     // the parser failed
    Quarantined = 6 // it crashed a worker: do not retry
};

struct DocumentRecord {
    uint32_t volumeId = 0;
    uint64_t frn = 0;       // FULL FRN: the top 16 sequence-number bits make
                            // it impossible by construction for a new file to
                            // inherit the content of a deleted one
    uint64_t usn = 0;
    uint64_t size = 0;
    int64_t  mtime = 0;
    uint64_t contentHashLow = 0;   // XXH3-128 of the raw bytes
    uint64_t contentHashHigh = 0;
    uint8_t  kind = 0;             // ContentKind
    DocState state = DocState::Ok;
    uint32_t extractorVersion = 0;
    double   rankPenalty = 0.0;   // see contentRankPenalty()
};

struct ChunkRecord {
    uint32_t byteOffset = 0;  // inside the normalized text
    uint32_t byteLength = 0;
};

// The content database: %LOCALAPPDATA%\filo\content-C.db
//
// A separate file from layer 1's binary snapshot, which stays as it is. The
// lexical index, the metadata and (tomorrow) the vectors all live in here, so
// that an update triggered by the USN is ONE transaction instead of a
// reconciliation protocol between different stores.
class ContentDb {
public:
    ContentDb() = default;
    ~ContentDb();

    ContentDb(const ContentDb&) = delete;
    ContentDb& operator=(const ContentDb&) = delete;

    // readOnly opens a read-only connection: every search thread has its own,
    // while writing goes through a single thread.
    bool open(const std::wstring& path, bool readOnly, std::wstring* error);
    void close();

    sqlite3* handle() const { return db_; }
    bool exec(const char* sql, std::wstring* error);

    int64_t metaInt(const char* key, int64_t fallback) const;
    bool setMetaInt(const char* key, int64_t value, std::wstring* error);

    int64_t pageCount() const;
    int64_t fileBytes() const;

    // Read-only, so we cannot empty anything: the caller has to know the index
    // was built with a tokenizer different from the one that will interpret
    // the queries, and that it would therefore not answer them correctly.
    bool isStaleForReading() const;
    bool staleIndexRebuilt() const { return staleIndexRebuilt_; }

    // Folds the write-ahead log back into the database. Without it, all the
    // work of an indexing run stays in the -wal file, which grows as large as
    // the whole session and takes up the disk twice.
    void checkpoint(bool truncate);

private:
    bool applyPragmas(std::wstring* error);
    bool applySchema(std::wstring* error);

    sqlite3* db_ = nullptr;
    bool readOnly_ = false;
    bool staleIndexRebuilt_ = false;
};

// Single writer.
//
// In WAL mode SQLite allows N readers but ONE SINGLE writer: opening more write
// connections does not raise the throughput, it only produces SQLITE_BUSY. With
// one writer conflicts are impossible by construction, and all the pending work
// can be packed into a single transaction.
class ContentWriter {
public:
    explicit ContentWriter(ContentDb& db);
    ~ContentWriter();

    ContentWriter(const ContentWriter&) = delete;
    ContentWriter& operator=(const ContentWriter&) = delete;

    bool prepare(std::wstring* error);

    // Inserts document, chunks and FTS5 rows in one go. `text` is the
    // normalized UTF-8 text; the chunks are ranges inside it.
    bool addDocument(const DocumentRecord& document, const std::string& text,
                     const std::vector<ChunkRecord>& chunks, int64_t* docId,
                     std::wstring* error);

    // Removes document, chunks and FTS5 rows. Needed because the USN gives us
    // the FRN, not the old text: deletion has to be possible by rowid alone,
    // and that is exactly what contentless_delete=1 enables.
    bool deleteDocument(int64_t docId, std::wstring* error);

    // State of a document already present, with what it takes to decide
    // whether it has to be redone: without the hashes the comparison is
    // impossible and every change after the first indexing would stay
    // invisible forever.
    struct ExistingDoc {
        int64_t  docId = -1;
        uint64_t size = 0;
        int64_t  mtime = 0;
        uint64_t contentHashLow = 0;
        uint64_t contentHashHigh = 0;
        // It is not enough that the FILE has not changed: if the EXTRACTOR
        // changed, the stored text is the one the previous version produced
        // and has to be redone even when the file is identical.
        uint32_t extractorVersion = 0;
    };

    bool findDocument(uint32_t volumeId, uint64_t frn, ExistingDoc* existing,
                      std::wstring* error) const;

    bool beginBatch(std::wstring* error);
    bool commitBatch(std::wstring* error);
    // Closes the transaction if it has piled up enough work.
    bool maybeCommit(std::wstring* error);

    uint64_t pendingDocuments() const { return pendingDocuments_; }
    uint64_t pendingBytes() const { return pendingBytes_; }

private:
    void finalizeStatements();

    ContentDb& db_;
    bool inBatch_ = false;
    uint64_t pendingDocuments_ = 0;
    uint64_t pendingBytes_ = 0;
    uint64_t committedBatches_ = 0;

    sqlite3_stmt* insertDoc_ = nullptr;
    sqlite3_stmt* insertChunk_ = nullptr;
    sqlite3_stmt* insertFt_ = nullptr;
    sqlite3_stmt* deleteFt_ = nullptr;
    sqlite3_stmt* selectChunks_ = nullptr;
    sqlite3_stmt* deleteChunks_ = nullptr;
    sqlite3_stmt* deleteDoc_ = nullptr;
    sqlite3_stmt* findDoc_ = nullptr;
    sqlite3_stmt* insertText_ = nullptr;
};

// Reads back the text stored for a document, empty if none was kept.
//
// Only the formats that need an extractor have a row here — see
// needsExtractor(). Everything else is cheaper to read off disk than to hold
// twice.
//
// Works on a read-only connection, which is the point: the search threads use
// it to build snippets for PDFs and Office documents without opening the file
// at all. That also makes snippets survive a file being moved, renamed or
// parked on a drive that is currently unplugged.
bool readDocumentText(ContentDb& db, int64_t docId, std::string* out);

}  // namespace filo
