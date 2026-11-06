// Shell integration: writes the embedded scripts to the platform's cache directory, and builds the launch of each shell
// that loads them.

#include "studio/terminal/src/terminal_shell.h"

#include "pr/count_of.h"
#include "pr/environment.h"
#include "pr/file.h"
#include "pr/mem.h"
#include "pr/string_ops.h"

#include "studio/terminal/shell/kTerminalShellScripts.h"

namespace pr_impl {

namespace {

// The shells that the terminal has integrations for.
enum {
    Shell_None,
    Shell_Zsh,
    Shell_Bash,
    Shell_Fish,
};
typedef uint8_t Shell;

// The shell that `program` runs, judged by its file name.
Shell shellOf(PrStringView program) {
    PrStringView name = prFilePathName(program);
    if (name == "zsh")
        return Shell_Zsh;
    if (name == "bash")
        return Shell_Bash;
    if (name == "fish")
        return Shell_Fish;
    return Shell_None;
}

// Returns `directory` joined to `part` with a slash.
PrRawString pathJoin(PrStringView directory, PrStringView part) {
    PrRawString path = prRawStringCopy(directory);
    prRawStringAppend(&path, "/");
    prRawStringAppend(&path, part);
    return path;
}

// Returns true if the file at `path` holds `bytes`.
bool fileHolds(PrStringView path, PrByteView bytes) {
    PrByteView mapping{};
    if (!prFileMap(path, &mapping))
        return false;
    bool same = mapping.size == bytes.size && prMemCompare(mapping.data, bytes.data, bytes.size) == 0;
    prFileUnmap(&mapping);
    return same;
}

// Writes each script under `directory` whose file there differs. Returns false if a write fails. A shell that is
// reading a script sees either the old file or the new one, never a mix, because prFileWrite writes a new file and then
// renames it over the old one.
bool scriptsWrite(PrStringView directory) {
    bool written = true;
    for (uint32_t i = 0; i < countOf(kTerminalShellScriptsPaths); ++i) {
        PrRawString path = pathJoin(directory, kTerminalShellScriptsPaths[i]);
        PrStringView view = prRawStringView(&path);
        PrStringView script = kTerminalShellScriptsBytes[i];
        PrByteView bytes{reinterpret_cast<const uint8_t*>(script.data), script.size};
        if (!fileHolds(view, bytes))
            written = written && prFileCreateDirectory(prFilePathParent(view)) && prFileWrite(view, bytes);
        prRawStringReset(&path);
    }
    return written;
}

void argumentAdd(TerminalShellLaunch* launch, PrStringView argument) {
    launch->arguments.push(prRawStringCopy(argument));
}

// Adds the variable `name` with `value` to the launch's environment.
void variableAdd(TerminalShellLaunch* launch, PrStringView name, PrStringView value) {
    PrRawString entry = prRawStringCopy(name);
    prRawStringAppend(&entry, "=");
    prRawStringAppend(&entry, value);
    launch->environment.push(entry);
}

} // namespace

PrRawString terminalShellVariable(PrStringView name, const PrStringView* PR_NULLABLE entries, uint32_t count) {
    for (uint32_t i = count; i > 0; --i) {
        PrStringView entry = entries[i - 1];
        if (entry.size > name.size && entry.data[name.size] == '=' && prStringStartsWith(entry, name))
            return prRawStringCopy(PrStringView{entry.data + name.size + 1, entry.size - name.size - 1});
    }
    return prEnvironmentVariable(name);
}

PrRawString terminalShellDirectory(void) {
    PrRawString cache = prFileUserDirectory(PrFileUserDirectory_Cache);
    if (prRawStringIsNull(&cache))
        return cache;
    PrRawString directory = pathJoin(prRawStringView(&cache), "prax/shell-integration");
    prRawStringReset(&cache);
    return directory;
}

bool terminalShellIntegrate(PrStringView program, PrStringView directory, const PrStringView* PR_NULLABLE entries,
        uint32_t count, TerminalShellLaunch* launch) {
    Shell shell = shellOf(program);
    if (shell == Shell_None)
        return false;
    if (shell == Shell_Bash) {
        // bash in POSIX mode ignores --rcfile.
        PrRawString posix = terminalShellVariable("POSIXLY_CORRECT", entries, count);
        bool strict = !prRawStringIsEmpty(&posix);
        prRawStringReset(&posix);
        if (strict)
            return false;
    }
    if (!scriptsWrite(directory))
        return false;
    switch (shell) {
        case Shell_Zsh: {
            // zsh reads its startup files from ZDOTDIR. The integration's .zshenv restores the user's ZDOTDIR.
            PrRawString user = terminalShellVariable("ZDOTDIR", entries, count);
            PrRawString own = pathJoin(directory, "zsh");
            argumentAdd(launch, "-l");
            variableAdd(launch, "ZDOTDIR", prRawStringView(&own));
            if (!prRawStringIsEmpty(&user))
                variableAdd(launch, "PRAX_ZSH_ZDOTDIR", prRawStringView(&user));
            prRawStringReset(&own);
            prRawStringReset(&user);
            break;
        }
        case Shell_Bash: {
            // A login bash ignores --rcfile, so bash starts as a non-login shell, and the rcfile reads the login files.
            PrRawString rcfile = pathJoin(directory, "bash/prax.bash");
            argumentAdd(launch, "--rcfile");
            argumentAdd(launch, prRawStringView(&rcfile));
            variableAdd(launch, "PRAX_BASH_LOGIN", "1");
            prRawStringReset(&rcfile);
            break;
        }
        case Shell_Fish: {
            // fish reads vendor_conf.d in each directory of XDG_DATA_DIRS. If the variable is unset, the default is the
            // one in the XDG Base Directory Specification.
            PrRawString user = terminalShellVariable("XDG_DATA_DIRS", entries, count);
            PrRawString dirs = prRawStringCopy(directory);
            prRawStringAppend(&dirs, ":");
            bool set = !prRawStringIsEmpty(&user);
            prRawStringAppend(&dirs, set ? prRawStringView(&user) : PrStringView("/usr/local/share:/usr/share"));
            argumentAdd(launch, "-l");
            variableAdd(launch, "XDG_DATA_DIRS", prRawStringView(&dirs));
            if (set)
                variableAdd(launch, "PRAX_FISH_XDG_DATA_DIRS", prRawStringView(&user));
            prRawStringReset(&dirs);
            prRawStringReset(&user);
            break;
        }
        default:
            break;
    }
    return true;
}

void terminalShellLaunchReset(TerminalShellLaunch* launch) {
    for (PrRawString& argument : launch->arguments.span())
        prRawStringReset(&argument);
    launch->arguments.reset();
    for (PrRawString& entry : launch->environment.span())
        prRawStringReset(&entry);
    launch->environment.reset();
}

} // namespace pr_impl
