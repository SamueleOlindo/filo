#include "platform/vulkan_probe.h"

#include <windows.h>
#include <delayimp.h>

#include <cstring>

namespace filo {
namespace {

bool g_vulkanMissing = false;

bool pretendMissing() {
    return GetEnvironmentVariableW(L"FILO_PRETEND_NO_VULKAN", nullptr, 0) > 0;
}

// VK_ERROR_INITIALIZATION_FAILED. Not including vulkan.h for one number.
constexpr int kVulkanInitializationFailed = -3;

// Stands in for any Vulkan entry point when the loader is absent, and reports
// the one failure every Vulkan caller already knows how to handle.
extern "C" int filoVulkanAlwaysFails() { return kVulkanInitializationFailed; }

// Stands in for vkGetInstanceProcAddr, which returns a POINTER rather than a
// result: handing back -3 here would produce an address that gets called.
// Every symbol it is asked for resolves to the failing stub instead.
extern "C" void* filoVulkanGetProcAddr(void*, const char*) {
    return reinterpret_cast<void*>(&filoVulkanAlwaysFails);
}

// Called by the delay-load helper when vulkan-1.dll cannot be resolved.
//
// RETURNING NULL HERE KILLS THE PROCESS, and that was measured, not assumed:
// the helper raises a structured exception (0xC06D007F), and ggml's catch-all
// around its Vulkan initialization does not catch structured exceptions. The
// delay-load alone lets the program START without a GPU driver and then kills
// it a second later, which is not an improvement.
//
// So instead of failing, the missing library is answered with stubs. The first
// real call — vkCreateInstance, through vulkan.hpp's dynamic dispatcher —
// returns VK_ERROR_INITIALIZATION_FAILED, vulkan.hpp turns that into a
// vk::SystemError, and ggml_backend_vk_reg() catches it and reports that there
// is no Vulkan backend. That path was already written and already correct; all
// this does is route a missing DLL into it.
FARPROC WINAPI onDelayLoadFailure(unsigned reason, PDelayLoadInfo info) {
    if (!info || !info->szDll || _stricmp(info->szDll, "vulkan-1.dll") != 0) {
        return nullptr;
    }
    g_vulkanMissing = true;

    if (reason == dliFailLoadLib) {
        // Any real module will do: nothing is ever called through it, but the
        // helper needs a handle to carry on to symbol resolution, which is
        // where the stubs take over.
        return reinterpret_cast<FARPROC>(GetModuleHandleW(L"kernel32.dll"));
    }
    if (reason == dliFailGetProc) {
        const char* name = info->dlp.fImportByName ? info->dlp.szProcName : nullptr;
        if (name && std::strcmp(name, "vkGetInstanceProcAddr") == 0) {
            return reinterpret_cast<FARPROC>(&filoVulkanGetProcAddr);
        }
        return reinterpret_cast<FARPROC>(&filoVulkanAlwaysFails);
    }
    return nullptr;
}

// Test harness: makes vulkan-1.dll behave as if it were not installed.
//
// The failure path is the one that matters and the one that cannot be reached
// on a machine that has a GPU driver — which is every machine this was written
// on. Under FILO_PRETEND_NO_VULKAN the loader is handed a module that exists
// but exports no Vulkan symbol, so the resolution fails exactly where it would
// fail on a computer without the loader, and the fallback gets exercised for
// real instead of being reasoned about.
FARPROC WINAPI onDelayLoadNotify(unsigned reason, PDelayLoadInfo info) {
    if (reason == dliNotePreLoadLibrary && pretendMissing() && info && info->szDll &&
        _stricmp(info->szDll, "vulkan-1.dll") == 0) {
        return reinterpret_cast<FARPROC>(GetModuleHandleW(L"kernel32.dll"));
    }
    return nullptr;
}

}  // namespace

extern "C" const PfnDliHook __pfnDliNotifyHook2 = onDelayLoadNotify;

// delayimp.lib declares this const and ships a default of nullptr; a program
// installs its own by DEFINING the symbol, which the linker then prefers over
// the library's. Assigning to it at runtime is rejected outright.
extern "C" const PfnDliHook __pfnDliFailureHook2 = onDelayLoadFailure;

bool vulkanAvailable() {
    if (pretendMissing()) return false;

    // LoadLibrary rather than trusting the delay-load hook to have fired: the
    // hook only speaks once something has already tried to use Vulkan, and this
    // question gets asked before that.
    const HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
    if (!loader) return false;
    FreeLibrary(loader);
    return !g_vulkanMissing;
}

bool vulkanFailedToLoad() { return g_vulkanMissing; }

}  // namespace filo
