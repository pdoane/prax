"""`./dev.sh viewer`: the draw viewer, from the default build, or Debug or Release when asked."""

import os

from tools import dev
from tools.browsers import browsers
from tools.build import build as builds
from tools.fetch import checkouts

USAGE = {
    "viewer": """\
./dev.sh viewer [debug|release] [--render <directory>] [--document <which>] [args...]
    the draw viewer, comparing lib/draw's renderers over collections of documents. With --render, it writes every
    document by every backend as PNGs instead. --document narrows to the documents that a number or a word of the name
    picks. The viewer's usage lists the rest""",
}


def viewer(argv):
    dev.usage(argv, USAGE["viewer"])
    checkouts.fetch("fonts")
    mode = argv.pop(0) if argv[:1] in (["debug"], ["release"]) else None
    program = dev.executable(builds.build(mode).directory, "viewer")
    # The other pages of third_party, such as MathFonts' tests, which name their resources from the root, are served
    # from a server of the directory that runs as long as the viewer does.
    third_party = os.path.join(dev.ROOT, "third_party")
    server = browsers.Server(third_party) if os.path.isdir(third_party) else None
    command = [program, *argv]
    if server is not None:
        command += ["--third-party-server", str(server.server_address[1])]
    try:
        # WPT's reftests are served from WPT's own server, which runs as long as the viewer does.
        if os.path.isdir(os.path.join(third_party, "wpt", ".git")):
            dev.python("tools.suite.wpt.wpt_serve", "--around", "third_party/wpt", *command)
        else:
            dev.run(command)
    finally:
        if server is not None:
            server.shutdown()
