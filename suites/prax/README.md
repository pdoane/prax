# Our pages

`suites/prax` walks CSS, SVG, MathML and input for a person to read and act on: CSS 2 chapter by chapter, every CSS
module section by section, SVG 2 chapter by chapter, MathML Core, HTML's chapter on its elements, and the
specifications of input and focus (UI Events, Pointer Events, and HTML's chapter on user interaction). A page shows
what the specification has a reader see, and where Chrome, Safari and Firefox draw something else. The workloads, real
pages that pull those features together, sit beside them.

This file holds the suite's rules and the decisions made along the way, with their reasons. `docs/testing.md` says how
the suite sits among the others.

## What the pages are, and are not

- **What is drawn, for a person to look at.** Parsing and serialization are not the suite's subject: WPT covers
  them, and they are not for a human to look at. No page or figure is about what parses, how a value serializes, or a
  computed or resolved value's string. A section that is only syntax, serialization or the object model has no page.
  A value's effect is shown by what is drawn; a value is read back (`.pr-computed`) only to give a size the reader
  cannot measure off the guides, never as the point of a figure. An invalid declaration is parsing too, even where the
  fallback it leaves is drawn differently: no figure shows a value being dropped.
- **The specification's content, not the engine's.** A page is written from the specification's source (`./dev.sh
  spec`), never from what the engine draws, which is what the page tests. No page says what the engine chose where a
  standard leaves a choice or where it follows the browsers against the text: the engine's choices are in its unit
  tests, which record them, and in `docs/`.
- **Browsers are compared, not followed.** The expected result is the authority's: the specification's, or a
  resolution newer than it. Every browser that departs is flagged after it (below).
- **Direct use of features.** A figure leaves no doubt whether the feature is supported: where it is not, the figure
  looks plainly wrong. A figure's stage is real markup using the feature under test. Nothing draws the expected
  result instead of the feature: no positioned boxes imitating a layout, no images of the answer. Nor does a figure set
  beside the real result a copy of it drawn another way, which shows the same thing twice. Markers, guides (`pr-ghost`,
  `pr-baseline`, `pr-expect`) and annotations mark up the real result, on it: a guide may be what the specification
  says the result equals, drawn another way over the feature's own result, such as a shape's equivalent path outlined
  on the shape.

## What is covered

All of CSS: CSS 2's chapters (`css2-08-box-model` … `css2-18-user-interface`, and `css2-e-stacking-contexts` for
Appendix E), and **every CSS module, each walked whole**: every section with something to see has a page, including
features no browser ships and features the engine does not implement yet. Pages come before the implementation. Within
each module, what is drawn is in and parsing and serialization are out, so a module that is only syntax or object model
has no directory. SVG 2 is walked the same way, a directory for each chapter that draws something
(`svg2-10-basic-shapes`), named by its number and title as CSS 2's chapters are. Input is walked as the parts of its
specifications that a user's input reaches: `uievents`, `pointerevents`, and HTML's chapter on user interaction, named
by its number and title (`html-06-user-interaction`). MathML Core is walked as a module is (`mathml-core`), and MathML
4's presentation markup as a chapter of CSS 2 is (`mathml4-03-presentation-markup`). A supports page of these lists the
interfaces and members that the pages use (`.pr-api`) beside any declarations, and a directory whose pages match
pseudo-classes or pseudo-elements lists those selectors (`.pr-selector`). `index.txt` lists the directories walked so
far, in the order of their names, each with the specifications it walks and a note where a module's content moved or a
later level is not walked yet; the rest are to be walked as well.

## Pages that walk no specification

A page that walks a specification shows one feature at a time, so a page's worth of features, or every state of one
control, is scattered across many pages and directories. Two directories gather them instead. `index.txt` lists each
by its name alone, and its own index lists its pages and no sections (`tools/suite/prax/suite.py`). Neither has a supports page,
an eyebrow, citations or guides. What the rest of this file says of a page's markup and of the browsers still holds:
each page fits 600 by 800, uses its features directly, and cites no test suite. A page is right if it looks as its
author meant it to in every browser, and a difference between the browsers and the engine is found by looking at them
side by side. A third, `syntax`, holds sources for an editor rather than pages for a browser (below).

### Workloads

`workloads` holds real pages, a checkout form, a dashboard, a settings panel, an invoice, each pulling together the
features that such a page uses, as a site would. They come from peregrine4's workloads, and grow where a feature has no
page that uses it in context.

- **A workload is a page, not a figure.** It keeps its own look, which it styles itself on the base that
  `support/workloads.css` gives, and none of the suite's chrome: it starts with its content, and a comment in its head
  says what the page is and what it leans on.
- **Its content is plausible.** The text, numbers and names are a real page's, so that a line breaks, a table sizes
  and a control fills as they would on a site.

### The forms gallery

`forms` gathers HTML's form controls as a gallery gathers a toolkit's widgets: a page for each control, or for what
gathers controls, as `examples/studio_gallery` gathers Studio Kit's.

- **A grid of states against appearances.** A control's page has a row for each of its states and a column for each
  appearance (`auto`, `none`, `base`, and an author-styled or restyled column), then panels that show it in each color
  scheme, with an `accent-color`, in large text, right to left and in a vertical writing mode.
- **Every control is live**, so that a reader clicks, types and drags in the browser and the engine side by side. A
  page that runs a form shows what its events and entry lists held.
- **The markup is static**, so that a page shows every state before any control works. Script only sets what markup
  cannot, such as `indeterminate`, and reports what the controls did.

### The syntax samples

`syntax` holds a source in each language that `lib/syntax` highlights, to open in Scribe (`./dev.sh app scribe
<file>`) and look at as a reader would. The unit tests check what the languages name; these show how it reads.

- **A file is named for its language**, with the extension or the whole file name that selects it (`rust.rs`,
  `Dockerfile`), so that the editor picks its language as it would any other file's. HTML's is `html.htm`, so that
  the tools that open the suite's pages in a browser leave it out.
- **Its content is plausible**: a small program, configuration or document of the kind the language is written for,
  that uses what the language's highlighting tells apart (comments, strings and their escapes, numbers, keywords,
  types, functions, constants, and the language's own constructs, such as Rust's lifetimes or a shell's expansions).
- **It is valid**, so that what looks wrong is the highlighting, not a parse that the source made fail. Where a
  language holds another, as Markdown's fences, PHP's pages or a shell's heredocs do, the sample includes it, to show
  the injection.

## How it is organized

- **Index files order the pages** (`tools/suite/prax/suite.py` describes the format). `index.txt` lists the directories in the
  order of their names, so that a reader looking for a module finds it where its name puts it (`compositing`, the `css-`
  modules, CSS 2's chapters, `filter-effects`, `forms`, HTML's chapters, `pointerevents`, SVG 2's chapters, `uievents`, `workloads`). A directory's `index.txt` lists its sections in the
  specification's order, each by anchor, number and title, the pages of each indented under it, and a `#` comment for
  each section without a page saying why. The viewer lists pages in this order, under their sections.
- **A directory is a module without its level** (`css-sizing`), and walks the module's latest complete level. A later
  level that only adds to it (a delta, as css-sizing-4 is) puts its sections in the same directory, after the base
  level's, each naming its own specification. The levels are the editor's drafts, as WPT's tests are, whether or not
  a draft of a level has been published; a level not walked yet is noted in `index.txt`. The directory's line in
  `index.txt` names
  every level it walks, its base level first, and its own index names the same levels.
- **A page is named by its subject** (`style-matching.html`), never by a number, so a draft that renumbers, or a move
  to a later level, changes the index and the eyebrows, not the paths.
- **Supports pages** come first: `00-supports-1.html`, then `-2` and on where the declarations need more room. One
  declaration per property the directory's pages use, but the pages' own look (`shared/page.css`); green where the
  engine parses it.
- **SVG 2's chapters are pages of one specification**, and an anchor such as `Introduction` recurs in each, so a
  section of SVG 2 is named by its page and anchor (`SVG2/shapes.html#RectElement`), and so is a chapter in
  `index.txt` (`svg2-10-basic-shapes SVG2/shapes.html`). Webref has no heading for a chapter, so the supports page's
  eyebrow gives the chapter's title as the specification does (`SVG 2 · 10 Basic Shapes · Support`). The supports
  page lists the geometry properties (`cx`, `r`, `rx`, `width`) that the pages set from CSS; presentation attributes
  are markup, not declarations.
- **CSS 2 pages keep CSS 2's text.** Where a later specification revises what a page shows, a `pr-later` card follows
  the cards it revisits, citing the section that says so; the module's directory walks that section in full. Where a
  module replaces a chapter, as css-fonts does chapter 15, the chapter's pages point to the module's.

## A page

- Fits 600 by 800 without scrolling (`./dev.sh prax fit`); aim for 790 in Chrome. A topic that needs more takes more
  pages, listed one after another under the same section.
- `<title>` and `<h1>` are the page's subject. The eyebrow is `<Spec name> · <number> <title>`: the specification as
  webref titles it without "Module" and "Level" (`suite.spec_name`), and the section's title, which may stop at its
  colon or go on (", step 4").
- The lede quotes the section's key sentence, then cites it. A citation is a link to the section's anchor in the
  editor's draft whose text is its number: `<a href="https://drafts.csswg.org/css2/#abs-non-replaced-width">CSS 2
  §10.3.7</a>`, or for SVG 2, `<a href="https://svgwg.org/svg2-draft/shapes.html#RectElement">SVG 2 §10.2</a>`. Every
  `§` on a page is inside such a link, quotations aside.
- Cards (`<section>`) each state a requirement, quoted verbatim from the source in `<q>`, then figures. A figure shows
  one requirement, and its caption says what the reader sees ("B covers A", "200px wide, on two lines"), with numbers
  the reader can check against the stage's 10px guides.
- `shared/page.css` holds every shared class, each with a comment; page-specific style goes in the page's `<style>`.
  A pattern that pages repeat, in any directory, becomes a shared class: `.pr-defs` for an svg that only holds
  definitions, `.pr-ghost` on an SVG shape for a dashed outline.
- **Fixtures** that one directory's figures use, such as images and documents, are in its `support/` directory; those
  that figures of several directories use are in `shared/resources/` (the flag images, 40 by 20 in quarters).
- **Motion.** A figure may move where motion shows its point better than a still does: a plane turning through
  `rotateY`, an origin sweeping. Its stage is `.pr-motion`, and its animations run all the time, so that a reader
  flipping through sees them move. A still pauses every `.pr-motion` stage's animations at load (`./dev.sh prax
  stills`), at the frame that the declared style or a negative `animation-delay` picks, the same in every browser and
  the engine. The caption says what that frame shows, as for any figure, then what the reader sees as it moves. No page
  pauses its own animations: a figure about a frame of an interpolation runs it too, and its still is that frame.
- **Interaction.** A figure about input asks the reader to act on its stage, by a click, a key, Tab, an input method
  or the wheel, and its caption says what the reader then sees. What the input does is drawn where it can be (a box
  takes the focus ring, a field holds the text typed, a box scrolls), and a `.pr-log` lists the events that reached
  the stage with the fields that the figure is about (`shared/events.js`). A figure that needs a listener of its own,
  such as one that cancels, sets it in the page's script and says so in the card.
- **Printed output.** A screen never draws pages, so a figure about paged media or fragmentation across pages has a
  stage `<iframe class=pr-paged srcdoc="…">` holding a whole small document with its own `@page` rules, at a small
  page size so that pages tile in a sheet: `@page { size: 240px 160px; margin: 10px }`. On screen the frame shows the
  document's continuous layout. The stills print the document in each browser and show the pages below the screen
  columns. Relative URLs in the document (`../shared/fonts.css`) resolve against the page, as on screen.

- **Color past sRGB and SDR.** Stills keep wide-gamut and HDR color: a page that draws past sRGB is captured in a
  format that holds it, which a display that shows it shows in full. A figure about such color still sets its subject
  beside a reference drawn within sRGB, such as `white` beside HDR Reference White, so that the difference reads on any
  display, and the caption says what a capable display shows and what an SDR one shows.

## Flags

- **Every deviation from the authority is flagged.** The authority is the specification's normative text, or a
  resolution that is newer than the text and not yet applied to it (below). The expected result is what the authority
  gives, and every browser that differs from it is flagged: one browser or all three. Nothing else changes what is
  flagged: not how many browsers agree, not an open issue, and not a reason a browser may have.
- **A departure** is `<figure class=pr-erratum>`; its caption gives the expected result first, then `<span
  class=pr-flag>Chrome</span>` (Safari, Firefox) and what that browser does instead. A feature no browser ships is
  flagged in all three. The engine is never flagged.
- **Only normative text is an authority.** What a Note, an example, or a "recommended" or "may" sentence describes
  is optional: a figure may show it and say what each browser does, without a flag. (css-inline's line metrics: the
  sTypo recommendation is in a Note, so browsers using hhea are not flagged.) HTML's Rendering chapter is an
  authority: its "expected" has the force of "must" for a user agent that supports the suggested default rendering,
  and the browsers render by it, so a departure from it is flagged.
- **Where the authority gives more than one result, the figure gives each.** Two sections that contradict each
  other, or a sentence that reads two ways, each give an expected result. The caption sets them out and cites
  where each comes from. A browser is flagged only if it matches none of them. Where the authority gives no result,
  the caption says so and flags no one. The suite does not choose between the results; docs/behavior.md is where the
  engine chooses.
- **Three browsers agreeing against a reading is a reason to check the reading.** The agreement suggests a mistake
  in the page's own reading, so before flagging all three, the result is re-derived from the whole algorithm, and the
  issues are read for a resolution that revises the text (below). A reading that holds is flagged in all three, as
  any departure is.
- **A page stands on the specifications, their resolutions and the browsers.** WPT has no part in the suite: a
  caption argues from the text and says what the browsers do, and a figure that follows a resolution against the
  text's letter cites the resolution (`tools/suite/prax/suite.py` reports a page that names WPT).
- **Every difference on a still is accounted for**: where a box is placed as well as how it is drawn. A difference a
  caption does not mention is a flag to add, or a sign the figure tests two things at once.
- **A resolution newer than the text is the authority.** A specification's text can lag its working group's
  decisions. Where the working group has resolved what the text does not yet say, or the engines' developers have
  agreed on it in an issue, the resolution decides the expected result, and the page says so, quoting the text and
  citing the issue: the omitted flex basis is `0%`, as csswg-drafts#5742 resolved, though css-flexbox-2's text still
  says `0`. A resolution counts whether or not its issue is closed, but only if it is newer than the text that it
  would revise: an issue resolved in 2017 may be what the draft already says, or what a later edit replaced, so its
  date is checked against the history of the draft's source. **An open issue means nothing to the suite**: anyone can
  open one, and until it is resolved it neither changes the expected result nor excuses a browser. A resolution is
  cited by a link whose text names its issue:
  `<a href="https://github.com/w3c/csswg-drafts/issues/5742">csswg-drafts#5742</a>` (`tools/suite/prax/suite.py` checks the
  form, and reports an issue named without a link).

## Fonts

- **The fonts are the checkout's fixtures**, from `third_party/fonts` (`./dev.sh`'s `fonts()` fetches them), named
  "PR …" in `shared/fonts.css` so no installed font stands in. Never `build/font_bundle`: those are the product's
  default fonts for applications, which may change for reasons of their own.
- **Text is for reading.** No Ahem or other test fonts that draw boxes. Where a size comes from text, `.pr-measure`
  sets real words in Noto Sans Mono at `calc(50px / 3)`, so every character is 10px wide and "auto" is 40px against
  the guides.
- **Math layout is measured against the font's own MATH table.** A guide marks the position that a constant of the
  formula's real math font gives, such as a superscript's baseline at SuperscriptShiftUp, and the caption names the
  constant and its value, which `shared/fonts.css` lists for each math font, so the reader can check it.
- **A font's own value and the fallback, both.** Where the specification takes a value from the font and falls back
  without it (baselines, metrics, features), one figure uses a font that has it, so the reader sees the font's value
  used, and another one that lacks it, so they see the fallback; synthesis is the engine's to do, and the page tests
  it. "PR Baseline Diagnostic", WPT's, has every BASE baseline at its own height.
- A figure that needs a font the checkout lacks gets one added as a fixture, rather than a weaker figure.

## Tools

- `./dev.sh prax stills --out <directory> <directory of the suite>... [--page NAME]` captures each page in Chrome,
  Safari, Firefox and the engine, and a sheet of the four side by side, with each browser's printed pages of every
  `pr-paged` figure below them (`--print-only` captures just those; printing needs poppler's `pdftoppm`). Stills are
  for a user who prefers a dark color scheme, unless `--scheme light` says otherwise.
- `./dev.sh probe <page> --screenshot <directory>` captures one page in each browser and the engine at 600 by 800,
  for checking a figure while writing it.
- **The Safari column** is the system's WebKit, Safari's engine, in a browser of the build's own
  (`tools/browsers/webkit_host.cpp`, on lib/app), which is fast, runs at any `--pixel-ratio` and in either color scheme, and
  keeps no state. It captures the page as the window server composites it for the display. `--safari`, on either
  command, captures the column in Safari itself, through `safaridriver` (Safari's "Allow remote automation" setting
  turns it on), where a capture must be Safari's own. Safari draws at the display's pixel ratio and in the system's
  appearance.
- `./dev.sh viewer` shows the suite's pages in the engine, in order under their sections, with their motion running.
  The engine's render of a page takes input as the browser's live page beside it does, so a figure that asks the reader
  to act is reviewed in both.
- `./dev.sh prax fit [directory...]` lists the pages that do not fit 600 by 800 in Chrome.
- `./dev.sh lint` runs `tools/suite/prax/suite.py`, which checks the indexes, eyebrows, citations, links between pages and
  supports pages against webref's headings; `./dev.sh prax fix` rewrites numbers where a draft renumbers.
- `./dev.sh spec <name> [pattern]` prints a specification's source verbatim.
