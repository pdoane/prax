# Rendering

This document describes how a document reaches the screen: the forces that the design answers to, the threads and what
each one owns, and how a change, a synchronous read and a load move through the stages. Each stage's own document has
the details: the cascade and the presentation in [css.md](css.md), the event loop in [html.md](html.md), trees, painting
and compositing in [layout.md](layout.md), and which thread reads the DOM in [dom.md](dom.md).

## Forces

1. **Script is slow, so the page's thread is precious.** QuickJS is an interpreter, several times slower than a JIT,
   and nothing in the engine closes that gap. What the engine can do is keep everything that is not script off the
   page's thread or, if someone waits for that work on the page's thread, spread it across cores.
2. **Mutation is the common case.** Pages change classes, attributes, inline style and subtrees all the time. Every
   stage caches what it computed and recomputes only what a change reaches, so the cost of a change depends on what it
   reaches, not on what the document holds.
3. **Two clients, one set of engines.** The web (DOM, cascade, boxes) and an immediate-mode UI (a tree declared each
   frame) share the layout, text and drawing engines. What the web owes its specs (specified values that round-trip,
   computed values that serialize, the CSSOM) stays in `lib/css` and `lib/dom` and costs the other client nothing.
   `lib/layout` knows nothing of the DOM.
4. **Synchronous reads exist.** `getComputedStyle`, `offsetWidth`, `getBoundingClientRect`, `scrollTop` and
   `elementFromPoint` answer as of script's last write, so a read pays for what that write needs and no more.
5. **Power before throughput.** Threads exist so that script gets time, the UI stays responsive and content loads in
   the background, not to spread work across every core. Work goes wide only while someone waits for it, as for a
   synchronous read or a change that the user made, and background work such as a load uses no more threads than its
   roles need. Splitting work across threads has to pay for its own structure first, and is judged on energy as well
   as time.

## Threads and their roles

- **The page's thread** belongs to its agent. It parses, runs script, owns the DOM and CSSOM, and computes styles. The
  cascade reads the DOM directly, with no mirror of it, and `getComputedStyle` answers without crossing threads.
- **The layout side** is a role, not a thread: one thread at a time holds a presentation's layout tree. The agent's
  layout thread holds the tree while it lays out a job and records its scene. A synchronous read takes the tree on the
  page's thread once that job is done. Restyling reads nothing from the tree. It queues the changes to the boxes, and
  the tree takes the queue when its holder starts a job, so the page's thread styles the next job while the layout
  thread lays out the previous one.
- **One pipeline per agent.** The threads belong to the agent (`PrDomAgent`), not to a document. Every document of the
  agent, including iframes, lays out in turn on the agent's single layout thread and restyles wide on its single pool,
  so the number of threads does not grow with the number of documents. A host presents a top-level browsing context
  through a view (`pr/html_view.h`), which runs the steps between the event loop and the compositor.
- **The compositor** plays the last scene committed to it, scrolls it as a user does, and runs the animations of
  opacity and transform, on its own thread or in a host's frame, without the page's thread.
- **Going wide.** A restyle of whole subtrees divides them among threads while the page's thread waits, if someone is
  waiting for the restyle: a synchronous read, or a change after all of the document's content has been taken in. A
  load's restyles stay on the page's thread.

Style runs on the page's thread, and the box tree is where the threads meet. The alternative would keep a mirror of the
DOM's style-relevant state on the layout side and restyle there, alongside script, at the cost of the mirror, a log of
every mutation and a round trip for `getComputedStyle`. That machinery is worth its cost only if restyling is a
meaningful share of a page's script-thread time after restyling goes wide. The rule sets, invalidation, styles and
boxes are the same either way, so the alternative would change who owns the cascade, not how it works.

## A change, a read, a load

- **A change** marks what it reaches, as the rule set's invalidations say. The next rendering update restyles the
  marked elements and whatever their changes reach, and queues the boxes that changed. An element restyled only
  because its parent's style changed keeps the rules that it matched last, which the invalidations say still hold, and
  matches no selectors. The presentation's check against a full recomputation matches every element. The next job lays
  out the boxes that changed and their ancestors, reusing every fragment it can, and records again only the recordings
  whose fragments or inputs changed. A change to a paint-only value lays out nothing, and a change to a layer's effects
  records nothing.
- **A style read** restyles what is pending and answers without laying out, except for the used sizes that a layout
  decides. **A geometry read** brings the layout tree up to date, waiting for the layout thread if it holds the tree,
  and finds the node's boxes where layout placed them, at a cost proportional to the node's depth.
- **A load** displays progressively. Its source streams in, and the parser runs in tasks, one slice at a time, so the
  event loop reaches rendering updates between them. Sheets in the head hold back the first rendering until they load.
  A sheet that arrives later restyles what its rules reach. New content needs its own style and little else, and
  appended content reuses every earlier fragment. With a job budget, the presentation takes in new content in jobs sized
  to the budget, so that a change that a user makes meanwhile waits for one short job at most.

## The display and the user's preferences

What a display shows and what its user chose are two values from core: `PrAppDisplay` (pixel ratio, headroom, potential
headroom, gamut) and `PrAppUserPreferences` (color scheme, contrast, reduced motion, reduced transparency). A platform
fills both into each `PrAppFrame`, and each consumer takes them whole, so that a property added to either reaches every
layer with no change to an application:

- **Presenting:** the application's last pass over a half-float render maps its colors for the display's headroom and
  gamut ([draw.md](draw.md#color)).
- **Pages:** `prHtmlViewSetDisplay` and `prHtmlViewSetUserPreferences`, which set the target pixel ratio that a page's
  layout snaps to, and the media features that describe them (`resolution`, which reports the ratio in dppx,
  `color-gamut`, `dynamic-range`, the `prefers-*` features), which a page's styles and colors read. A
  browsing context gives both to its frames when they attach and whenever they change. A frame's preferred color scheme
  is its iframe element's instead.
- **Immediate-mode UI:** `PrUiFrameDesc::display` and `preferences`. The UI lays its tree out at the display's pixel
  ratio, and a widget reads both (`prUiDisplay`), as the kit's color picker reads the gamut.

A host that shows content in a different form from the display, such as an 8-bit swap chain on a wide-gamut display,
passes the display that the content is shown in. A host that changes that form, as when it turns HDR on, shows the
content in the new form only once the content has rendered for it: a presentation made for one form and shown in another
clips or misplaces its colors.

## Checking it

Each stage that caches work is checked in three ways.

- **Correctness.** `prCssPresentationVerify` and `prLayoutTreeVerify` recompute every stage from scratch and compare the
  result with the cache: computed styles, which nodes generate boxes, the boxes' records, the layout, the cached
  recordings and the geometry queries. On failure they report the first difference and the element or node where it
  occurs.
- **Bounded work.** The layout tree records which kinds of input changed since its last layout and its last paint:
  layout inputs (layout groups, text, children, natural sizes, the viewport), paint inputs (paint groups, replaced
  content, whether a box has a layer), and effects. The same verify calls fail if a layout did work although no layout
  input changed, or a paint recorded although no layout or paint input changed. So a paint-only change that lays out,
  or an effects-only change that records, is caught wherever it happens, not only where a test expects it.
- **Wasted work.** The work counters (`prCssPresentationStats`, `prLayoutTreeStats`) count what an update did, and what
  it did to no effect: elements restyled whose style did not change, and boxes recorded again in a paint-only update
  although nothing that they paint changed. A test asserts what a change costs, and a benchmark reports the waste.

The checks run on more than hand-written tests. `pr_shell --verify` checks a page's presentation after every rendering
update, and `tools/suite/wpt/wpt.py --verify` runs every page of a suite that way, which turns the mutations that WPT's
scripts make into checks of invalidation and caching. `frame_bench --verify` checks every case that it measures, on a
page of the default size and on a page twenty times larger. A benchmark run leaves the check out, so that it costs the
run nothing.
