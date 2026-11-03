"""`./dev.sh prax`: the checks and stills of our pages (suites/prax), measured and captured in the browsers, from the
default build's pr_shell and WebKit host, or Safari itself with --safari."""

from tools import dev
from tools.build import build as builds
from tools.fetch import checkouts

USAGE = {
    "prax": """\
./dev.sh prax fit [directory...]
    the pages of suites/prax that do not fit 600x800 in Chrome (tools/suite/prax/suite_fit.py)
./dev.sh prax stills --out <directory> <directory>... [--page NAME]... [--browser C1|S|F|P]... [--print-only]
                     [--scheme light|dark] [--safari]
    each page of those directories of suites/prax in Chrome, WebKit, Firefox and the engine, and a sheet of the four
    side by side, with each browser's printed pages of the page's pr-paged figures below
    (tools/suite/prax/suite_stills.py). --browser captures only those columns, E being the engine, and the sheet keeps
    the others' stills from an earlier run into the same directory. Every column is for a user who prefers a dark color
    scheme, or a light one with --scheme light. --safari captures the S column in Safari itself
./dev.sh prax fix
    the sections' numbers and titles in the indexes of suites/prax, and the pages' eyebrows and citations, rewritten
    from webref where a draft renumbers (tools/suite/prax/suite.py)""",
}

TOOLS = {"fit": "tools.suite.prax.suite_fit", "stills": "tools.suite.prax.suite_stills"}


def prax(argv):
    dev.usage(argv, USAGE["prax"], None if argv[:1] in (["fit"], ["stills"], ["fix"]) else 1)
    if argv[0] == "fix":
        # Rewriting the indexes, eyebrows and citations needs no build or browser.
        dev.python("tools.suite.prax.suite", "--fix")
        return
    checkouts.fetch("fonts")
    chosen = builds.build()
    if dev.darwin():
        builds.build(targets=["webkit_host"])
    dev.python(TOOLS[argv[0]], "--build", chosen.directory, *argv[1:])
