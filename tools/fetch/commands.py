"""`./dev.sh fetch`: the checkouts under third_party (checkouts.py)."""

from tools import dev
from tools.fetch import checkouts

USAGE = {
    "fetch": """\
./dev.sh fetch [name...]
    fetch what third_party holds, at the commits that tools/fetch/checkouts.py pins: the checkouts named, or every one.
    Nothing under third_party is committed, and every command fetches what it needs, so this takes a checkout ahead of
    time. The real pages roll forward only when named, as `./dev.sh fetch pages` or `./dev.sh pages roll` asks; with
    every checkout, they are taken only if there are none""",
}


def fetch(argv):
    dev.usage(argv, USAGE["fetch"])
    if argv:
        checkouts.fetch(*argv)
    else:
        checkouts.fetch(*checkouts.CHECKOUTS, pages_args=("--if-empty",))
