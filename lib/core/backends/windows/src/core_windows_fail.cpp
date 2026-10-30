// The Windows backend of failure.

#include "pr/core_windows_fail.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

namespace pr_impl {
namespace {

constexpr USHORT kMaxFrames = 128;

// Writes the stack of the calling thread to the standard error, one frame a line as its module and offset, without its
// `skip` innermost frames.
void writeStack(ULONG skip) {
    void* frames[kMaxFrames];
    USHORT count = CaptureStackBackTrace(skip + 1, kMaxFrames, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        HMODULE module = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCSTR>(frames[i]), &module))
            GetModuleFileNameA(module, name, MAX_PATH);
        uintptr_t offset = reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(module);
        fprintf(stderr, "%-3u %s+0x%llx\n", unsigned(i), name, static_cast<unsigned long long>(offset));
    }
    fflush(stderr);
}

// Reports the exception, then lets it end the process as it would have.
LONG WINAPI crashed(EXCEPTION_POINTERS* exception) {
    const EXCEPTION_RECORD* record = exception->ExceptionRecord;
    fprintf(stderr, "crashed: exception 0x%08lx at address %p\n", record->ExceptionCode, record->ExceptionAddress);
    writeStack(0);
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace
} // namespace pr_impl

using namespace pr_impl;

void prCoreWindowsFailed(PrStringView message, PrStringView file, uint32_t line) {
    fprintf(stderr, "%.*s:%u: %.*s\n", int(file.size), file.data, line, int(message.size), message.data);
    fflush(stderr);
    writeStack(0);
    if (IsDebuggerPresent())
        __debugbreak();
    // The exit code is that of abort(). Ending the process directly skips the C runtime's abort dialog and the error
    // report.
    TerminateProcess(GetCurrentProcess(), 3);
    __assume(false);
}

void prCoreWindowsCrashHandler(void) {
    // A crash of a thread that overflows its stack is reported with this much of the stack kept back.
    ULONG reserve = 64 * 1024;
    SetThreadStackGuarantee(&reserve);
    SetUnhandledExceptionFilter(crashed);
}
