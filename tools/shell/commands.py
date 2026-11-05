"""`./dev.sh document`: a page written as a document by the default build's pr_shell."""

import os

from tools import dev
from tools.build import build as builds
from tools.fetch import checkouts

USAGE = {
    "document": """\
./dev.sh document <page> <output.pdf | output.svg>... [--snapshot [--viewport WxH]] [args...]
    the page, an HTML file, URL or capture, printed as the pages of its @page boxes, over a US letter page with no
    margins, and written as a tagged PDF, or SVG, by each output's extension (lib/draw's writers). SVG has no pages, so
    a document of several pages is an SVG file for each, numbered from 1 before the extension, as out-1.svg and
    out-2.svg for out.svg, and a document of one page is out.svg. --snapshot writes it instead as it renders in the
    viewport, as one page of the viewport's size, 800x600 by default. --timings reports how long each phase took, from
    the load to the writer. Other arguments go to pr_shell""",
}


def document(argv):
    dev.usage(argv, USAGE["document"], None if len(argv) >= 2 else 1)
    checkouts.fetch("fonts")
    page = argv[0]
    options = []
    rest = []
    for argument in argv[1:]:
        extension = os.path.splitext(argument)[1].lower()
        if extension in (".pdf", ".svg") and not argument.startswith("-"):
            options += ["--" + extension[1:], os.path.abspath(argument)]
        else:
            rest.append(argument)
    if not options:
        dev.fail("name an output that ends in .pdf or .svg")
    chosen = builds.build(targets=["pr_shell"])
    dev.run([dev.executable(chosen.directory, "pr_shell"), *rest, *options, "--html", page])
