# Testing

Each runner's docstring (`tools/suite/wpt/wpt.py`, `tools/suite/css_parsing.py`, `tools/suite/html_tokens.py`,
`tools/suite/html_tree.py`, `tools/suite/markdown/markdown_spec.py`, `tools/suite/markdown/markdown_dom.py`,
`tools/suite/reftest.py`) documents its selection, counting and exclusions.

## Running

`./dev.sh` is the fast loop: lint, build and unit tests. Then run the suites for the areas that a change touches:

| area | suites |
| --- | --- |
| CSS | `css-parsing`, `css-wpt` |
| node tree, events and input | `dom-wpt` |
| HTML, focus | `html-tokens`, `html-tree`, `html-wpt` |
| MathML | `mathml-wpt` |
| Markdown | `commonmark`, `gfm`, `markdown-dom` |
| URL | `url-parsing`, `url-wpt` |
| shell, runner, anything every page uses | every `*-wpt` suite |
| SVG, Lottie rendering | `resvg`, `lottie-web`, `skottie`, `rlottie`, `lottie-ios` |
| the build, a library's public headers or its links | `./dev.sh package` |
| invalidation, caching, rendering cost | the `*-wpt` suites with `--verify`, and `./dev.sh pages invalidation` |

Corpus revisions are pinned in `tools/fetch/checkouts.py`. A case's key is `<file>:<index>`. `--shaper` and `--hinting`
run a suite with another of lib/text's shapers or a hinting of lib/font's default hinter, as pr_shell, the reftest
harness and the viewer take them, and leave the scoreboard alone. `--write` keeps each render, its reference and their
difference, in `tools/suite/reftest.py` and `tools/suite/wpt/wpt.py` alike.

`./dev.sh suite <name> --ab` runs a suite against a base commit as well, with every harness that the suite uses built
from the base, and lists the cases that pass more or fewer tests than there. Every runner writes its cases' outcomes
with `--json` and compares them with another run's with `--against` (`tools/suite/corpus.py`), which is all the A/B asks
of it.

## A suite's data

What the engine decides about a suite's cases, and the suite's scores, are the suite's data, in `suites/<suite>/`
(`corpus.Data`), apart from the code that runs it in `tools/`. Every suite keeps the same files:

| file | holds |
| --- | --- |
| `errata.txt` | the cases in which the engine is right and the reference is wrong, each with the reason |
| `cut.txt` | the cases that test what the engine has decided not to build, such as a player's own extension |
| `deferred.txt` | the cases that wait on what the engine will build |
| `skip.txt` | the cases that we choose not to run, such as a JIT's benchmark or a case whose result varies for a reason that the engine does not control |
| `scoreboard.md` | what a plain run of the default build passes, which the run writes |
| `fuzzy.txt` | tolerances that override a case's own, in a suite whose cases state them |
| `render.txt` | the fonts and settings that a suite renders with (`tools/suite/suite_render.h`) |

A list that would be empty is absent. The lists share one form: each entry lists its cases, one per line, and then the
reason, indented (`corpus.read_errata`). A key names a case, or a file for every case of the file. A plain run leaves
out the cut, deferred and skipped cases, and `--match` runs them like any other. Commit the scoreboard with the change
that alters it. Its diff is the report.

## Errata

Every runner shares `tools/suite/corpus.py`. An errata entry records a case in which the engine is right and the corpus
is wrong. A run fails on an unrecorded failure, a recorded case that starts passing, or an entry that names no case.
Errata never excuse a failure of the engine's own, such as a feature that it has yet to build.

A disagreement between the corpus and the specification is not settled by either one alone. It is decided as
[behavior.md](behavior.md) describes: the browsers are measured (`./dev.sh probe`), the drafts and the issues are
read, and the judgement follows. The decision is made before an entry is written, and the entry records what the
sources say and why the corpus loses. If the corpus wins, the engine follows it, and a code comment says so. A plan
that meets such a case records the measurements and the decision.

## WPT

`tools/suite/wpt/wpt.py` runs pages in `pr_shell` with WPT's `testharness.js`, on a virtual clock that advances only
when a page waits, so its timers and animations take no time. A page stops once the harness reports. Every page runs
against the engine as it is. Nothing stands in for what the engine lacks, so a page that uses an entry point that the
engine does not define fails as it would in an engine without it, and its tests count. The vendor's files in
`tools/suite/wpt/` are the reporter (`report.js`) and a `testdriver-vendor.js` that turns testdriver's clicks, keys and
action sequences into a host's input (`PrAppEvent`), which the shell dispatches through the page's view as tasks. Each
other action of testdriver fails at once, as it does in a browser whose vendor lacks it.

`--verify` checks the engine's incremental rendering as the pages run (`pr_shell --verify`). After every rendering
update, each page's presentation is compared with a full recomputation, and its work is compared with the inputs that
changed (docs/rendering.md). The pages that fail a check are listed after the results, which the checks do not change.
In this way, a suite whose pages script mutations tests invalidation, beyond what its own assertions check.

A plain run excludes five kinds of page. The pages that `WAITS` finds would stall instead of failing on what the engine
lacks. The pages whose WEB_FEATURES.yml names a feature that `tools/suite/wpt/web_features.txt` does not build are
excluded. The pages that `suites/wpt/deferred.txt` lists wait on what the engine will build. The pages that
`suites/wpt/cut.txt` lists test what the engine has decided not to build. The pages that `suites/wpt/skip.txt` lists
are pages that we choose not to run: they cost more than they test, or their result varies for a reason that the engine
does not control. Every WPT suite shares `suites/wpt/`, and a plain run of the default build, with or without `--verify`,
rewrites its own table in the scoreboard.

## Invalidation over real pages

The captured pages under `third_party/pages` are not a suite: each machine rolls its own, so they have no data in
`suites/` and no scoreboard. `./dev.sh pages invalidation` runs `tools/pages/invalidation.py`, which runs the
invalidation harness (`tools/pages/invalidation.cpp`) over those pages, which `tools/pages/pages.py` captures whole,
with everything each page loads. The harness loads each page without its scripts and changes elements spread through it:
it toggles their classes, sets an attribute, removes and reinserts them, replaces their text, and inserts a float into
them and removes it. Then it resizes the viewport and back. After each change it checks the presentation against a full
recomputation and counts what the change cost, by kind of change, together with the work that was wasted: elements
restyled to the same style, and boxes painted again although nothing that they paint changed. A page fails if a check
fails. The counts show where invalidation is coarse on real pages and real CSS, and the `page-changes` benchmark
(docs/performance.md) records them.

A capture is the local copy of what its page loads, so a host answers each captured resource as it answers a local
one (`PageHost::addCapture`): as from the request's own origin, with no CORS check, whatever the request's mode. A URL
that the capture left out, as one that its server refused, fails as a network error, so a captured page never reaches
the network. `tools/pages/pages.py local` captures pages that are files, such as the Prince samples
(`./dev.sh fetch prince_samples`): each loads at its file: URL, and its capture holds only what other origins serve.

The Prince samples are only those that we are licensed to use. yeslogic/prince-samples provides a sample without
license information for evaluating Prince alone, so the fetch takes only the directories whose README licenses both
the HTML and Prince's PDF of it (`tools/fetch/checkouts.py`). The captured fonts, style sheets and images belong to
their owners under their own terms. They are a local copy for testing, fetched into third_party, never committed and
never redistributed.

A failure is a defect wherever it lies, and is cut down to a test before it is fixed:

- `invalidation --element N <capture> <page>` makes only the changes of the Nth chosen element, and prints which
  element that is. Its report names the first check that failed. A style check names the property and both values.
- `python3 -m tools.pages.reduce --element N <capture> <page> <out>` cuts down the page's elements, its sheets' rules
  and their declarations, and its attributes, as long as that check still fails at that element. What remains is usually
  a few elements and a few rules, with the command that reproduces the failure, and these become a unit test that fails
  without the fix. `--match TEXT` names the element by its class, its name or an attribute instead.
- A failure that does not cut down depends on the page's size. For example, the restyle divides work among threads
  only above a certain number of elements.

## Reftests

`tools/suite/reftest.cpp` renders with the cpu backend and reports both WPT's measure and ThorVG's
(`tools/harness/compare.h`). WPT's reftests run in every `*-wpt` suite. `pr_shell --screenshot` captures an HTML or
XHTML page once it settles, as WPT captures one, and the harness renders SVG documents itself and compares the two. A
page that still has `reftest-wait` once it settles is rendered and sent `TestRendered`, the event of WPT's reftest
protocol that says the page has rendered, and settles again.
The WPT runner fetches the checkout's pages from WPT's own server, wptserve, with its headers, substitutions and
handlers, as wptrunner serves them to a browser (`tools/suite/wpt/wpt_serve.py`). pr_shell knows nothing of WPT: the
runner hands it the options that a browser under wptrunner is started with, host rules that point WPT's hosts at the
loopback (`--resolve`), WPT's CA (`--ca`), wptrunner's wait for a screenshot (`--capture-wait`) and Ahem (`--font`), and
the URLs of the pages. The viewer reaches the server the same way when `./dev.sh viewer` runs it around one. Any other
page, such as Prax's, loads at its file: URL. Pages have WPT's Ahem font installed, and the `Standard14` font bundle
(`pr/font_bundle.h`), whose generic families have the metrics of Times, Helvetica and Courier.
`tools/lint/test_font_bundle.py` checks that the bundles cover each language's exemplar characters. Ahem, whether
installed or loaded by `@font-face`, is drawn without antialiasing, as reftests built on it expect (`prFontIsAliased`).
A page's `@font-face` faces load when its text asks for them in a rendering update, and `document.fonts.ready` waits for
them, so a page settles with its fonts loaded. The end of a load lays the document out, and the faces that its text asks
for then delay its load event, so a test that measures at load measures them. A reftest passes if it matches any of its
match references and none of its mismatch references. A print reftest (web-platform-tests docs, "Print Reftests") and
its references are printed (`pr_shell --print`). They start from the page box that WPT's protocol gives a document, 5 by
3 inches with half-inch margins, which the runner hands the shell (`--page-size`, `--page-margin`). Each page that the
test's `reftest-pages` metas select is rendered as an image of its own, and compared with the reference's page in the
same place. Two documents with different numbers of pages differ.

`tools/suite/reftest.py` runs the suites whose references are images. resvg's tests are each compared with the PNG
beside them. Each Lottie corpus is compared with the player that it was written for: lottie-web's tests with
lottie-web's SVG renderer, Skottie's with Skottie, rlottie's with rlottie, and lottie-ios's samples with the snapshots
that lottie-ios's own tests took. An erratum of a Lottie suite therefore says that one player is wrong, as decided from
After Effects, the players and the specification. The comparison finds the cases to look at, and a person decides them
in the viewer (`tools/viewer`), which shows the engine beside each suite's reference.

`./dev.sh probe <page> --screenshot <directory>` captures a page in the browsers and in the engine, side by side.
`--viewport 600x800` lays out a page of `suites/prax` as the suite is meant to be read.

## Our pages (`suites/prax`)

The pages walk through all of CSS, CSS 2 and every module, and SVG 2 chapter by chapter, one page per section, for a
person to read. Each page shows what the specification describes, and where Chrome, Safari and Firefox draw something
else. The pages cover what is drawn, not parsing or serialization, which WPT covers. They present the specification's
content, not the engine's choices, which its unit tests record.
[suites/prax/README.md](../suites/prax/README.md) holds the suite's rules and decisions, and the modules that
it covers. `./dev.sh lint` checks the pages' indexes, eyebrows and citations against webref
(`tools/suite/prax/suite.py`), `./dev.sh prax stills` captures them in the browsers beside the engine, and `./dev.sh
prax fit` lists the pages that do not fit their 600 by 800 viewport.

## Examples

The examples under `examples/` are applications over the public headers, each a window on lib/app's default backend
(`prAppCreate`) whose `main()` composes everything that runs it. Each is checked by running it.
`./dev.sh example <name> [args...]` builds one and runs it.

`./dev.sh package` checks the libraries as a consumer takes them. It builds them with the build's defaults and installs
them, then builds `tools/package`, a program over their public headers, against the installed package and against the
source tree as FetchContent adds it, and runs each.

- `draw_gallery` draws animated scenes with `lib/draw` alone, and shows a frame of HDR through the display shader
  (`pr/draw_display.h`).
- `draw_features` shows a captioned drawing of each feature of `lib/draw`'s list, and the workloads that `./dev.sh
  bench draw` measures, from a library of drawings that the benchmark links.
- `studio_gallery` shows every widget of Studio Kit, live.
- `editor` is a level editor of Studio Kit's widgets and a HUD library of its own, composed in one tree.
- `perspective` stands Kit panels in 3D over a background that it draws with its own pipeline, as a 3D engine would.
- `incremental` renders Markdown and HTML as they arrive in pieces.
- `browser` shows a web page in a window, and navigates from it, as a browser's threads do.

The tools that measure and compare the engine are under `tools/`: the viewer (`./dev.sh viewer`, whose header
comment in tools/viewer/viewer.cpp describes it), and the benchmarks that `./dev.sh bench` runs.
