// The launcher of Windows processes, started with CreateProcessW.

#pragma once

#include "pr/base.h"
#include "pr/process.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a launcher of Windows processes, which open no console window. Each process has a thread of its own, which
// waits on its pipes and for the process to exit. A process inherits the caller's environment, with the variables that
// its desc sets replacing those of the same name. It inherits no handle of the caller except standard error, and that
// only if its desc asks for it.
//
// A process on a terminal runs on a pseudoconsole (ConPTY), which translates console output to terminal escape
// sequences. A pseudoconsole does not report its foreground program, so prProcessTerminalForegroundDirectory returns
// the null string. A pseudoconsole has no pixel size, so prProcessTerminalResize ignores `pixelExtent`.
//
// prProcessKill calls TerminateProcess, and the process exits with 1.
PrProcessLauncher* prProcessWindowsCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
