#pragma once

#include <mutex>
#include <string>

#include "search/query_plan.h"

namespace filo {

// The small language model that reads a question and says what it is asking
// for. Layer 5, and the only generative model in the program.
//
// WHAT IT DOES NOT DO
//
// It never sees a file, never reads the disk and never produces an answer. It
// receives a sentence and returns at most three things: the subject to search
// for, a format from a fixed list, and a time from a fixed list. Everything
// after that is the same machinery layers 1 to 4 already use.
//
// WHY THE OUTPUT SPACE IS CLOSED
//
// The reply is constrained by a grammar, so the model physically cannot emit
// anything outside the enumerations below. That is not a matter of tidiness:
// it is what makes the failure mode "picked the wrong option from a list I
// wrote" rather than "invented a date format nobody parses". The deterministic
// planner already handles explicit cases; this exists for the residue, and the
// residue is where a free-form answer would do the most damage.
//
// WHY IT RUNS RARELY
//
// Measured on this machine: about three seconds to load the model on the first
// question, then 0.9 to 1.1 seconds per query and 0.7 to 4.2 seconds per file
// explanation. The earlier note here said "a few hundred milliseconds", which
// was optimistic by a factor of three against its own benchmark.
//
// At that price it cannot run on every keystroke, and it must not run on
// queries the deterministic pass already understood — there it could only
// disagree. It is asked when a query looks like a QUESTION and the cheap layers
// came back with little or nothing.
class QueryModel {
public:
    QueryModel() = default;
    ~QueryModel();

    QueryModel(const QueryModel&) = delete;
    QueryModel& operator=(const QueryModel&) = delete;

    // Loads on first call and does nothing thereafter. Safe from any thread:
    // the search thread reaches it through a query, the reclaim thread through
    // someone asking what a file is, and whichever arrives first pays.
    bool ensureLoaded(const std::wstring& modelPath, std::wstring* error);
    bool ready() const { return context_ != nullptr; }

    // True when this query is worth spending the model on.
    //
    // Deliberately conservative. A wrong answer here costs the user a confusing
    // result; a skipped call costs them nothing at all, because the query still
    // goes through the other four layers untouched.
    static bool worthAsking(const QueryPlan& plan);

    // Fills in what the deterministic pass could not. On any failure the plan
    // is left exactly as it was: the model is an improvement, never a
    // dependency.
    //
    // Gives up immediately if the model is busy describing a file. A search is
    // the thing the user is waiting on; making it queue behind an explanation
    // that can take four seconds would trade a certain delay for an optional
    // improvement.
    bool refine(const std::wstring& query, int64_t now, QueryPlan* plan);

    // One sentence saying what a file is, for the space screen.
    //
    // THIS IS THE MODEL'S OPINION AND MUST BE SHOWN AS SUCH.
    //
    // Everywhere else in Filo the model only picks from a list or is checked
    // against something. Here it writes prose that nothing verifies, about a
    // file the user is deciding whether to delete — so the interface labels it
    // as the model talking, and the reasons Filo is sure of stay separate.
    //
    // Only worth asking for files the rules could not already explain: if the
    // path says node_modules, a 3B model adds nothing but latency and a chance
    // to be wrong.
    bool describe(const std::wstring& path, uint64_t size, std::wstring* out);

    // Last raw reply, for diagnostics. Understanding why a query went wrong is
    // impossible without seeing what the model actually said.
    const std::string& lastReply() const { return lastReply_; }

private:
    bool load(const std::wstring& modelPath, std::wstring* error);

    // The body, with no exception guard. llama.cpp is a C API implemented in
    // C++ and it throws; anything escaping would cross the boundary into
    // std::terminate, which is how a five-hour indexing run died once already.
    bool refineUnguarded(const std::wstring& query, int64_t now, QueryPlan* plan);

    // Runs the model over a prompt. `grammar` may be null for free prose.
    bool generate(const std::string& prompt, const char* grammar, int maxTokens,
                  std::string* out);

    void* model_ = nullptr;
    void* context_ = nullptr;
    std::string lastReply_;

    // One context, two callers. A llama context is single-threaded, and the
    // search thread and the reclaim thread both want this one.
    std::mutex busy_;
    std::once_flag loadOnce_;
    bool loaded_ = false;
    std::wstring loadError_;
};

}  // namespace filo
