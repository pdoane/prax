// Processes: the backends that the build has, and the public functions over the function tables.

#include "pr/process.h"

#include "pr/ascii.h"
#include "pr/assert.h"

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
        launcher->destroy(launcher);
}

PrStatus prProcessLaunch(PrProcessLauncher* launcher, const PrProcessDesc* desc, PrProcess* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrStatus status = launcher->launch(launcher, desc, out);
    PR_ASSERT((status == PrStatus_Ok) == (*out != nullptr));
    return status;
}

void prProcessDestroy(PrProcess* PR_NULLABLE process) {
    if (process != nullptr)
        process->destroy(process);
}

void prProcessWrite(PrProcess* process, PrByteView bytes) {
    if (bytes.size != 0)
        process->write(process, bytes);
}

void prProcessCloseInput(PrProcess* process) {
    process->closeInput(process);
}

size_t prProcessRead(PrProcess* process, PrProcessStream stream, uint8_t* PR_NULLABLE output, size_t capacity) {
    return process->read(process, stream, output, capacity);
}

PrProcessInfo prProcessInfo(const PrProcess* process) {
    return process->info(process);
}

void prProcessKill(PrProcess* process) {
    process->kill(process);
}
