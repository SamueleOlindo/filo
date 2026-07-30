// The structured exception guard, in C and not in C++ on purpose.
//
// MSVC's __try/__except catches STRUCTURED exceptions (access violations,
// divide by zero, stack overflow), which are what a parser produces on a
// malformed file. C++ exceptions have nothing to do with it.
//
// Why a .c file: in C++, mixing SEH and destructors requires /EHa, which
// blocks some optimizations and changes the destruction semantics of objects
// across the whole translation unit. In C the problem does not exist, because
// there are no destructors, and the rest of the project stays on /EHsc.
//
// This is the FIRST of the three layers of containment, and on its own it is
// not enough: it does not see heap corruption (the damage is already done, the
// crash lands later, in innocent code), it catches neither __fastfail nor the
// stack integrity checks, and it does not stop an infinite loop. The other two
// layers are the Job Object and the watchdog in the coordinator.

#include <windows.h>

// Implemented in C++ (extract/api.cpp).
int filoExtractRaw(unsigned int kind, const char* input, unsigned long long inputSize,
                   char* output, unsigned long long outputCapacity,
                   unsigned long long* outputSize, unsigned int fault);

int filoExtractGuarded(unsigned int kind, const char* input, unsigned long long inputSize,
                       char* output, unsigned long long outputCapacity,
                       unsigned long long* outputSize, unsigned int fault,
                       unsigned long* exceptionCode) {
    __try {
        return filoExtractRaw(kind, input, inputSize, output, outputCapacity, outputSize,
                              fault);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (exceptionCode) *exceptionCode = GetExceptionCode();
        return -1;
    }
}
