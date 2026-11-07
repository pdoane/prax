// Processes: the backends that the build has, and the public functions over the function tables.

#include "pr/process.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/process_backend.h"
#include "pr/string_ops.h"

#if defined(PR_PROCESS_HAS_POSIX)
#include "pr/process_posix.h"
#endif
#if defined(PR_PROCESS_HAS_WINDOWS)
#include "pr/process_windows.h"
#endif

namespace pr_impl {

namespace {

constexpr PrStringView kBackendNames[PrProcessBackend_Count] = {"default", "posix", "windows"};

// The backends that the build has, which Default creates the first of, and then Default, so that a build without
// backends creates none.
constexpr PrProcessBackend kBuilt[] = {
#if defined(PR_PROCESS_HAS_POSIX)
    PrProcessBackend_Posix,
#endif
#if defined(PR_PROCESS_HAS_WINDOWS)
    PrProcessBackend_Windows,
#endif
    PrProcessBackend_Default,
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrProcessBackend prProcessBackendFromName(PrStringView name) {
    for (PrProcessBackend backend = 0; backend < PrProcessBackend_Count; ++backend) {
        if (prAsciiEqualsFold(kBackendNames[backend], name))
            return backend;
    }
    return PrProcessBackend_Count;
}

PrStringView prProcessBackendName(PrProcessBackend backend) {
    return backend < PrProcessBackend_Count ? kBackendNames[backend] : PrStringView{};
}

bool prProcessBackendIsBuilt(PrProcessBackend backend) {
    for (PrProcessBackend built : kBuilt) {
        if (built != PrProcessBackend_Default && (built == backend || backend == PrProcessBackend_Default))
            return true;
    }
    return false;
}

PrProcessLauncher* PR_NULLABLE prProcessLauncherCreate(PrProcessBackend backend) {
    switch (backend == PrProcessBackend_Default ? kBuilt[0] : backend) {
#if defined(PR_PROCESS_HAS_POSIX)
        case PrProcessBackend_Posix: return prProcessPosixCreate();
#endif
#if defined(PR_PROCESS_HAS_WINDOWS)
        case PrProcessBackend_Windows: return prProcessWindowsCreate();
#endif
        default: return nullptr;
    }
}

void prProcessLauncherDestroy(PrProcessLauncher* PR_NULLABLE launcher) {
    if (launcher != nullptr)
        launcher->destroyFn(launcher);
}

PrStatus prProcessLaunch(PrProcessLauncher* launcher, const PrProcessDesc* desc, PrProcess* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PR_ASSERT(desc->streams != PrProcessStreams_Terminal || (desc->terminalExtent.x > 0 && desc->terminalExtent.y > 0));
    for (uint32_t i = 0; i < desc->environmentCount; ++i) {
        PrStringView variable = desc->environment[i];
        PR_ASSERT(prStringFindByte(variable, '=') != 0 && prStringFindByte(variable, '=') < variable.size);
    }
    PrStatus status = launcher->launchFn(launcher, desc, out);
    PR_ASSERT((status == PrStatus_Ok) == (*out != nullptr));
    return status;
}

void prProcessDestroy(PrProcess* PR_NULLABLE process) {
    if (process != nullptr)
        process->destroyFn(process);
}

void prProcessWrite(PrProcess* process, PrByteView bytes) {
    if (bytes.size != 0)
        process->writeFn(process, bytes);
}

void prProcessCloseInput(PrProcess* process) {
    process->closeInputFn(process);
}

size_t prProcessRead(PrProcess* process, PrProcessStream stream, uint8_t* PR_NULLABLE output, size_t capacity) {
    return process->readFn(process, stream, output, capacity);
}

PrProcessInfo prProcessInfo(const PrProcess* process) {
    return process->infoFn(process);
}

void prProcessKill(PrProcess* process) {
    process->killFn(process);
}

void prProcessTerminalResize(PrProcess* process, PrUVec2 extent, PrUVec2 pixelExtent) {
    PR_ASSERT(extent.x > 0 && extent.y > 0);
    PR_ASSERT(process->terminalResizeFn != nullptr);
    process->terminalResizeFn(process, extent, pixelExtent);
}

PrRawString prProcessTerminalForegroundDirectory(const PrProcess* process) {
    if (process->terminalForegroundDirectoryFn == nullptr)
        return prRawStringNull();
    return process->terminalForegroundDirectoryFn(process);
}
