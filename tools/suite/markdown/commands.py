"""`./dev.sh markdown compare`: lib/markdown's HTML beside cmark-gfm's, which the build compiles from its checkout."""

from tools import dev
from tools.build import build as builds
from tools.fetch import checkouts

USAGE = {
    "markdown": """\
./dev.sh markdown compare [path...]
    lib/markdown's HTML beside cmark-gfm's, on the Markdown files named, or under the directories named, or else the
    repository's and its checkouts'; each file whose HTML differs, with the start of the difference
    (tools/suite/markdown/markdown_compare.py)""",
}


def markdown(argv):
    dev.usage(argv, USAGE["markdown"], None if argv[:1] == ["compare"] else 1)
    checkouts.fetch("cmark_gfm")
    builds.build()
    dev.python("tools.suite.markdown.markdown_compare", *argv[1:])
