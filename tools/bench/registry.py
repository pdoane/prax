"""The table of benchmarks: what each one measures, what it reads, and what runs it.

There are two kinds of benchmark. A scripted benchmark is a script under tools/bench/scripts that does its work through
the web platform's APIs, so pr_shell and a browser run the same file (tools/bench/measure.js), and `./dev.sh bench
browser` compares their times. Most scripted benchmarks also have a harness that runs the same cases through the C API,
and its rows are filed under "prax (C)". An engine benchmark is a harness that measures what only the engine
exposes, such as whole frames, recording and rendering, or the browser example. No browser runs it.

`./dev.sh bench list` prints this table.
"""

import glob
import os

from tools.bench import common
from tools.bench import engine

CSS = os.path.join(common.ROOT, "third_party", "css")
FONTS = os.path.join(common.ROOT, "third_party", "fonts")
SCRIPTS = os.path.join(common.HERE, "scripts")


class Bench:
    """A benchmark. `harnesses` are the build targets that it runs, and `fetch` lists the checkouts that it reads
    (tools/fetch/checkouts.py).

    A scripted benchmark sets these fields:
    - `script`: its script under tools/bench/scripts.
    - `inputs`: a function that returns the files that it reads, keyed by the name under which the script reads each.
    - `native`: the C harness that runs beside it, which takes the paths of the inputs in that order.
    - `compare`: the field of its rows that browsers are compared on.
    - `cases`: true if it can run one case alone (__ONLY__).
    - `fonts`: the faces that a browser loads for it, as (family, style, file). pr_shell already has them.
    - `kind`: a function that returns the kind of a row, by name, for the comparison's summary.

    An engine benchmark sets `run(args)`, which returns its results by engine and the files that it read, and
    `options(parser)`, which adds its own options. `sources` are the files, besides its inputs, whose change makes a
    measure measure something else."""

    def __init__(self, name, summary, harnesses, fetch=(), script=None, inputs=None, native=None, compare="time",
                 cases=True, fonts=(), kind=None, run=None, options=None, sources=()):
        self.name = name
        self.summary = summary
        self.harnesses = list(harnesses)
        self.fetch = list(fetch)
        self.script = os.path.join(SCRIPTS, script) if script else None
        self.inputs = inputs or (lambda: {})
        self.native = native
        self.compare = compare
        self.cases = cases
        self.fonts = list(fonts)
        self.kind = kind or (lambda name: "all")
        self.run = run
        self.options = options or (lambda parser: None)
        self.sources = list(sources)

    @property
    def scripted(self):
        return self.script is not None


def stylesheets(*names):
    """Returns a function that returns stylesheets under third_party/css, keyed by their names without the
    extension."""
    return lambda: {name[:-len(".css")]: os.path.join(CSS, name) for name in names}


def roles(**names):
    """Returns a function that returns stylesheets under third_party/css, keyed by the role in which the script reads
    each."""
    return lambda: {role: os.path.join(CSS, name) for role, name in names.items()}


def corpus_pages():
    return {os.path.basename(path): path for path in common.pages(common.PAGES)[:-1]}


def layout_kind(name):
    """Returns the kind of a layout case: a rebuild, a resize, or a change that reaches part of the document."""
    last = name.split()[-1]
    return last if last in ("rebuild", "resize") else "change"


TESTS = os.path.join(common.ROOT, "lib")
# The directory of the flex benchmark's harness and its Taffy library. The library's lockfile pins the crates that
# Taffy depends on.
FLEX = os.path.join(common.ROOT, "tools", "bench", "flex_bench")
# The directory that holds the vterm benchmark's harness and the adapters for its engines.
VTERM = os.path.join(common.ROOT, "tools", "bench", "vterm_bench")

BENCHES = [
    # Scripted benchmarks: the same file runs in pr_shell and in browsers.
    Bench("css-parse", "parsing real stylesheets, reading every declaration, and serializing them",
          ["pr_shell", "css_bench"], fetch=["css"], script="css-parse.js", native="css_bench", compare="ready",
          cases=False,
          inputs=stylesheets("bootstrap.css", "bulma.css", "normalize.css", "bootstrap.min.css", "bulma.min.css",
                             "tailwind.min.css", "primer.css", "fontawesome.min.css", "animate.min.css"),
          sources=[os.path.join(TESTS, "css", "tests", "css_bench.cpp")]),
    Bench("html-parse", "parsing the real pages under third_party/pages with DOMParser",
          ["pr_shell", "html_bench"], fetch=["pages"], script="html-parse.js", native="html_bench", compare="parse",
          cases=False, inputs=corpus_pages,
          sources=[os.path.join(TESTS, "html", "tests", "html_bench.cpp")]),
    Bench("text-decode", "decoding the real pages under third_party/pages with TextDecoder, as UTF-8 and windows-1252",
          ["pr_shell", "charset_bench"], fetch=["pages"], script="text-decode.js", native="charset_bench",
          compare="utf8", cases=False, inputs=corpus_pages,
          sources=[os.path.join(common.ROOT, "lib", "charset", "tests", "charset_bench.cpp")]),
    Bench("selectors", "querySelectorAll and matches over a long list and a deep tree",
          ["pr_shell", "selector_bench"], script="selectors.js", native="selector_bench",
          sources=[os.path.join(TESTS, "html", "tests", "selector_bench.cpp")]),
    Bench("typed-om", "making, reading and setting values through the CSS Typed OM",
          ["pr_shell", "typed_bench"], script="typed-om.js", native="typed_bench",
          sources=[os.path.join(TESTS, "css", "tests", "typed_bench.cpp")]),
    Bench("mutation", "the DOM and CSSOM mutations a page makes in a loop, with no read of style after them",
          ["pr_shell", "mutation_bench"], fetch=["css", "fonts"], script="mutation.js", native="mutation_bench",
          inputs=roles(small="normalize.css", large="bootstrap.css"),
          sources=[os.path.join(TESTS, "html", "tests", "mutation_bench.cpp")]),
    Bench("restyle", "a change to a page of cards under bootstrap, then a read of a computed value or a box",
          ["pr_shell", "style_bench"], fetch=["css", "fonts"], script="restyle.js", native="style_bench",
          inputs=roles(large="bootstrap.css"),
          sources=[os.path.join(TESTS, "html", "tests", "style_bench.cpp")]),
    Bench("layout", "generated documents laid out from scratch, at another width, and after small changes",
          ["pr_shell"], fetch=["fonts"], script="layout.js", kind=layout_kind,
          fonts=[("Arimo", "normal", os.path.join(FONTS, "Arimo.ttf")),
                 ("Arimo", "italic", os.path.join(FONTS, "Arimo-Italic.ttf")),
                 ("NewComputerModernMath", "normal",
                  os.path.join(common.RELEASE, "font_bundle", "NewCMMath-Regular.otf"))]),

    # Engine benchmarks: they measure what only the engine exposes.
    Bench("frame", "whole frames after each kind of change to a page of cards, at 100 and 2000 cards, and its load",
          ["frame_bench"], fetch=["css", "fonts"], run=engine.frame,
          sources=[os.path.join(TESTS, "html", "tests", "frame_bench.cpp")]),
    Bench("lists", "a list of 10,000 items: loaded in pieces, an item inserted at the start or appended, and a restyle",
          ["list_bench"], fetch=["fonts"], run=engine.lists,
          sources=[os.path.join(TESTS, "html", "tests", "list_bench.cpp")]),
    Bench("print", "a long generated document printed: paginated, its pages painted, and written as PDF and SVG",
          ["print_bench"], fetch=["fonts"], run=engine.print_document,
          sources=[os.path.join(TESTS, "html", "tests", "print_bench.cpp")]),
    Bench("ui", "immediate-mode frames: trees declared whole every frame, unchanged, changed, scrolled and cold",
          ["ui_bench", "editor_bench"], fetch=["fonts"], run=engine.ui),
    Bench("text-edit", "editing a text of 100, 10,000 and 1,000,000 lines: at the caret, far away, and with undo; and moving a "
          "caret through layout",
          ["text_edit_bench"], fetch=["fonts"], run=engine.text_edit,
          sources=[os.path.join(common.ROOT, "tools", "bench", "text_edit_bench.cpp")]),
    Bench("text-view", "a windowed view of 100, 10,000 and 1,000,000 lines: opening, a keystroke, a caret's motion and a "
          "scroll, each through layout",
          ["text_view_bench"], fetch=["fonts"], run=engine.text_view,
          sources=[os.path.join(common.ROOT, "tools", "bench", "text_view_bench.cpp")]),
    Bench("flex", "trees of flex boxes in lib/layout, Yoga and Taffy: built, laid out, resized, changed and unchanged",
          ["flex_bench"], run=engine.flex, options=engine.flex_options,
          sources=sorted(glob.glob(os.path.join(FLEX, "*.*")) + glob.glob(os.path.join(FLEX, "taffy", "*.*"))
                         + glob.glob(os.path.join(FLEX, "taffy", "src", "*.rs")))),
    Bench("vterm", "programs' output written to a terminal in lib/vterm and libvterm: captures, floods, SGR, Unicode, a "
          "TUI, long lines and a resize",
          ["vterm_bench"], run=engine.vterm, options=engine.vterm_options,
          sources=sorted(glob.glob(os.path.join(VTERM, "*.*")))
          + sorted(glob.glob(os.path.join(common.ROOT, "lib", "vterm", "tests", "captures", "*.tty")))),
    Bench("page-changes", "the real pages after each kind of change: their style, layout and recording",
          ["invalidation"], fetch=["pages", "fonts"], run=engine.page_changes, options=engine.page_changes_options,
          sources=[os.path.join(common.ROOT, "tools", "pages", "invalidation.cpp")]),
    Bench("long-doc", "a long specification in the browser example: load, scroll, hover, navigate, restyle, resize",
          ["doc_bench"], fetch=["docs", "fonts"], run=engine.long_doc, options=engine.long_doc_options,
          sources=[os.path.join(common.ROOT, "tools", "bench", "doc_bench.cpp")]),
    Bench("draw", "recording and rendering draw_features' drawings on a renderer of lib/draw",
          ["draw_bench"], fetch=["fonts"], run=engine.draw, options=engine.draw_options),
    Bench("image-decode", "decoding the PNG and JPEG images of WPT's images directory",
          ["image_bench"], fetch=["wpt"], run=engine.image_decode, options=engine.image_decode_options,
          sources=[os.path.join(common.ROOT, "tools", "bench", "image_bench.cpp")]),
]

BY_NAME = {bench.name: bench for bench in BENCHES}

# What `./dev.sh bench terminals` measures (tools/bench/terminals.py). It is not in BENCHES, because it opens the
# windows of other applications, and no other command should run it.
TERMINALS = ("floods of text, Unicode, long lines and a paced TUI in the Prax terminal app, Ghostty, kitty, WezTerm, "
             "Alacritty, iTerm2 and Terminal.app: throughput, CPU, energy, GPU, memory")


def named(names):
    """Returns the benchmarks in `names`, or every benchmark if `names` is empty, in the table's order."""
    unknown = [name for name in names if name not in BY_NAME]
    if unknown:
        raise SystemExit("no benchmark named %s; ./dev.sh bench list names them" % ", ".join(unknown))
    return [bench for bench in BENCHES if not names or bench.name in names]


def table():
    """Returns a description of what each benchmark measures, grouped by kind."""
    width = max(len(b.name) for b in BENCHES)
    out = []
    for title, scripted in (("scripted: the same script in pr_shell and in browsers (./dev.sh bench browser)", True),
                            ("engine: what only the engine exposes", False)):
        out.append(title)
        out += ["  %s  %s" % (b.name.ljust(width), b.summary) for b in BENCHES if b.scripted == scripted]
        out.append("")
    out.append("end to end: whole terminal applications, on macOS, run only by ./dev.sh bench terminals")
    out.append("  %s  %s" % ("terminals".ljust(width), TERMINALS))
    out.append("")
    out.append("Every benchmark in the first two groups is counted in instructions by ./dev.sh bench and ./dev.sh bench ab.")
    return "\n".join(out)
