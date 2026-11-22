#!/usr/bin/env python3
"""Writes the documents of suites/prax-limits, whose content multiplies or runs long, which `./dev.sh bench limits`
measures (tools/bench/limits_bench.cpp). They are made here, and none is committed: a long document is large, and what
each holds is read from its generator rather than from the file. The bench writes them into the build that it runs.

Usage: python3 -m tools.bench.gen_limits <directory>
"""

import json
import os
import sys

# How many times each level of a multiplying document uses the one below it, and its levels: a million at the last.
FANOUT = 10
LEVELS = 6

# The rows of the long table, and the items of the long flex and grid containers.
ROWS = 1000
ITEMS = 1000

# The levels of each chain of flex containers.
DEPTH = 256

# The page that the long documents print on: 500 px of page area, inside a margin of 50.
PAGE = "@page { size: 600px 600px; margin: 50px }"


def use_explosion():
    """An SVG document whose last level uses the one below it ten times, down to a rectangle: a million rectangles,
    which the use elements' instances bound."""
    out = ["<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'><defs>",
           "<rect id='l0' width='1' height='1'/>"]
    for level in range(1, LEVELS + 1):
        out.append("<g id='l%d'>%s</g>" % (level, "".join("<use href='#l%d'/>" % (level - 1) for _ in range(FANOUT))))
    out.append("</defs><use href='#l%d'/></svg>\n" % LEVELS)
    return "\n".join(out)


def precomp_explosion():
    """A Lottie animation whose compositions each hold ten precomps of the one before it: a million solids at its last
    level, which the player's layers bound."""
    def layers(ref):
        return [{"ty": 0, "refId": ref, "w": 10, "h": 10, "ip": 0, "op": 60, "st": 0, "ks": {}} for _ in range(FANOUT)]

    assets = [{"id": "c0", "w": 10, "h": 10,
               "layers": [{"ty": 1, "ip": 0, "op": 60, "st": 0, "sw": 10, "sh": 10, "sc": "#00ff00", "ks": {}}]}]
    assets += [{"id": "c%d" % level, "w": 10, "h": 10, "layers": layers("c%d" % (level - 1))}
               for level in range(1, LEVELS + 1)]
    animation = {"v": "5.7.0", "w": 100, "h": 100, "fr": 30, "ip": 0, "op": 60, "assets": assets,
                 "layers": layers("c%d" % LEVELS)}
    return json.dumps(animation, indent=1) + "\n"


def html(style, body):
    return "<!doctype html>\n<style>%s %s</style>\n<body style='margin: 0'>\n%s\n" % (PAGE, style, body)


def long_table():
    """A table of a header row and a thousand rows of three cells with collapsed borders, printed: sized once, and each
    page lays out and paints its own rows."""
    style = ("table { border-collapse: collapse; width: 300px } td, th { border: 2px solid black; height: 18px; "
             "padding: 0 }")
    rows = "\n".join("<tr><td></td><td></td><td></td></tr>" for _ in range(ROWS))
    return html(style, "<table><thead><tr><th></th><th></th><th></th></tr></thead><tbody>\n%s\n</tbody></table>" % rows)


def long_container(display):
    """A flex column or a grid of a thousand items 20 px tall, printed: laid out once, and each page places the items
    on it."""
    style = ".c { display: %s; flex-direction: column } .c > div { width: 100px; height: 20px }" % display
    items = "\n".join("<div></div>" for _ in range(ITEMS))
    return html(style, "<div class=c>\n%s\n</div>" % items)


# The kinds of chain. Each level is a flex container of a leaf and the next level, so that the nodes grow by a constant
# per level, and so should the work of laying them out. A kind is its style, the classes of each level's container, and
# what holds the next level inside the container.
CHAINS = {
    # Single-line columns of auto height, their items stretched.
    "columns": (".l { display: flex; flex-direction: column }", lambda level: "", ("", "")),
    # Rows and columns in turn.
    "alternating": (".l { display: flex; flex-direction: column } .r { flex-direction: row }",
                    lambda level: " r" if level % 2 else "", ("", "")),
    # Columns, each level's next level inside a block that wraps it.
    "wrapped": (".l { display: flex; flex-direction: column }", lambda level: "", ("<div>", "</div>")),
    # Rows of auto height whose next level sits in an item's child of half its stretched height.
    "percentages": (".l { display: flex; flex-direction: row } .h { height: 50% }", lambda level: "",
                    ("<div><div class=h>", "</div></div>")),
    # Columns whose items sit at flex-start, so that each level's width is its content's.
    "fitted": (".l { display: flex; flex-direction: column; align-items: flex-start }", lambda level: "", ("", "")),
    # Columns, each in the other writing mode from its parent's, an orthogonal item, main-inline in its parent.
    "orthogonal": (".l { display: flex; flex-direction: column } .v { writing-mode: vertical-rl }",
                   lambda level: " v" if level % 2 else "", ("", "")),
    # Multi-line columns.
    "wrapped-columns": (".l { display: flex; flex-direction: column; flex-wrap: wrap }", lambda level: "", ("", "")),
}


def chain(kind):
    style, classes, (into, out) = CHAINS[kind]
    style += " .leaf { height: 10px; width: 10px }"
    opened = "".join("<div class='l%s'><div class=leaf></div>%s" % (classes(level), into) for level in range(DEPTH))
    return html(style, opened + (out + "</div>") * DEPTH)


def write(directory):
    """Writes the documents into `directory`, and returns their paths."""
    files = {
        "svg/use-explosion.svg": use_explosion(),
        "lottie/precomp-explosion.json": precomp_explosion(),
        "html/long-table.html": long_table(),
        "html/long-flex.html": long_container("flex"),
        "html/long-grid.html": long_container("grid"),
    }
    files.update({"html/flex-chain-%s.html" % kind: chain(kind) for kind in CHAINS})
    paths = []
    for name, text in sorted(files.items()):
        path = os.path.join(directory, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        paths.append(path)
    return paths


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: python3 -m tools.bench.gen_limits <directory>")
    write(sys.argv[1])
