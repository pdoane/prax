// Shell integration: scripts that make zsh, bash and fish write semantic prompt marks (OSC 133) and their working
// directory (OSC 7), and the arguments and environment that start each shell so that it loads its script after the
// user's own startup files.
//
// The scripts are embedded in the binary. When a shell starts, they are written to their own directory under the
// platform's cache directory, so that a shell finds them wherever the application runs from. Each shell finds its
// script through an environment variable or an argument: zsh through ZDOTDIR, bash through --rcfile, and fish through
// XDG_DATA_DIRS. Each script restores the variable's original value, so that the shell's children see the user's
// environment.

#pragma once

#include "pr/base.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The arguments and environment variables, each NAME=value, that start a shell with its integration. The shell behaves
// as a login shell. The arguments replace the caller's.
struct TerminalShellLaunch {
    RawArray<PrRawString> arguments;
    RawArray<PrRawString> environment;
};

// Returns the value of the environment variable `name`. `entries`, each NAME=value, take precedence over this process's
// environment, and the last entry for a name wins. Returns the empty string if the variable is unset or empty. The
// caller releases the string with prRawStringReset.
PrRawString terminalShellVariable(PrStringView name, const PrStringView* PR_NULLABLE entries, uint32_t count);

// The directory that the scripts are written to, under the platform's cache directory, or the null string if the
// platform has no cache directory. The caller releases the string with prRawStringReset.
PrRawString terminalShellDirectory(void);

// Writes the scripts under `directory`, skipping each file that already holds the same bytes. Then fills `launch` with
// what starts the shell `program`, a path or a name, with its integration. `entries` are the caller's environment
// variables, as for terminalShellVariable. Returns false, and leaves `launch` empty, if the terminal has no integration
// for the shell, or if a script cannot be written.
bool terminalShellIntegrate(PrStringView program, PrStringView directory, const PrStringView* PR_NULLABLE entries,
    uint32_t count, TerminalShellLaunch* launch);

// Frees the strings and arrays of `launch`.
void terminalShellLaunchReset(TerminalShellLaunch* launch);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
