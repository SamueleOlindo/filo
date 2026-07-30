#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "index/file_index.h"
#include "store/content_db.h"

namespace filo {

// Native window that hosts the interface.
//
// The UI is HTML inside WebView2, but the SEARCH stays in C++: the JavaScript
// only sends the query and receives the results. Asking the JS engine to filter
// 2.4 million records would be an order of magnitude slower and would throw
// away the work the index does.
class WebViewWindow {
public:
    WebViewWindow() = default;
    ~WebViewWindow();

    WebViewWindow(const WebViewWindow&) = delete;
    WebViewWindow& operator=(const WebViewWindow&) = delete;

    // initialQuery pre-fills the search box at startup: the automated checks
    // need it, since otherwise they would have to synthesize keystrokes and
    // would end up typing into whichever window happens to be focused.
    // `content` may be null: search then covers names only.
    bool create(FileIndex* index, ContentDb* content, const std::wstring& statusLine,
                const std::wstring& initialQuery, std::wstring* error);
    int run();

    // Turns on semantic search. Optional in the strongest sense: if the model
    // is missing, or the vector file was built with a different model, chunker
    // or content generation, this returns false and the window still works —
    // with two result lists instead of three.
    //
    // Call before create(). Loading a model takes about a second, which is
    // acceptable at startup and would not be mid-keystroke.
    bool loadSemantic(const std::wstring& modelPath, const std::wstring& vectorPath,
                      uint32_t contentGeneration, std::wstring* error);
    bool semanticReady() const;

    // Points layer 5 at a generative model. Nothing is loaded here: the file is
    // opened the first time a query actually reads like a question, on the
    // search thread, so a 3B model never delays startup and is never paid for
    // by someone who only searches by keyword.
    void useQuestionModel(const std::wstring& modelPath);

    // What to do once files have really reached the Recycle Bin.
    //
    // Filo moved them, so nothing has to go looking to find out they are gone:
    // no rescan, no journal, and no administrator rights, because the
    // information never left the process. Without this the index keeps
    // returning them at their old path until the next launch — and on a machine
    // that cannot read the USN journal, until somebody rebuilds it by hand.
    //
    // Called on the reclaim thread, with the index locked for writing, and only
    // with the slots the shell confirmed one at a time. The window does not do
    // the work itself because main.cpp is what owns the index file, the
    // database and the paths to both.
    using AfterRecycle = std::function<void(const std::vector<uint32_t>& slots)>;
    void onRecycled(AfterRecycle callback);

    // How the index keeps up with changes Filo did not make itself.
    //
    // The journal used to be read exactly once, at startup, so every file
    // another program created or deleted during the session was invisible until
    // the next launch. Called on a timer with the index held for writing, it
    // should apply what it can and return how many entries it changed, or -1
    // when the journal cannot be read at all — which is the ordinary case
    // without administrator rights, and the reason a second source exists.
    //
    // Returning -1 is not an error to report to the user every minute. It is
    // answered by falling back to watching the folders that matter, which needs
    // no rights, and by saying so once in the status line.
    using Refresh = std::function<int64_t()>;
    void refreshWith(Refresh callback, unsigned everySeconds);

    // Folders to watch when the journal is out of reach. Watching needs no
    // privileges, and it covers the trees a person actually searches — their
    // own profile — rather than the whole volume.
    void watchFolders(std::vector<std::wstring> roots);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace filo
