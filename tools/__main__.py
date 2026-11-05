"""The developer workflow, which ./dev.sh runs: `./dev.sh help` lists the commands, and `./dev.sh <command> --help`
describes one. With no command, it lints, builds, and runs the unit tests and the tools' tests: the fast loop, which
runs no suite. Lint runs beside the rest, and every failure is reported before the run fails.

Nothing under third_party is committed, and every command fetches what it needs (tools/fetch/checkouts.py). Benchmark
results are kept under perf, which is not committed either.
"""

import importlib
import sys


# The commands, in the order that help lists them, each by the module that defines it.
COMMANDS = {
    "build": "tools.build.commands",
    "test": "tools.build.commands",
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


def everything():
    """The whole of `./dev.sh test`: lint beside the unit tests and the tools' tests."""
    from tools.build import commands as build

    build.test([])


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
        everything()
        return
    command, args = argv[0], argv[1:]
    if command in ("help", "-h", "--help"):
        help()
    if command not in COMMANDS:
        print("dev.sh: no command named %s" % command, file=sys.stderr)
        help(1)
    getattr(importlib.import_module(COMMANDS[command]), command)(args)


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except KeyboardInterrupt:
        sys.exit(130)
