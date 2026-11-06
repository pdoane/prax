# HTML

This document describes `lib/html`.

The library enforces the web's rules between origins, as Fetch and HTML define them. Isolating untrusted content from
the host, as by sites or processes, is the host's responsibility. A browsing context's frames share their agent, its
threads and its memory.

## Names

Element names are `PrDomTag`s. Names that the parser knows but webref lacks are listed in `tools/gen/gen_dom.py`, with
their justification. Lookup matches the exact spelling. The adjustments for foreign content (`textPath`) come from
`tools/gen/gen_html.py`. Named character references are `lib/html_entity`'s (`pr/html_entity.h`), a module of its own
that depends only on core, so that a library that does not depend on HTML, such as Markdown, can expand them. The
generator takes them from WHATWG's `entities.json`.

## Tokenizer

- Tree construction drives the tokenizer state (RCDATA, RAWTEXT, script data, PLAINTEXT, CDATA). `document.write`
  inserts at the read point, so there is no independent tokenizing pass.
- Only states that persist between tokens are stored. Character tokens are runs, flagged by their content.
  Tree-construction errors are unnamed.
- The tokenizer owns a copy of its input with newlines normalized, and inserted source is normalized too.
- `HtmlSpan` is an offset into either the input or the per-token values buffer (selected by the top bit of `start`).
  Never keep a pointer into the input across an insertion.
- With open input (a parser created by script), a token cut short by the end of the input is rolled back until more
  input arrives or the input closes (`htmlTokenizerNext`). A CR at the end of one write and an LF at the start of the
  next form one newline.

## Loading and scripts

- The browsing context owns the Window, its Document and loading. Creating it runs no script, so the host can attach
  bindings first. It must outlive the bindings that reference its objects.
- The library prepares scripts, and the host runs them through synchronous `PrHtmlHost` callbacks
  (`html_page.h`). A null `execute` disables scripting.
- Every resource is fetched through the host's fetcher (`PrHtmlHost::fetcher`, `pr/fetch.h`), as the document's
  origin, with its URL as the referrer, and in the mode that its destination takes. Each fetch runs on the fetcher's
  loop, which reads the response whole and posts it to the agent, and the response is used in a task. A data: URL is
  answered at once. A fetch that the context no longer waits for is dropped, and is cancelled on the loop.
- The tests and tools answer fetches themselves, from a test's or a capture's files, through lib/fetch's intercepting
  fetcher (`pr/fetch_intercept.h`). On the null network, whose loop runs on the host's thread (`pr/net_null.h`), the
  host decides the order in which responses arrive.
- A page loads the document at a URL by navigating (`prHtmlPageNavigate`): it fetches the document through the host's
  fetcher and loads it as it arrives, and its current document stays until the response arrives. A network error
  loads a document that says why. A navigation to another document by a hyperlink or by setting the location runs
  from a task, and aborts the load of the current document. Its fetch is from the document's origin, while the host's
  navigations and traversals of the session history are from none, so a fetcher can keep a host's own pages from
  documents. The page is the navigable: each document that it commits
  has a Window of its own, and the host learns of each Window as it opens and closes (`PrHtmlHost::windowOpened`,
  `windowClosed`). A navigation within the document, to a fragment, keeps its Window.
- The session history (`PrHtmlSessionHistory`, `pr/html_history.h`) is the host's for a tab, and outlives the browsing
  contexts that its navigations make. Each document that a browsing context loads takes an entry: a new one after the
  current entry, which drops those after it, or the entry that a traversal or a `location.replace()` asked for. A
  fragment navigation and `history.pushState()` add entries of the same document, and a traversal to one of them
  changes the document's URL and history state in a task and fires `popstate`, then `hashchange` if the fragment
  changed. A traversal to another document's entry asks the host to navigate to its URL. A browsing context without a
  history of its host's, as an iframe's, keeps its own.
- A window's `localStorage` and `sessionStorage` are its document's origin's areas in the host's providers
  (`PrHtmlHost::localStorage` and `sessionStorage`, `pr/html_storage.h`), which keep an area for each origin, under its
  quota, in memory or on disk (`lib/storage`). Local storage is shared by every browsing context, and session storage
  by a tab's navigations. An opaque origin, or a host that gives no provider, has no storage, and the attribute throws.
  `pr_shell` keeps both in memory and empties them for each run, and the browser keeps local storage in the user's
  data directory (`prFileUserDirectory`).
- Scripts run in this order:
  - A parser-blocking script pauses the parse until it is fetched and no sheet that the parser inserted is still being
    fetched (html/#pending-parsing-blocking-script). Then the script runs, and the parse continues in tasks. The XML
    parser pauses in the same way (html/#parsing-xhtml-documents).
  - Module scripts and deferred classic scripts run after parsing, in document order, each once it is fetched: a
    module script once its whole module graph is.
  - Async scripts run once they are fetched.
  - Then `settle` runs (and may run timers), for a load that ends within its call.
  - Then DOMContentLoaded fires, and load fires once the fetches that delay it are answered.
  - A script that script inserts is prepared once the insertion that connects it ends, or when its text or source
    changes while it is connected (html/#script-processing-model), through the document host's `prepareScript`. An
    inline classic script runs at once, inside the call that inserted it. An external or module script runs in a task:
    as soon as it is ready if it is async, as such a script is unless its `async` says otherwise, and otherwise in the
    order of insertion. The load event waits for both. A fragment parse, as for `innerHTML`, marks its scripts as
    already started, so they never run.

  Only parser-blocking classic scripts write at the read point (`prHtmlDocumentWrite`).
- The browsing context fetches module graphs, and the host parses each module (`PrHtmlHost::parseModule`), so that
  the script engine finds every module that a graph imports already compiled when it links the graph. The window's
  module map (html/#module-map) fetches and parses each module once, and fetches the modules that a module imports
  together. A module whose fetch fails, or whose response is not JavaScript, fires `error` at its script. A script's
  `import()` fetches its graph the same way (`prHtmlPageImportModule`). QuickJS gains what this asks of it
  in `patches/quickjs`: compiling a module without loading its imports, reading the specifiers that it imports, and
  a hook for `import()`.
- A failed `execute` or `settle` aborts the load. An exception in an author script is reported to the Window, and
  parsing continues.
- `pr_shell` is the test host. It fetches a page and streams it into its browsing context, as a browser loads one,
  from WPT's server or, without one, from the files of a WPT checkout, and any other page at its file: URL. A module
  evaluation that is still pending when nothing is left to run or fetch fails the load. The `settle` hook that waits
  for module evaluations spins the event loop (`prDomAgentSpinTask`), since a streamed load ends in a task.
- A host learns how a load ended, and why it failed, from `PrHtmlHost::loaded`.
- `DOMParser.parseFromString` runs no scripts, and its document is independent of the parser.
- A load's bytes decode in the charset that html/#encoding-sniffing-algorithm decides: a byte order mark, then the
  response's `charset`, then a prescan of the first kilobyte, then a frame's parent document, then the context's
  default (`prHtmlPageSetDefaultCharset`, windows-1252 until the host sets one). Nothing guesses a charset
  from the content. The prescan reads the content of `title`, `script` and the other elements whose content is text
  as text, and a `meta` element that the parser reaches past it counts only in the head, and never if
  `document.write` wrote it (whatwg/html#6962). Such a `meta` element changes the decoder in place if the bytes
  decoded so far read the same in both charsets, and otherwise the load parses its bytes again in the new charset.
- Bytes that decode to themselves are not copied: well-formed UTF-8, and ASCII in any charset whose ASCII is ASCII,
  reach the tokenizer as the load's own bytes. A streamed load whose response names no charset waits for the first
  kilobyte, which the prescan reads, before it parses.
- A classic script decodes in its response's charset, or its `charset` attribute's, or its document's output charset
  (encoding/#get-an-output-encoding). A module script is UTF-8. A style sheet decodes as css-syntax-3 says, with its
  link's `charset` attribute or its document's charset as the fallback. A URL that a document resolves has its query
  percent-encoded in the document's charset (`domDocumentParseUrl`).
- A load streams if its source arrives in pieces (`prHtmlPageBeginLoad`). Tasks parse what has arrived, one
  slice at a time, so the event loop reaches rendering opportunities between them. An HTML document is render-blocked
  while the parser is in its head, which may still add a sheet, and while the sheets that the parser added there are
  being fetched. While it is render-blocked, its rendering is not updated, and a host paints nothing.
- An XML document streams too. Its charset is that of its byte order mark, or of its response, or else the one that
  its XML declaration names (Appendix F of XML), so a streamed load waits for the end of the declaration before it
  parses. The XML parser reports each piece of content, such as a tag or a comment, once the whole of it has arrived,
  so that a document reads the same however its bytes are divided.
- Autofocus candidates (html/#flush-autofocus-candidates) are flushed in the tree order of the elements that carry the
  attribute, HTML's, SVG's and MathML's alike. A document with a frame of its own flushes its own candidates.

## Markdown

- Markdown (`pr/html_markdown.h`) builds the DOM that parsing lib/markdown's HTML of it gives. A text/markdown resource
  loads as an HTML document whose body holds that DOM, and an element's children can be set to it, whole or as the
  source streams. Tasks build a streamed document a slice at a time, as they parse HTML. A document parses the variant
  that its content type names (RFC 7763): `CommonMark` alone, `GFM`'s extensions, or, for any other variant or none,
  the extensions that GitHub renders.
- The builder takes the events of lib/markdown's HTML (`prMarkdownWriteHtml`) and feeds tree construction
  tokens that it makes from them (`htmlTokenizerMakeTag`, `htmlTokenizerMakeCharacters`), so that Markdown's text is
  never tokenized. Raw HTML goes through the tokenizer. While raw
  HTML leaves the tokenizer within a token or in a state other than data, the builder writes what follows as HTML for
  the tokenizer to read, so that the DOM still matches the parse of the whole HTML.
- A change to the Markdown rebuilds from the last clean point before it. A clean point is a top-level block before
  which the parse is at rest (`htmlParserIsAtRest`): only its base elements are open, no formatting element is active,
  and the tokenizer is idle. The DOM before a clean point depends on nothing after it, so the builder removes what
  follows the point, restores the parse and builds the blocks again. Markdown without raw HTML that stays open has a
  clean point before every block, so a streamed source rebuilds only its tentative blocks. The footnotes follow the
  blocks, after a clean point of their own.
- An element's contents are parsed as a fragment whose root redirects to the element (`HtmlParser::rootTarget`), so
  they appear in the element as they are parsed.
- A Markdown document runs no script: its parse has no script engine, and its event handler content attributes do not
  compile. Its user agent sheet (`kHtmlStyleMarkdown`) applies only to it (`PrCssSheetCondition_Markdown`).

## The event loop and rendering

- The event loop belongs to the agent (`prDomAgentRunTask`). It runs the tasks of every window on the agent, including
  an iframe's window, and the windows that have a task due take turns. Each window keeps its own task queue and
  timers. A discarded iframe's window leaves the loop, and a window whose load is running on the stack runs no task
  except from its settle hook.
- The Window's timers, animation frames and animations run on the host's clock (`PrHtmlHost::now`), or else on the
  system's clock. A host that steps its own clock runs pages deterministically, as the tests do.
- A rendering update reveals the document, fires the scroll events that are due, moves its presentation's animations to
  the frame's time and fires the transition and animation events that changed, runs the animation frame callbacks, and
  computes the document's styles, which starts the transitions that its changes make. The host's view then lays out and
  paints the document. Updates are due while frames are requested, while events are waiting, if styles changed, or while
  the presentation's animations run. A page is idle (`prDomAgentIsIdle`) once nothing is pending except frames for
  animations that send no more events, so an infinite animation's iterations do not keep a page busy.
- A view (`pr/html_view.h`) is how a host presents a browsing context. It sizes the viewport, which the media
  environment follows. It commits the scene of each job to a compositor and starts the next job if something changed.
  It hands the offsets that the compositor scrolled back to the document, whose next rendering update fires their
  events. And it routes the host's input (`PrAppEvent`, `prHtmlViewEvent`) to the document's elements. A host calls the
  view when its frames start, as tasks run, and when the agent's layout thread finishes its jobs
  (`prDomAgentSetLayoutDone`).
- A user's wheel scrolls in the compositor, on the host's thread, unless the document may cancel it. An element's
  wheel listener that is not passive puts the element's box, with what its descendants reach, in its scene's wheel
  region, and such a listener on the window, the document, its root or its body puts the whole viewport there. The
  host asks the compositor whether a wheel waits (`prLayoutCompositorWheelBlocks`). The view fires a cancelable `wheel`
  for a wheel that waits, and scrolls the document as the event's default action. The host scrolls a wheel that does
  not wait, and the view then fires a `wheel` that cannot be canceled (`PrHtmlViewEventFlag_Scrolled`). A sequence of
  wheels is one gesture. It keeps the target of its first wheel, and its first wheel decides whether the rest wait.
- Input reaches into frames. The view hit tests through a frame's content box into the frame's document, so the
  pointer and the wheel go to that document's elements, and keys go to the focused frame's focused element. A scroll
  that a frame's document cannot take passes on to the containers around the frame, unless its root's
  `overscroll-behavior` contains it, in the view and in the compositor alike.
- The pointer's shape comes from the document. The view hit tests the pointer's position at each hover and at each
  commit of a scene, and reads the `cursor` property of the element that it hits (`prHtmlViewOutput`), which the host
  returns from its frame. Hit testing honors `pointer-events`, `visibility` and inertness, so the element hit is
  the one that takes the pointer's events.
- Some states of an element are the host's to know: whether a link's URL is in the user's history (`:visited`), and
  whether the user agent autofilled a control (`:autofill`). A restyle asks the host (`PrHtmlHost::elementState`), and a
  script always sees a link as unvisited. The host calls `prHtmlPageInvalidateHostStates` once what it knows changes,
  which restyles the elements that the change can reach.
- A rendering update does not wait for the layout thread. Its styles are computed at once, and the layout tree takes
  them when the view starts the next job, so the page's tasks continue while the layout thread lays out
  (`prCssPresentationIsBusy`).
- A click that is a MouseEvent runs the activation behavior of an a or area element, or of an SVG a element that is a
  hyperlink, at or above the click's target. The activation behavior follows the hyperlink. Following a URL of the
  document with a fragment scrolls to the element that the fragment indicates, and that element becomes the document's
  target element (`:target`).

## Printing

- A view prints by laying its document out as pages (`prHtmlViewSetPaged`, [layout.md](layout.md#paged-media-prlayout_pageh)).
  A paged view's window matches the print media type. The host gives the box that a page has before the document's
  rules apply, as its paper and margins give it, and the document's `@page` rules give each page's size, margins and
  background over it (css-page-3/#page-properties), by the page's name, side, and whether it is the first or a blank
  page.
- Layout asks for a page's box on whichever thread lays the tree out, and the rules are the document's, which only the
  document's thread reads. So the view resolves the boxes ahead, for every kind of page that the rules' selectors tell
  apart, each time the host asks for the page count, and layout reads only those boxes.
- Each page's margin boxes come from its @page rules' margin rules (css-page-3/#margin-boxes), which the
  presentation makes into boxes of the tree once the content is laid out into pages (css.md, Page-margin boxes). So
  counting pages lays the content out, makes the margin boxes from what each page holds, and lays out again, which
  lays out only the margin boxes that changed.
- Each page is painted into a list of its own (`prHtmlViewPaintPage`): its background, the document's content on the
  page, with the canvas's color in its page area, and its margin boxes, all through the tree's scene.
- A paged view shows its pages down its viewport as a print preview does, each sheet on white paper, the gap apart that
  its host gives (`prHtmlViewSetPaged`). It records the sheets alone, and what lies around and between them is the
  host's to draw.

## Replaced elements

An `iframe` is a replaced element whose content is embedded. Its slot shows the document of its browsing context. That
document is laid out in a viewport of the iframe's content box. A script's query, or a scroll, in the frame's document
first brings the parent's layout up to date and takes the content box from it (`DomDocumentHost::viewport`), so a frame
that measures or scrolls itself while it loads sees its real size. A view lays the document out in the extent of the
slot that the last scene around it gave, which is the same box. The view prepares every document's job before it posts
any, so the jobs run back to back on the agent's layout thread, and it commits each job's scene to the compositor by
the document's id.

An `img` is a replaced element of its presentation's tree. Its box is sized by the natural size of the image of its
current request, and draws the image, decoded by the host's codec (`PrHtmlHost::images`). An SVG image is instead parsed
as its own document and drawn by `lib/svg` at the size that the box shows it ([svg.md](svg.md)), and loads nothing else.
If there is no image, the box draws nothing. A new image tells the presentation that what the element shows has changed,
which sets the box's natural size and content without a restyle.

## Button layout

A `button` is laid out by HTML's button layout (html/#button-layout), which the presentation applies when it projects
the element's box. A display other than flex or grid behaves as `flow-root`, or as `inline-block` if the box is
inline-level. The box is a semi-replaced element (`PrLayoutNodeKind_SemiReplaced`), so that an auto inline size in block
flow is its fit-content size, while alignment and insets can still stretch it.

## Media queries

The Window owns the media environment, and `lib/css` evaluates it (`css_media.h`). The scripting feature reflects
whether `execute` exists. A sheet that is in no document matches nothing. A change to the environment restyles the
document only if a media query that decided its rule set now answers differently, and a new viewport restyles it only
if a style resolved a viewport-relative length.

The environment also describes the display and the user's preferences, which the host sets
(`prHtmlPageSetDisplay`, `prHtmlPageSetUserPreferences`), and which the context gives its
frames ([rendering.md](rendering.md#the-display-and-the-users-preferences)). Script cannot read the headroom. The
`color-gamut` and `dynamic-range` media features follow the display, and a display matches each lesser value of either,
as a P3 display matches `(color-gamut: srgb)`. Colors resolve for the headroom when an element's style is projected
onto layout, never in the draw list: `color-hdr()` picks between its colors, and `dynamic-range-limit` lowers the
headroom that an element allows, caps its colors, and caps its replaced image through the image content's limit, which
the image brush applies without a layer. A new headroom projects again only the boxes whose records read it. Colors
past the display's gamut stay in the draw list and the presentation as they are, and the application maps them into it
([draw.md](draw.md#color)).

The reader's preferred color scheme is the `prefers-color-scheme` feature, which the user's preferences set. An
element's used scheme follows it if the element's `color-scheme` names both schemes. An element whose `color-scheme` is
`normal` uses the page's supported schemes, which `<meta name=color-scheme>` gives. A box whose colors read the
preference is projected again when it changes. A frame's document prefers the scheme of its iframe element, which the
view carries over on each update. The canvas is the `Canvas` color of the root's scheme, which the presentation gives
its layout tree as the tree's canvas color (`prLayoutTreeSetCanvasColor`), so that the scene reports it like any
client's canvas (layout.md). A frame's canvas has that color only if its root's scheme differs from its iframe's
(`PrCssPresentationEmbedding`), and is transparent otherwise, so that an embedder sees through.

## Inline stylesheets

A style element contributes a sheet if its root is the document and its type is absent, empty or `text/css` (ASCII
case-insensitive), and it contributes the sheet before the next parser script runs. The document's sheet list is in
the tree order of the owner nodes, not in discovery order, because foster parenting can reorder them.

## Text controls

A `<textarea>` or a text `<input>` shows its value in `::field-content`, the box that scrolls. While a control is empty
its placeholder shows in that box, which then takes the rules of `::placeholder`, so the caret needs no special case. A
password shows a mark for each code point unless `input-security` is `none`, and the view maps between the offsets of
what it shows and those of the value.

The view runs the user's motions and edits (`pr/text_edit_command.h`), and reports the caret to the window
(`prHtmlViewOutput`). `::selection` colors the selection. Cut, copy and paste fire their events, whose listeners can take
them over through `clipboardData`. The clipboard belongs to the application that the host gives the view
(`prHtmlViewSetApp`), and a paste's text comes back as a Clipboard event that the host passes on.

## Structure

A document's structure (`pr/html_structure.h`) is a pass over the DOM, apart from painting, that builds the structure
tree of [draw.md](draw.md#the-structure-tree). A writer, or a platform's accessibility tree, joins the structure to the
marks that layout painted. A view records marks when its host asks (`prHtmlViewSetMarking`). The pass reads the DOM,
and reads style and layout only to learn what is hidden.

- **Roles.** Each element's role is its `role` attribute's first token that names a WAI-ARIA role, or else its implicit
  role, as HTML-AAM's element role mappings give it. The role decides the node's structure type: headings by their level
  (`aria-level`, or the element's rank), paragraphs, lists and their items, tables, rows, header and data cells, figures
  and images, links, block quotes, code. An element whose role is generic, none or presentation adds no node, and its
  children join its parent's. So does a role that has no structure type of its own.
- **Content.** A text node is a key child of the node of its nearest element that adds one. An image, and any element
  whose role is img, is a figure whose alternate text is its accessible name (accname-1.2/#computation-steps), and its
  own key is its child, since layout marks a replaced element by its element. Its descendants add nothing.
- **Paragraphs.** Text and inline content directly in a node that holds blocks, such as a section or a division, is
  gathered into a paragraph, which ends at the next block. A list item's content is its body (`LBody`), beside its
  marker, and a description list's terms are labels (`Lbl`).
- **Reading order** is the DOM's, with the elements that `aria-owns` names moved to the end of their owner, each once.
- **Hidden content** is left out: an element whose box is not rendered (`display: none`, or no box at all), or that is
  `aria-hidden` or `hidden`. Its marks, if it painted any, then belong to no node, and the writer writes them as
  artifacts. Text that `visibility` hides paints no marks, so the references to it resolve to nothing.
- **Generated content** of `::before`, `::after` and `::marker` is not in the DOM. Its text lies in anonymous boxes,
  which have no key, so layout paints it unmarked and a writer writes it as an artifact.
- **Page-margin boxes** are a pagination artifact (PDF/UA), and their boxes have no key, except the content of a link
  inside a copy of a running element (css-gcpm-3/#running-elements). Each such copied box has a key of its own, which
  the presentation lists with its link, and the structure makes a link of each copy, after the document's content, so
  that the link stays a link on every page. The running element itself, which no box shows where it is, is left out.
- **Names and text.** A link's URI is its `href`, resolved against the document's base URL. A link to a fragment of the
  document goes to the first key of the element that the fragment indicates, in tree order. A heading's title is its
  text content, which the outline shows. An `abbr`'s `title` is its expansion. An element's `lang` is its node's
  language when it differs from its parent's, and the document's language is the root's.
