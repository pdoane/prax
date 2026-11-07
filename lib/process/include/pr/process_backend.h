// The interface that process backends implement. Callers use the functions of pr/process.h, which call these. Only
// backends include this header.

#pragma once

#include "pr/base.h"
#include "pr/process.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The functions of a launcher's backend, at the start of its state. Each implements the function of pr/process.h that
// it is named for.
struct PrProcessLauncher {
    void (*destroyFn)(PrProcessLauncher* launcher);
    PrStatus (*launchFn)(PrProcessLauncher* launcher, const PrProcessDesc* desc, PrProcess* PR_NULLABLE* PR_NONNULL out);
};

// The functions of a process's backend, at the start of its state. Each implements the function of pr/process.h that it
// is named for.
struct PrProcess {
    void (*destroyFn)(PrProcess* process);
    void (*writeFn)(PrProcess* process, PrByteView bytes);
    void (*closeInputFn)(PrProcess* process);
    size_t (*readFn)(PrProcess* process, PrProcessStream stream, uint8_t* PR_NULLABLE output, size_t capacity);
    PrProcessInfo (*infoFn)(const PrProcess* process);
    void (*killFn)(PrProcess* process);
    void (*terminalResizeFn)(PrProcess* process, PrUVec2 extent, PrUVec2 pixelExtent);
    PrRawString (*terminalForegroundDirectoryFn)(const PrProcess* process);
};

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
