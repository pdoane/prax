"""`./dev.sh gen`: the generated tables, from their pinned sources."""

from tools import dev
from tools.fetch import checkouts

USAGE = {
    "gen": """\
./dev.sh gen
    regenerate the tables from webref and the other sources""",
}

GENERATORS = ("gen_brotli", "gen_charset", "gen_css", "gen_dom", "gen_html", "gen_url", "gen_webidl", "gen_unicode",
              "gen_locale")


def gen(argv):
    dev.usage(argv, USAGE["gen"])
    checkouts.fetch("webref", "entities", "encodings", "unicode", "cldr", "rfcs", "drafts")
    for generator in GENERATORS:
        dev.python("tools.gen." + generator)
