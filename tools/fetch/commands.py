"""`./dev.sh fetch`: the checkouts under third_party (checkouts.py)."""

from tools import dev
from tools.fetch import checkouts

USAGE = {
    "fetch": """\
./dev.sh fetch [name...]
    fetch what third_party holds, at the commits that tools/fetch/checkouts.py pins. Nothing under third_party is
    committed, and every command fetches what it needs, so this takes a checkout ahead of time""",
}


def fetch(argv):
    dev.usage(argv, USAGE["fetch"])
    checkouts.fetch(*argv)
