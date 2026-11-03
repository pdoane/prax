// The launcher of Windows processes, started with CreateProcessW.

#pragma once

#include "pr/base.h"
#include "pr/process.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a launcher of Windows processes, which open no console window. Each process has a thread of its own, which
// waits on its pipes and for it to exit. A process inherits the caller's environment, and no handle of the caller but
// its standard error, if its desc asks for it. prProcessKill calls TerminateProcess, and the process exits with 1.
PrProcessLauncher* prProcessWindowsCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
