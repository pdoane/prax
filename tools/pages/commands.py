"""`./dev.sh pages`: the real pages under third_party/pages and third_party/pages-corpus, rolled forward, captured
again or checked, and the invalidation check over them."""

import os

from tools import dev
from tools.build import build as builds
from tools.fetch import checkouts

USAGE = {
    "pages": """\
./dev.sh pages roll
    the day's real pages into third_party/pages, each captured whole by headless Chrome with everything it loads, and
    the oldest dropped (tools/pages/pages.py)
./dev.sh pages refresh [--jobs N] [word...]
    capture again, from its URL, each page of third_party/pages and third_party/pages-corpus, or those with a word of
    the name. A page that no longer captures is removed
./dev.sh pages check [word...]
    the URLs that each capture's markup and sheets name and that it lacks
./dev.sh pages take <url>...
    capture each page at its URL into third_party/pages-corpus, as roll captures, and print the path of its record,
    which pr_shell --html and the browser example open
./dev.sh pages invalidation [debug|asan] [args...]
    changes to elements of each page of third_party/pages, each checked against a full recomputation, and what the
    changes cost (tools/pages/invalidation.py), from that build's harness""",
}


def pages(argv):
    commands = (["roll"], ["refresh"], ["check"], ["take"], ["invalidation"])
    dev.usage(argv, USAGE["pages"], None if argv[:1] in commands else 1)
    if argv[0] != "invalidation":
        os.makedirs(os.path.join(dev.ROOT, "third_party", "pages"), exist_ok=True)
        dev.python("tools.pages.pages", *argv)
        return
    args = argv[1:]
    mode = args.pop(0) if args[:1] in (["debug"], ["asan"]) else None
    chosen = builds.build(mode)
    checkouts.fetch("pages", pages_args=("--if-empty",))
    checkouts.fetch("fonts")
    dev.python("tools.pages.invalidation", *args, env=chosen.environment())
