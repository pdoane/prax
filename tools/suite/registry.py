"""The conformance suites: what each reads, the runner that drives it, and the harnesses that the runner needs.

`./dev.sh suite <name>` fetches what a suite reads, builds, and runs it. A suite's data, its errata and its scoreboard,
is under suites/, in the directory that its runner names.
"""

import os

from tools import dev


class Suite:
    """A suite. `checkouts` are what it reads (tools/fetch/checkouts.py), and `corpus` the paths under third_party whose
    presence says that it is checked out. `runner` is the module that runs it and the arguments before the run's own,
    and `harnesses` the build's targets that the runner drives."""

    def __init__(self, name, checkouts, corpus, runner, harnesses):
        self.name = name
        self.checkouts = checkouts
        self.corpus = corpus
        self.runner = runner
        self.harnesses = harnesses

    def ready(self, directory):
        """Returns true if the suite's corpus is checked out and its harnesses are built in `directory`."""
        return (all(os.path.exists(os.path.join(dev.ROOT, "third_party", path)) for path in self.corpus)
                and all(os.path.exists(dev.executable(directory, harness)) for harness in self.harnesses))


def reftest(name, checkouts, corpus, harnesses=("reftest",)):
    """A suite whose references are images (tools/suite/reftest.py)."""
    return Suite(name, checkouts, corpus, ("tools.suite.reftest", name), harnesses)


def wpt(name):
    """A suite of web-platform-tests, run in pr_shell (tools/suite/wpt/wpt.py). The runner renders the SVG documents of
    reftests with the reftest harness."""
    return Suite(name + "-wpt", ("wpt", "webfeatures", "fonts"), ("wpt/.git",), ("tools.suite.wpt.wpt", name),
                 ("pr_shell", "reftest"))


SUITES = [
    Suite("css-parsing", ("cssparser",), ("cssparser/src",), ("tools.suite.css_parsing",), ("css_parsing",)),
    *(wpt(name) for name in ("css", "dom", "encoding", "html", "infrastructure", "networking")),
    Suite("html-tokens", ("html5lib",), ("html5lib/tokenizer",), ("tools.suite.html_tokens",), ("html_tokens",)),
    Suite("html-tree", ("wpt",), ("wpt/.git",), ("tools.suite.html_tree",), ("html_tree",)),
    Suite("url-parsing", ("wpt",), ("wpt/.git",), ("tools.suite.url_parsing",), ("url_parsing",)),
    *(wpt(name) for name in ("url", "mathml", "performance", "svg", "webaudio", "xhr")),
    reftest("resvg", ("resvg_tests", "fonts"), ("resvg-tests/tests",)),
    reftest("lottie-web", ("lottie_corpora", "lottie_web"), ("lottie-web/version",), ("reftest", "lottie_reference")),
    reftest("skottie", ("lottie_corpora", "canvaskit", "resvg_tests"), ("canvaskit/version",),
            ("reftest", "lottie_reference")),
    reftest("rlottie", ("lottie_corpora",), ("lottie/rlottie",), ("reftest", "rlottie_reference")),
    reftest("lottie-ios", ("lottie_corpora",), ("lottie/lottie-ios/Tests/__Snapshots__",)),
    Suite("commonmark", ("commonmark",), ("commonmark-spec/spec.txt",),
          ("tools.suite.markdown.markdown_spec", "commonmark"), ("markdown_spec",)),
    Suite("gfm", ("cmark_gfm",), ("cmark-gfm/test/spec.txt",), ("tools.suite.markdown.markdown_spec", "gfm"),
          ("markdown_spec",)),
    Suite("markdown-dom", ("commonmark", "cmark_gfm"), ("commonmark-spec/spec.txt", "cmark-gfm/test/spec.txt"),
          ("tools.suite.markdown.markdown_dom",), ("markdown_dom",)),
]

BY_NAME = {suite.name: suite for suite in SUITES}


def named(name):
    """Returns the suite that `name` names, or exits naming every suite."""
    if name not in BY_NAME:
        dev.fail("no suite named %s; one of: %s" % (name, " ".join(BY_NAME)))
    return BY_NAME[name]
