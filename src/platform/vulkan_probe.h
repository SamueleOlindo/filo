#pragma once

namespace filo {

// Whether this machine can use the GPU at all.
//
// vulkan-1.dll ships with the graphics driver, not with Windows. On an office
// PC, a virtual machine or a laptop with a stale driver it is simply not there.
//
// Filo used to import it statically, which meant the Windows loader resolved it
// BEFORE any of our code ran: the process was refused outright, with a system
// dialog, and the CPU fallback we had written could never execute because the
// code that chose it never started. It is delay-loaded now, so the program
// starts either way and this function decides what to do about it.
//
// FILO_PRETEND_NO_VULKAN makes this report false on a machine that does have
// it, which is the only way to exercise the fallback without finding a computer
// without a GPU driver.
bool vulkanAvailable();

// Whether a delay-loaded Vulkan symbol has actually failed to resolve. Distinct
// from the question above: this one is about what happened, not what is
// possible.
bool vulkanFailedToLoad();

}  // namespace filo
