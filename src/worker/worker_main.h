#pragma once

namespace filo {

// Body of the worker process. `channelName` is the base name of the
// synchronization objects, handed over by the coordinator on the command line.
// Returns the process exit code.
int workerMain(const wchar_t* channelName);

}  // namespace filo
