"""`./dev.sh gen`: the generated tables, from their pinned sources."""

from tools import dev
from tools.fetch import checkouts

USAGE = {
    "gen": """\
./dev.sh gen [name...]
    regenerate the tables from webref and the other sources, or only those of the generators named, as float for
    tools/gen/gen_float.py""",
}

GENERATORS = ("brotli", "charset", "css", "dom", "html", "url", "webidl", "unicode", "locale", "mathml", "float")

# The checkouts that the generators read, and the generators that compute their tables and read none.
SOURCES = ("webref", "entities", "encodings", "unicode", "cldr", "rfcs", "drafts", "mathml")
COMPUTED = ("float",)


def gen(argv):
    dev.usage(argv, USAGE["gen"])
    unknown = [name for name in argv if name not in GENERATORS]
    if unknown:
        dev.fail("no generator named %s; they are %s" % (", ".join(unknown), ", ".join(GENERATORS)))
    names = argv or list(GENERATORS)
    if any(name not in COMPUTED for name in names):
        checkouts.fetch(*SOURCES)
    for name in names:
        dev.python("tools.gen.gen_" + name)
