"""What the commands of dev.sh share: running programs, failing with a message, and the platform."""

import os
import shutil
import subprocess
import sys

import tools

ROOT = tools.ROOT

# Windows: the commands run from Git Bash, with Windows' Python, and build with MSVC (tools/build/build.py).
WINDOWS = os.name == "nt"


def fail(message):
    """Prints `message` as an error and exits with a failure. What was printed before it comes first. Under GitHub
    Actions, the message is also the step's error annotation, which the run's summary shows."""
    sys.stdout.flush()
    print("dev.sh: error: " + message, file=sys.stderr, flush=True)
    if os.environ.get("GITHUB_ACTIONS") == "true":
        # A workflow command's message is one line, with its percent signs and line breaks escaped.
        escaped = message.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
        print("::error title=dev.sh::" + escaped, flush=True)
    sys.exit(1)


def run(command, cwd=ROOT, env=None, check=True, stdout=None, stderr=None):
    """Runs `command` in `cwd`, with `env` added to the environment, and returns its exit status. A failure exits with
    the command's status unless `check` is false."""
    environment = None
    if env:
        environment = dict(os.environ)
        environment.update({name: str(value) for name, value in env.items()})
    status = subprocess.run([str(part) for part in command], cwd=cwd, env=environment, stdout=stdout,
                            stderr=stderr).returncode
    if check and status != 0:
        sys.exit(status)
    return status


def output(command, cwd=ROOT, check=True):
    """Returns what `command` prints, without its final newline. A failure exits unless `check` is false, and then
    returns None."""
    done = subprocess.run([str(part) for part in command], cwd=cwd, capture_output=True, text=True)
    if done.returncode != 0:
        if check:
            sys.stderr.write(done.stderr)
            sys.exit(done.returncode)
        return None
    return done.stdout.rstrip("\n")


def python(module, *args, cwd=ROOT, env=None, check=True):
    """Runs a module of the tools with the arguments, as `python3 -m <module>` from `cwd`."""
    return run([sys.executable, "-m", module, *args], cwd=cwd, env=env, check=check)


def executable(directory, name, wasm=False, ios=False):
    """Returns the path of the program `name` in `directory`, with the platform's extension, or the extension of
    Emscripten's script that loads a WebAssembly program. On macOS, a program built as an application bundle runs from
    inside the bundle, and an iOS bundle holds its program at its top."""
    if ios:
        return os.path.join(directory, name + ".app", name)
    if not wasm and darwin():
        bundled = os.path.join(directory, name + ".app", "Contents", "MacOS", name)
        if os.path.exists(bundled):
            return bundled
    return os.path.join(directory, name + (".js" if wasm else ".exe" if WINDOWS else ""))


def has(program):
    """Returns true if `program` is on the path."""
    return shutil.which(program) is not None


def darwin():
    """Returns true on macOS."""
    return sys.platform == "darwin"


def usage(argv, text, code=None):
    """Prints a command's usage, `text`, and exits, if `argv` asks for help, or with `code` if it is not None."""
    if code is not None or argv[:1] in (["-h"], ["--help"]):
        print(text, file=sys.stderr if code else sys.stdout)
        sys.exit(code or 0)
