"""`./dev.sh spec`: the passages of a specification's source, verbatim."""

from tools import dev

USAGE = {
    "spec": """\
./dev.sh spec <name> [pattern] [--context N] [--refresh]
    a specification's editor's draft, its Bikeshed source fetched once into third_party/specs, and the passages that
    match a pattern, verbatim (tools/spec/spec.py)""",
}


def spec(argv):
    dev.usage(argv, USAGE["spec"])
    dev.python("tools.spec.spec", *argv)
