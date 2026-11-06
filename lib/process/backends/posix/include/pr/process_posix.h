// The launcher of POSIX processes, started with posix_spawn.

#pragma once

#include "pr/base.h"
#include "pr/process.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a launcher of POSIX processes. Each process has a thread of its own, which polls its pipes or terminal and
// waits for the process to exit. A process inherits the caller's environment, with the variables that its desc sets
// replacing those of the same name. It inherits no descriptor of the caller except standard error, and that only if its
// desc asks for it. A process on a terminal leads a new session, with the terminal as its controlling terminal.
// prProcessTerminalForegroundDirectory reads /proc on Linux and uses proc_pidinfo on a Mac. On other systems it returns
// the null string. prProcessKill sends SIGKILL.
PrProcessLauncher* prProcessPosixCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
