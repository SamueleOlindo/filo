// What Embedder and QueryModel are when llama.cpp was not part of the build.
//
// WHY THIS FILE EXISTS
//
// Filo is five layers and each one is optional AT RUNTIME: names always work,
// content works once a database is there, meaning works once a model is loaded.
// The interface already handles every one of those being absent, because on a
// fresh machine they all are.
//
// What was not optional was the LINK. embedder.cpp and query_model.cpp are only
// compiled when llama.cpp is found, but the window calls into both of them
// unconditionally, so a build without llama.cpp compiled cleanly and then died
// with eight unresolved externals. Anyone cloning the repository without first
// building llama.cpp got exactly that, which is to say: anyone cloning the
// repository.
//
// This is the other half of that promise. The classes exist, they answer "not
// available", and the callers do not have to know the difference — which is the
// same answer they already give when the model file is simply missing from
// disk. Guarding every call site with #if instead would have put the build
// configuration into the middle of the search path, for a feature that is
// already allowed to be absent.
//
// KEEPING IT HONEST
//
// If a new Embedder or QueryModel method gets called from outside, this file
// has to grow a stub for it or the dependency-free build breaks again in the
// same silent way. A build of the no-dependency configuration is what catches
// that, and nothing else will.

#include "search/query_model.h"
#include "vector/embedder.h"

namespace filo {

// --- Embedder ---------------------------------------------------------------

Embedder::~Embedder() = default;

bool Embedder::load(const std::wstring&, const Options&, std::wstring* error) {
    if (error) *error = L"this build has no embedding model: search by meaning is off";
    return false;
}

// ready() is false, so nothing should reach these. They return false rather
// than assert: a wrong answer here costs a search result, and a crash costs the
// application.
bool Embedder::embedPassages(const std::vector<std::string>&, float*) { return false; }
bool Embedder::embedPassage(const std::string&, float*) { return false; }
bool Embedder::embedQuery(const std::string&, float*) { return false; }
bool Embedder::embed(const std::string&, float*) { return false; }

// --- QueryModel -------------------------------------------------------------

QueryModel::~QueryModel() = default;

bool QueryModel::ensureLoaded(const std::wstring&, std::wstring* error) {
    if (error) *error = L"this build has no language model: questions are read literally";
    return false;
}

// False every time. Saying "not worth asking" is exactly right when there is
// nothing to ask, and it keeps the caller on the deterministic path it already
// takes whenever the model file is absent.
bool QueryModel::worthAsking(const QueryPlan&) { return false; }

bool QueryModel::refine(const std::wstring&, int64_t, QueryPlan*) { return false; }
bool QueryModel::describe(const std::wstring&, uint64_t, std::wstring*) { return false; }

bool QueryModel::load(const std::wstring&, std::wstring*) { return false; }
bool QueryModel::refineUnguarded(const std::wstring&, int64_t, QueryPlan*) { return false; }
bool QueryModel::generate(const std::string&, const char*, int, std::string*) { return false; }

}  // namespace filo
