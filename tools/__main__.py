"""The developer workflow, which ./dev.sh runs: `./dev.sh help` lists the commands, and `./dev.sh <command> --help`
describes one. With no command, it prints the everyday commands and the state of the builds.

Nothing under third_party is committed. The build fetches what it and its tests read into third_party/downloads
(cmake/dependencies.cmake, cmake/test_data.cmake), and every other command fetches the checkouts it needs, such as a
suite's corpus (tools/fetch/checkouts.py). Benchmark results are kept under perf, which is not committed either.
"""

import importlib
import sys


# The commands, in the order that help lists them, each by the module that defines it.
COMMANDS = {
    "build": "tools.build.commands",
    "test": "tools.build.commands",
    "ci": "tools.build.commands",
    "suite": "tools.suite.commands",
    "bench": "tools.bench.commands",
    "profile": "tools.bench.commands",
    "probe": "tools.browsers.commands",
    "prax": "tools.suite.prax.commands",
    "markdown": "tools.suite.markdown.commands",
    "pages": "tools.pages.commands",
    "spec": "tools.spec.commands",
    "gen": "tools.gen.commands",
    "lint": "tools.lint.commands",
    "package": "tools.package.commands",
    "rewrap": "tools.lint.commands",
    "includes": "tools.lint.commands",
    "layout": "tools.lint.commands",
    "prose": "tools.lint.commands",
    "viewer": "tools.viewer.commands",
    "document": "tools.shell.commands",
    "example": "tools.build.commands",
    "app": "tools.build.commands",
    "fetch": "tools.fetch.commands",
    "clean": "tools.build.commands",
}


# The everyday commands, which ./dev.sh with no command prints.
EVERYDAY = (
    ("test", "lint, build, and run the unit tests and the tools' tests on the host: the fast loop"),
    ("ci [asan]", "lint, build and test every target that this machine builds, and report each"),
    ("build [mode]", "configure and build: debug, release, asan, tsan, wasm or ios"),
    ("suite [name]", "run a conformance suite, or every suite that is checked out"),
    ("app <name>", "build and run an app under apps"),
    ("example <name>", "build and run an example under examples"),
    ("help", "every command, and what it takes"),
)


def age(seconds):
    """Returns how long ago `seconds` was, in the largest unit that fits."""
    for unit, size in (("d", 86400), ("h", 3600), ("m", 60)):
        if seconds >= size:
            return "%d%s ago" % (seconds // size, unit)
    return "just now"


def overview():
    """Prints the everyday commands, and each build directory with when it last built."""
    import os
    import time

    from tools import dev
    from tools.build import build as builds

    print("Prax's developer workflow, which ./dev.sh runs.\n")
    width = max(len(command) for command, _ in EVERYDAY)
    for command, text in EVERYDAY:
        print("    ./dev.sh %s  %s" % (command.ljust(width), text))
    print("\nbuilds:")
    found = False
    for name, chosen in builds.MODES.items():
        log = os.path.join(chosen.directory, ".ninja_log")
        if os.path.exists(log):
            found = True
            print("    %-10s %-24s built %s" % (name, os.path.relpath(chosen.directory, dev.ROOT),
                                              age(time.time() - os.path.getmtime(log))))
    if not found:
        print("    none yet; ./dev.sh test makes the default build")


def help(code=0):
    """Prints every command's usage, and exits with `code`."""
    out = sys.stdout if code == 0 else sys.stderr
    print(__doc__.strip() + "\n", file=out)
    for name, module in COMMANDS.items():
        usage = importlib.import_module(module).USAGE.get(name)
        if usage:
            print(usage, file=out)
    print("\nPR_BUILD_DIR names the default build's directory in place of build, for a second checkout's work beside "
          "the\nfirst's, or work that must not share a build with another running at the same time.", file=out)
    sys.exit(code)


def main(argv):
    if not argv:
        overview()
        return
    command, args = argv[0], argv[1:]
    if command in ("help", "-h", "--help"):
        help()
    if command not in COMMANDS:
        print("dev.sh: error: no command named %s" % command, file=sys.stderr)
        help(1)
    getattr(importlib.import_module(COMMANDS[command]), command)(args)


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except KeyboardInterrupt:
        sys.exit(130)
