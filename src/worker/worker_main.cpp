#include "worker/worker_main.h"

#include <windows.h>

#include <string>

#include "worker/protocol.h"

// Defined in seh_guard.c, compiled /EHa.
extern "C" int filoExtractGuarded(unsigned int kind, const char* input,
                                  unsigned long long inputSize, char* output,
                                  unsigned long long outputCapacity,
                                  unsigned long long* outputSize, unsigned int fault,
                                  unsigned long* exceptionCode);

namespace filo {

int workerMain(const wchar_t* channelName) {
    const std::wstring base = channelName;

    // No error dialog, nothing sent to Microsoft: a crash here is expected and
    // is for the coordinator to handle, not something to show the user.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    HANDLE section = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, (base + L"-mem").c_str());
    HANDLE requestReady = OpenEventW(SYNCHRONIZE, FALSE, (base + L"-req").c_str());
    HANDLE responseReady = OpenEventW(EVENT_MODIFY_STATE, FALSE, (base + L"-res").c_str());
    if (!section || !requestReady || !responseReady) return 2;

    auto* view = static_cast<char*>(
        MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, static_cast<SIZE_T>(kSectionSize)));
    if (!view) return 3;

    auto* header = reinterpret_cast<SharedHeader*>(view);
    char* const inputArea = view + kHeaderSize;
    char* const outputArea = inputArea + kInputCapacity;

    for (;;) {
        // Wait with no deadline: when the coordinator shuts down, the Job
        // Object takes us with it. No other exit mechanism is needed.
        if (WaitForSingleObject(requestReady, INFINITE) != WAIT_OBJECT_0) break;

        if (header->magic != kProtocolMagic || header->version != kProtocolVersion) {
            header->status = static_cast<uint32_t>(WorkerStatus::Failed);
            header->outputSize = 0;
            SetEvent(responseReady);
            continue;
        }

        unsigned long long produced = 0;
        unsigned long exceptionCode = 0;
        const int result = filoExtractGuarded(
            header->kind, inputArea, header->inputSize, outputArea, kOutputCapacity,
            &produced, header->fault, &exceptionCode);

        if (result < 0) {
            // The guard caught a structured exception. We answer and then we
            // EXIT: after a fault like that the state of the process is no
            // longer trustworthy, and output produced from a corrupted state
            // must not end up in the index.
            header->status = static_cast<uint32_t>(WorkerStatus::Crashed);
            header->outputSize = 0;
            SetEvent(responseReady);
            return 4;
        }

        header->outputSize = produced;
        header->status = static_cast<uint32_t>(result);
        SetEvent(responseReady);
    }

    UnmapViewOfFile(view);
    CloseHandle(section);
    CloseHandle(requestReady);
    CloseHandle(responseReady);
    return 0;
}

}  // namespace filo
