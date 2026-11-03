// The launcher of POSIX processes, started with posix_spawn.

#pragma once

#include "pr/base.h"
#include "pr/process.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a launcher of POSIX processes. Each process has a thread of its own, which polls its pipes and waits for it
// to exit. A process inherits the caller's environment, and no descriptor of the caller but its standard error, if its
// desc asks for it. prProcessKill sends SIGKILL.
PrProcessLauncher* prProcessPosixCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
