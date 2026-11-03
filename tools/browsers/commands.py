"""`./dev.sh probe`: a probe page in the default build's pr_shell and in the browsers, WebKit in the build's host, or
Safari itself with --safari."""

from tools import dev
from tools.build import build as builds
from tools.fetch import checkouts

USAGE = {
    "probe": """\
./dev.sh probe <page> [--browser C|C1|S|F]... [--json] [--screenshot <directory>] [--scan] [--viewport WxH]
                      [--pixel-ratio <pixels per px>] [--scheme light|dark] [--safari] [--keys] [--no-key-commands]
    a probe page's cases measured in Chrome at a pixel ratio of two and of one, WebKit, Firefox, and the engine, side
    by side (tools/browsers/probe.py); or with --screenshot, each one's rendering as a PNG, at 800x600 or --viewport
    (600x800 for a page of suites/prax). --pixel-ratio runs every one at that many pixels per px instead, --scheme light
    for a user who prefers a light color scheme, where dark is the default, --safari runs the S column in Safari
    itself, through safaridriver, in place of WebKit in the build's host, --scan reads the screenshots along the
    scanlines the page names, and --keys presses the keys that the page asks for, as trusted input, in Chrome, Firefox,
    the engine and, with --safari, Safari, with Chrome given macOS's editing commands unless --no-key-commands""",
}


def probe(argv):
    dev.usage(argv, USAGE["probe"], None if argv else 1)
    checkouts.fetch("fonts")
    chosen = builds.build()
    if dev.darwin():
        builds.build(targets=["webkit_host"])
    dev.python("tools.browsers.probe", "--build", chosen.directory, *argv)
