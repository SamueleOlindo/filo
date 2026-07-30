#pragma once

#include <string>

struct sqlite3;

namespace filo {

// The name the tokenizer is referred to by in CREATE VIRTUAL TABLE.
inline constexpr const char* kTokenizerName = "filo";

// Registers the tokenizer on a connection.
//
// This has to happen on EVERY connection that opens the database, readers
// included: FTS5 resolves the tokenizer when it loads the table, and on a
// connection where it is not registered the table simply will not open.
bool registerTokenizer(sqlite3* db, std::wstring* error);

}  // namespace filo
