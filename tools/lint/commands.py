"""`./dev.sh lint`, `rewrap`, `includes`, `layout` and `prose`: the conventions that a compiler cannot check, and the
tools that fix what they can."""

import subprocess
import sys
import tempfile

from tools import dev
from tools.fetch import checkouts

USAGE = {
    "lint": """\
./dev.sh lint
    the conventions the compiler cannot check""",
    "rewrap": """\
./dev.sh rewrap [path...]
    refill comment paragraphs to the line limit, in the files named or in every source that lint reads""",
    "includes": """\
./dev.sh includes [path...]
    order the includes as lint requires, in the files named or in every source that lint reads""",
    "layout": """\
./dev.sh layout [path...]
    lay out the code as lint requires: indent it four spaces a level and each continuation one level past the line it
    continues, and break its lines no sooner than the column limit makes them, in the files named or in every source
    that lint reads""",
    "prose": """\
./dev.sh prose [--base REV | --staged] [path...]
    check a change that rewrites only comments (docs/technical_writing.md). The files named, or every file changed from REV (HEAD), must have
    the same code as at REV once comments are removed. With --staged, the files are those staged, and they are compared
    with the index. Then rewrap them and report what lint finds in them (tools/lint/prose.py)""",
}


def sources():
    """Fetches what lint reads: the references to specifications are checked against the specifications themselves."""
    checkouts.fetch("webref", "webfeatures", "commonmark", "cmark_gfm")


class Background:
    """A run of lint beside other work, which `finish` waits for. Its report is kept until then."""

    def __init__(self):
        sources()
        self.report = tempfile.TemporaryFile("w+")
        self.process = subprocess.Popen([sys.executable, "-m", "tools.lint.lint"], cwd=dev.ROOT, stdout=self.report,
                                        stderr=subprocess.STDOUT, text=True)

    def finish(self):
        """Waits for lint, prints its report, and returns its exit status."""
        status = self.process.wait()
        self.report.seek(0)
        print("\nlint:")
        print(self.report.read(), end="", flush=True)
        return status


def attempt(work):
    """Runs `work` and returns its exit status, zero if it returned, so that a failure that exits is reported rather
    than ending the run."""
    try:
        work()
    except SystemExit as stopped:
        return stopped.code if isinstance(stopped.code, int) else 1
    return 0


def beside(work):
    """Runs lint beside `work`, so that a failure of either never hides the other's. Lint's report is printed when the
    work ends, however it ends, and the run fails if either failed."""
    linting = Background()
    status = 0
    try:
        status = attempt(work)
    finally:
        linted = linting.finish()
    sys.exit(status or linted)


def tools_tests(env=None):
    """Runs the tools' tests, which drive the harnesses and catch what only a page can reach. They run pages with WPT's
    harness, and check the font bundles against CLDR's exemplar characters."""
    checkouts.fetch("wpt", "cldr")
    dev.run([sys.executable, "-m", "unittest", "discover", "-q", "-s", "tools", "-t", ".", "-p", "test_*.py"], env=env)


def lint(argv):
    dev.usage(argv, USAGE["lint"])
    sources()
    dev.python("tools.lint.lint")


def rewrap(argv):
    dev.usage(argv, USAGE["rewrap"])
    dev.python("tools.lint.rewrap", *argv)


def includes(argv):
    dev.usage(argv, USAGE["includes"])
    dev.python("tools.lint.includes", *argv)


def layout(argv):
    dev.usage(argv, USAGE["layout"])
    dev.python("tools.lint.layout", *argv)


def prose(argv):
    dev.usage(argv, USAGE["prose"])
    dev.python("tools.lint.prose", *argv)
