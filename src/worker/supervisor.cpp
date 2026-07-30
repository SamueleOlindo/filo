#include "worker/supervisor.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace filo {
namespace {

// How long a file gets before we declare the worker stuck.
// The Job Object CANNOT be the watchdog: its time limit counts only user mode
// CPU time, so a parser sitting on a wait, or inside a loop the system
// considers legitimate, never trips it.
constexpr DWORD kTimeoutSmallMs = 10'000;
constexpr DWORD kTimeoutLargeMs = 30'000;
constexpr uint64_t kLargeFileBytes = 10ull << 20;

std::wstring ownExecutablePath() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

}  // namespace

struct ExtractorPool::Slot {
    unsigned index = 0;
    std::wstring name;

    HANDLE section = nullptr;
    HANDLE requestReady = nullptr;
    HANDLE responseReady = nullptr;
    char* view = nullptr;

    SharedHeader* header = nullptr;
    char* input = nullptr;
    char* output = nullptr;

    PROCESS_INFORMATION process = {};
    uint64_t sequence = 0;
};

ExtractorPool::~ExtractorPool() { stop(); }

bool ExtractorPool::start(unsigned workerCount, std::wstring* error) {
    stop();

    job_ = CreateJobObjectW(nullptr, nullptr);
    if (!job_) {
        *error = L"could not create the Job Object";
        return false;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags =
        // The decisive flag: if the coordinator dies in any way at all, even
        // by TerminateProcess, the workers die with it. No orphaned processes
        // left behind on the user's machine.
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
        JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION |
        JOB_OBJECT_LIMIT_PROCESS_MEMORY |
        JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.ProcessMemoryLimit = 512ull << 20;
    limits.BasicLimitInformation.ActiveProcessLimit = workerCount + 2;

    if (!SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits,
                                 sizeof(limits))) {
        *error = L"could not set the Job Object limits";
        return false;
    }

    // No access to the desktop, to the clipboard, to the global environment
    // variables: a parser has no reason to touch any of this.
    JOBOBJECT_BASIC_UI_RESTRICTIONS ui = {};
    ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS |
                             JOB_OBJECT_UILIMIT_EXITWINDOWS | JOB_OBJECT_UILIMIT_GLOBALATOMS |
                             JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_READCLIPBOARD |
                             JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_WRITECLIPBOARD;
    SetInformationJobObject(job_, JobObjectBasicUIRestrictions, &ui, sizeof(ui));

    slotLock_ = new CRITICAL_SECTION();
    InitializeCriticalSection(static_cast<CRITICAL_SECTION*>(slotLock_));
    available_ = CreateSemaphoreW(nullptr, 0, static_cast<LONG>(workerCount), nullptr);

    for (unsigned i = 0; i < workerCount; ++i) {
        auto* slot = new Slot();
        slot->index = i;

        wchar_t buffer[128];
        swprintf_s(buffer, L"Local\\filo-%lu-%u", GetCurrentProcessId(), i);
        slot->name = buffer;

        slot->section = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            static_cast<DWORD>(kSectionSize >> 32),
            static_cast<DWORD>(kSectionSize & 0xFFFFFFFF), (slot->name + L"-mem").c_str());
        slot->requestReady = CreateEventW(nullptr, FALSE, FALSE, (slot->name + L"-req").c_str());
        slot->responseReady = CreateEventW(nullptr, FALSE, FALSE, (slot->name + L"-res").c_str());

        if (!slot->section || !slot->requestReady || !slot->responseReady) {
            *error = L"could not create the communication objects";
            delete slot;
            return false;
        }

        slot->view = static_cast<char*>(MapViewOfFile(slot->section, FILE_MAP_ALL_ACCESS, 0, 0,
                                                     static_cast<SIZE_T>(kSectionSize)));
        if (!slot->view) {
            *error = L"could not map the shared memory";
            delete slot;
            return false;
        }
        slot->header = reinterpret_cast<SharedHeader*>(slot->view);
        slot->input = slot->view + kHeaderSize;
        slot->output = slot->input + kInputCapacity;

        if (!launch(*slot, error)) {
            delete slot;
            return false;
        }

        slots_.push_back(slot);
        freeList_.push_back(i);
        ReleaseSemaphore(available_, 1, nullptr);
    }
    return true;
}

bool ExtractorPool::launch(Slot& slot, std::wstring* error) {
    std::wstring commandLine = L"\"" + ownExecutablePath() + L"\" --extractor-worker \"" +
                               slot.name + L"\"";

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info = {};

    // CREATE_SUSPENDED is mandatory: between CreateProcess and the assignment
    // to the Job Object there is a window in which the process could already be
    // running and slip past the limits. This way it does not start until it is
    // inside the fence.
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | DETACHED_PROCESS,
                        nullptr, nullptr, &startup, &info)) {
        *error = L"could not start the worker process";
        return false;
    }

    if (!AssignProcessToJobObject(job_, info.hProcess)) {
        TerminateProcess(info.hProcess, 1);
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
        *error = L"could not assign the worker to the Job Object";
        return false;
    }

    ResumeThread(info.hThread);
    slot.process = info;
    return true;
}

void ExtractorPool::recycle(Slot& slot) {
    if (slot.process.hProcess) {
        TerminateProcess(slot.process.hProcess, 1);
        WaitForSingleObject(slot.process.hProcess, 2000);
        CloseHandle(slot.process.hThread);
        CloseHandle(slot.process.hProcess);
        slot.process = {};
    }
    // The events may have been left signalled by a late reply.
    ResetEvent(slot.requestReady);
    ResetEvent(slot.responseReady);

    std::wstring ignored;
    if (launch(slot, &ignored)) ++stats_.restarts;
}

void ExtractorPool::stop() {
    for (Slot* slot : slots_) {
        if (slot->process.hProcess) {
            TerminateProcess(slot->process.hProcess, 0);
            CloseHandle(slot->process.hThread);
            CloseHandle(slot->process.hProcess);
        }
        if (slot->view) UnmapViewOfFile(slot->view);
        if (slot->section) CloseHandle(slot->section);
        if (slot->requestReady) CloseHandle(slot->requestReady);
        if (slot->responseReady) CloseHandle(slot->responseReady);
        delete slot;
    }
    slots_.clear();
    freeList_.clear();

    if (available_) { CloseHandle(available_); available_ = nullptr; }
    if (job_) { CloseHandle(job_); job_ = nullptr; }
    if (slotLock_) {
        DeleteCriticalSection(static_cast<CRITICAL_SECTION*>(slotLock_));
        delete static_cast<CRITICAL_SECTION*>(slotLock_);
        slotLock_ = nullptr;
    }
}

WorkerStatus ExtractorPool::extract(ContentKind kind, const char* data, size_t size,
                                    std::string* text, uint32_t fault) {
    text->clear();
    if (slots_.empty()) return WorkerStatus::Unavailable;
    if (size > kInputCapacity) return WorkerStatus::TooBig;

    if (WaitForSingleObject(available_, 60'000) != WAIT_OBJECT_0) {
        return WorkerStatus::Unavailable;
    }

    unsigned index = 0;
    auto* lock = static_cast<CRITICAL_SECTION*>(slotLock_);
    EnterCriticalSection(lock);
    index = freeList_.back();
    freeList_.pop_back();
    ++stats_.requests;
    LeaveCriticalSection(lock);

    Slot& slot = *slots_[index];
    WorkerStatus status = WorkerStatus::Failed;

    slot.header->magic = kProtocolMagic;
    slot.header->version = kProtocolVersion;
    slot.header->kind = static_cast<uint32_t>(kind);
    slot.header->fault = fault;
    slot.header->inputSize = size;
    slot.header->outputSize = 0;
    slot.header->status = static_cast<uint32_t>(WorkerStatus::Idle);
    slot.header->sequence = ++slot.sequence;
    if (size) std::memcpy(slot.input, data, size);

    SetEvent(slot.requestReady);

    // We wait for the reply OR for the death of the process. Waiting only for
    // the reply would mean sitting there until the timeout even when the worker
    // is already dead, and death is exactly the case to handle quickly.
    const DWORD timeout = size > kLargeFileBytes ? kTimeoutLargeMs : kTimeoutSmallMs;
    HANDLE waits[2] = {slot.responseReady, slot.process.hProcess};
    const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, timeout);

    if (wait == WAIT_OBJECT_0) {
        status = static_cast<WorkerStatus>(slot.header->status);
        if (status == WorkerStatus::Crashed) {
            ++stats_.crashes;
            recycle(slot);
        } else {
            if (slot.header->outputSize > 0 && slot.header->outputSize <= kOutputCapacity) {
                text->assign(slot.output, static_cast<size_t>(slot.header->outputSize));
            }
            if (status == WorkerStatus::Ok) ++stats_.ok;
        }
    } else if (wait == WAIT_OBJECT_0 + 1) {
        // The process died without answering: the SEH guard caught nothing, so
        // it was a fault that does not go through there (heap corruption,
        // __fastfail, the Job memory limit).
        ++stats_.crashes;
        status = WorkerStatus::Crashed;
        recycle(slot);
    } else {
        ++stats_.timeouts;
        status = WorkerStatus::Timeout;
        recycle(slot);
    }

    EnterCriticalSection(lock);
    freeList_.push_back(index);
    LeaveCriticalSection(lock);
    ReleaseSemaphore(available_, 1, nullptr);

    return status;
}

const wchar_t* workerStatusName(WorkerStatus status) {
    switch (status) {
        case WorkerStatus::Idle:        return L"idle";
        case WorkerStatus::Ok:          return L"ok";
        case WorkerStatus::Empty:       return L"no text";
        case WorkerStatus::Failed:      return L"failed";
        case WorkerStatus::TooBig:      return L"truncated";
        case WorkerStatus::Encrypted:   return L"protected";
        case WorkerStatus::NeedsOcr:    return L"images only";
        case WorkerStatus::Crashed:     return L"worker died";
        case WorkerStatus::Timeout:     return L"worker stuck";
        case WorkerStatus::Unavailable: return L"no worker";
    }
    return L"?";
}

}  // namespace filo
