// The incremental example: a document that arrives in pieces, as a language model's answer or a slow network delivers
// it, rendered as it arrives, beside Studio Kit's controls for the stream.
//
// Three samples stream in their own way. The Markdown answer is a text/markdown document, loaded as it arrives
// (prHtmlPageBeginLoad): its final blocks keep their nodes, and the blocks after them are built again from each piece.
// The chat is an HTML page whose messages are Markdown, each streamed into its own element (pr/html_markdown.h) as the
// answer to a question arrives. The HTML page is a text/html document, loaded as it arrives. A Markdown document of the
// same pieces runs beside a Markdown sample and reports what each piece changed (prMarkdownTakeChanges), which the
// status line shows.
//
// The page runs on the window's thread. Before each frame, the stream delivers the pieces that its rate allows, the
// page runs its due tasks, and the view records the page into a draw list (prHtmlViewPaint) for the pane that the last
// frame laid out. The pane is a replaced element whose painter embeds that list.

#include "pr/app.h"
#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/css_view.h"
#include "pr/dom.h"
#include "pr/draw_list.h"
#include "pr/draw_renderer.h"
#include "pr/font_bundle.h"
#include "pr/gpu.h"
#include "pr/html.h"
#include "pr/html_markdown.h"
#include "pr/html_page.h"
#include "pr/html_view.h"
#include "pr/kit_box.h"
#include "pr/kit_button.h"
#include "pr/kit_choice.h"
#include "pr/kit_input.h"
#include "pr/kit_surface.h"
#include "pr/kit_text.h"
#include "pr/kit_theme.h"
#include "pr/layout_frame.h"
#include "pr/layout_scene.h"
#include "pr/layout_unit.h"
#include "pr/markdown.h"
#include "pr/text.h"
#include "pr/text_font.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

using namespace pr;

// ---------------------------------------------------------------------------------------------------------------------
// The samples

enum {
    Sample_Markdown,
    Sample_Chat,
    Sample_Html,
    Sample_Count,
};
typedef uint32_t Sample;

constexpr const char* kSampleLabels[Sample_Count] = {"Markdown answer", "Markdown chat", "HTML page"};

// An answer such as a language model writes. The links' references are defined at the end, so each link resolves only
// once its definition's line arrives, and the footnotes list follows the answer once their references arrive.
constexpr const char* kAnswer = R"md(# Streaming Markdown

Markdown that arrives in pieces is parsed **as it arrives**. The blocks that a later line can no longer change are
*final*, and only the blocks after them are parsed again. A reader sees the answer grow a few words at a time, the
way a language model writes it, while the page underneath stays put.[^final]

> [!NOTE]
> Nothing in this answer is special-cased for streaming. The same parser reads the whole text at once, and a stream
> that ends gives exactly the page that the whole text gives.

## What a new piece can change

Most pieces only lengthen the open paragraph. A few change more than that:

1. A paragraph's next line may turn it into a heading, if that line is a row of `=` or `-`.
2. A blank line between items makes a whole list loose, which wraps every item in a paragraph.
3. A delimiter row turns the paragraph above it into a table's head.
4. A closing fence ends a code block, and until it arrives everything after the opening fence is code.
5. A definition at the very end can turn earlier text into a link, as the [CommonMark spec][spec] allows.

| Piece | What it changes | How often |
| :--- | :---: | ---: |
| a word | the open paragraph | almost always |
| a newline | maybe the block's kind | often |
| a blank line | whether the open block ends | often |
| a definition | earlier blocks that use its label | rarely |

## Costs

Parsing again from the first open block keeps the work small. If an answer has $n$ blocks and each piece reopens only
the last one, a whole stream costs about $n$ block parses rather than $n^2$. The exception is a long list or table
that stays open: each piece parses it again, which is still cheap next to laying it out.

The inline parser's worst cases are bounded too. Runs of unmatched `*`, `_` and `[` are tracked so that a
paragraph of them parses in linear time, as the reference implementation's pathological tests check.

## Code

A host feeds pieces to a document, or to an element of a page:

```cpp
PrHtmlMarkdownStream* stream = prHtmlMarkdownStreamCreate(message, nullptr);
for (PrStringView piece : pieces)
    prHtmlMarkdownStreamAppend(stream, piece);
prHtmlMarkdownStreamEnd(stream);
prHtmlMarkdownStreamDestroy(stream);
```

A client that keeps its own view of the blocks reads the changes instead:

```cpp
PrMarkdownChange changes[8];
uint32_t count = 0;
while ((count = prMarkdownTakeChanges(document, changes, 8)) != 0)
    applyChanges(view, changes, count);
```

## Blocks that change their kind

Some blocks only learn what they are from the line after them. This one arrives as a paragraph and turns into a
heading once its underline does
-----------------------------------

A list whose items are separated by blank lines is loose, and that is known only once the second item arrives:

- the first item, which reads as tight until the blank line below it

- the second item, which makes both of them paragraphs

- the third, which changes nothing more

Display math works the same way: until the closing `$$` arrives, the dollar signs read as text.

$$
\int_0^1 x^2 \, dx = \frac{1}{3}
$$

## What stays tentative

> A quote, with `code`, ~~a mistake~~, and a [late link][guide] whose definition is one of the last lines. Until that
> line arrives, the brackets read as text.

- [x] blocks before the open one stay as they are
- [x] a footnote is numbered once its first reference arrives[^numbers]
- [ ] a reference defined later rebuilds only the blocks that use it
- [ ] the front matter of a document, if it has any, holds back every block until its closing fence

> [!TIP]
> Keep the page following the end while an answer streams, and stop following as soon as the reader scrolls up.

That is all there is to it: parse what arrived, keep what is final, and build the rest again.

[^final]: A block is final once a line after it has started another block, so the last block is always tentative
    until the source ends.
[^numbers]: Footnotes are numbered in the order of their first references, as GitHub numbers them.

[spec]: https://spec.commonmark.org/0.31.2/
[guide]: https://github.github.com/gfm/
)md";

// The messages of the chat: a question, each followed by an answer that streams. The answers parse with GitHub's
// footnotes, which an element's stream takes as an option, where a text/markdown document has the default options.
struct Message {
    bool fromUser;
    const char* markdown;
};

constexpr Message kChat[] = {
    {true, "How do I stream Markdown into a page?"},
    {
        false, R"md(Create a stream on the element that holds the message, then append each piece as it arrives:

```c
PrHtmlMarkdownStream* stream = prHtmlMarkdownStreamCreate(message, NULL);
prHtmlMarkdownStreamAppend(stream, piece);
prHtmlMarkdownStreamEnd(stream);
```

The element's children for the **final** blocks are kept, and only the rest is built again. Two things are worth
knowing:

- The stream retains the element, so the element outlives a page that drops it while the answer is still arriving.
- Ending the stream settles every block. Until then, the last block may still change its kind.)md",
    },
    {true, "What does a table cost while it streams?"},
    {
        false, R"md(Until its delimiter row arrives, the header is a paragraph:

| Lines so far | Parsed as |
| --- | --- |
| `a \| b` | a paragraph |
| `a \| b` and `--- \| ---` | a table |
| one more row | a table with a body |

Each row after that adds to the open table, which stays *tentative* until a line ends it.[^rows] So a long table is
parsed again with each piece, and its layout is redone too; neither is noticeable at a reading pace.

[^rows]: A footnote is numbered once its first reference arrives, and the list of footnotes follows the answer.)md",
    },
    {true, "Can an answer use math?"},
    {
        false, R"md(Yes. With GitHub's extensions, TeX between dollar signs is math: $e^{i\pi} + 1 = 0$ inline, and

$$\sum_{k=1}^{n} k = \frac{n(n+1)}{2}$$

on its own. Until a typesetter lays it out, the TeX shows as it is written, in a span that a style sheet can pick
out.

> [!WARNING]
> A dollar sign followed by a digit, as in $5, does not start math, so prices read as text.)md",
    },
    {true, "How do lists behave while they stream?"},
    {
        false, R"md(A list stays open for as long as its items keep coming, so the whole list is the tentative block:

1. Each new item is parsed with the items before it.
2. A blank line between two items makes the list *loose*, and every item becomes a paragraph.
3. A line that is not indented under an item ends the list, and from then on it is final.

Nested lists work the same way, one level inside another:

- fruit
  - apples
  - pears
- vegetables
  - [x] carrots
  - [ ] leeks

A list item can also hold a code block, a quote or a table; each is part of the item until the item ends.)md",
    },
    {true, "And when the reference to a link comes last?"},
    {
        false, R"md(The link reads as plain text until its definition arrives. Here is [the specification][cm] and
[GitHub's flavor][gfm] of it; both resolve only once the lines below them do.

1. The paragraph that uses a label is final long before the definition.
2. When the definition arrives, the document rebuilds just that paragraph.
3. Nothing else on the page moves.

[cm]: https://spec.commonmark.org/
[gfm]: https://github.github.com/gfm/)md",
    },
};

// A page that a slow network delivers.
constexpr const char* kPage = R"html(<!DOCTYPE html>
<html lang="en">
<title>Streaming HTML</title>
<style>
  body { max-width: 44em; margin: 2em auto; padding: 0 1.5em; font-family: sans-serif; line-height: 1.5; }
  h1 { color: #2f6fdf; }
  .card { padding: 0.8em 1em; margin: 1em 0; border-radius: 8px; background: #eef3fb; border: 1px solid #c9d6ef; }
  table { border-collapse: collapse; }
  td, th { border: 1px solid #c9d6ef; padding: 0.3em 0.8em; }
  figure { margin: 1.5em 0; }
  figcaption { font-size: 0.9em; color: #556; }
  code { background: #f3f4f7; padding: 0 0.2em; border-radius: 3px; }
</style>
<h1>Streaming HTML</h1>
<p>The HTML parser takes the page a slice at a time, in tasks of the event loop, so the page renders as it arrives.
HTML mostly appends each element as its tag arrives, but tree construction can move nodes that it already built, as
the adoption agency algorithm does for misnested formatting, and it holds a table's text until it knows where the text
goes.</p>
<div class="card"><strong>Render-blocking:</strong> while the parser is in the head and a style sheet loads, nothing is
painted. A style element in the body styles the elements that it follows and those after it.</div>

<h2>Elements as they arrive</h2>
<p>Each kind of token does one small thing to the tree, and the presentation catches up with whatever the tasks of a frame
added. A page that arrives slowly is therefore painted many times over, each time a little longer than the last.</p>
<ul>
  <li>A start tag opens an element at once.</li>
  <li>Text is appended to the last text node, which grows a piece at a time.</li>
  <li>An end tag closes the elements that it matches.</li>
  <li>A character reference such as <code>&amp;amp;</code> becomes the character it names: &amp;.</li>
</ul>
<table>
  <tr><th>Piece</th><th>Effect</th></tr>
  <tr><td>&lt;p&gt;</td><td>a new paragraph</td></tr>
  <tr><td>text</td><td>a longer text node</td></tr>
  <tr><td>&lt;li&gt;</td><td>a new item, which closes the open one</td></tr>
  <tr><td>&lt;/table&gt;</td><td>the table closes</td></tr>
</table>

<h2>Implied tags</h2>
<p>Many end tags may be left out, and the parser supplies them. A new paragraph closes the open one
<p>like this, and a list item closes the item before it:
<ol>
  <li>first, with no end tag
  <li>second, which closed the first
  <li>third, which the end of the list closes
</ol>
<p>The table that follows has no <code>tbody</code> in its source, yet its rows land in one, because the parser inserts
the body as soon as the first row arrives.</p>
<table>
  <tr><th>Tag</th><th>Closed by</th></tr>
  <tr><td>&lt;p&gt;</td><td>a block start tag, or the end of its parent</td></tr>
  <tr><td>&lt;li&gt;</td><td>the next item, or the end of its list</td></tr>
  <tr><td>&lt;td&gt;</td><td>the next cell or row</td></tr>
</table>

<h2>What tree construction moves</h2>
<p>A paragraph that starts inside bold text: <b>bold, then<p>a paragraph</b> whose bold the end tag closes, and the
adoption agency moves the paragraph out of the bold element it was built in.</p>
<p>Formatting that is closed out of order is mended the same way: <b>bold <i>bold italic</b> italic</i> plain.</p>
<table>
  <tr><td>A cell</td></tr>
  text in a table, held until the parser knows it goes before the table
</table>

<h2>Figures and quotes</h2>
<figure>
  <blockquote>The page is never finished until its source ends, but it is always readable.</blockquote>
  <figcaption>A figure whose caption arrives after its quote.</figcaption>
</figure>
<p>A long paragraph shows text growing within one line box after another. It wraps at the width of the pane, and each
piece that arrives may push a word onto the next line. The lines above it do not move, because nothing before the
open text node changes; only the paragraph's own height grows as the words arrive, and with it the page.</p>
<p>Inline elements nest in the paragraph as they arrive: <em>emphasis</em>, <strong>strong text</strong>,
<code>code</code>, <a href="#top">a link</a>, and <mark>a highlight</mark>, each a start tag, text and an end tag.</p>

<h2>Lists inside lists</h2>
<ul>
  <li>Streaming
    <ul>
      <li>pieces arrive in order</li>
      <li>each piece is parsed in a task</li>
    </ul>
  </li>
  <li>Rendering
    <ul>
      <li>styles apply to the elements that the tasks added</li>
      <li>layout places them, and the frame paints them</li>
    </ul>
  </li>
</ul>
<dl>
  <dt>Slice</dt>
  <dd>The part of the source that one task parses.</dd>
  <dt>Rendering opportunity</dt>
  <dd>A point between tasks at which the page may be painted.</dd>
  <dt>Render-blocked</dt>
  <dd>The state of a document whose head is not yet past, which paints nothing.</dd>
</dl>
<details open>
  <summary>Details that arrive open</summary>
  <p>A details element shows its content as it arrives, because its <code>open</code> attribute was in its start
  tag.</p>
</details>

<h2>Once the source ends</h2>
<p>The end of the source pops every element that is still open, fires the document's <code>DOMContentLoaded</code>
event, and the page's load event follows once nothing delays it.</p>
<p>The page is complete once its source ends.</p>
)html";

// The styles of the chat's page.
constexpr const char* kChatPage = R"(<!DOCTYPE html>
<style>
  :root { color-scheme: light dark; }
  body { margin: 0; padding: 1.5em; font-family: sans-serif; line-height: 1.5; background: Canvas; }
  .message { max-width: 40em; margin: 0 0 1em; padding: 0.2em 1.1em; border-radius: 12px; }
  .user { margin-left: auto; background: color-mix(in srgb, #2f6fdf 22%, Canvas); }
  .assistant { background: color-mix(in srgb, CanvasText 6%, Canvas); }
  pre { padding: 0.8em; border-radius: 6px; overflow: auto; background: color-mix(in srgb, CanvasText 10%, Canvas); }
  table { border-collapse: collapse; }
  td, th { border: 1px solid color-mix(in srgb, CanvasText 25%, Canvas); padding: 0.2em 0.6em; }
</style>
<body>
)";

// ---------------------------------------------------------------------------------------------------------------------
// The stream

// The rate and the size of the pieces, which the controls set, and their ranges.
constexpr double kMinRate = 20;
constexpr double kMaxRate = 20000;
constexpr double kMinChunk = 1;
constexpr double kMaxChunk = 256;

// The height of a line that a wheel scrolls by, in logical pixels.
constexpr float kLineHeight = 40;

// How long the page's tasks may run before each frame, in milliseconds.
constexpr double kTaskBudget = 8;

// A sample as it streams: the page that shows it, what has been delivered, and what the Markdown that runs beside it
// reported.
struct Stream {
    Sample sample = Sample_Markdown;
    float rate = 120;               // bytes a second, about a language model's pace
    float chunk = 6;                // bytes a piece
    bool paused = false;

    PrTextFont* font = nullptr;
    PrTextShaper* shaper = nullptr;
    PrHtmlPage* page = nullptr;
    PrHtmlView* view = nullptr;

    // The text that streams, and how much of it has been delivered. In the chat, the text is the messages in order, and
    // `messageEnds` is where each one ends.
    std::string text;
    size_t delivered = 0;
    double budget = 0;              // bytes that the rate has allowed and that are not yet delivered
    uint32_t pieces = 0;
    uint32_t message = 0;            // the chat's message that is streaming
    PrDomElement* element = nullptr; // the chat's message's element
    PrHtmlMarkdownStream* markdownStream = nullptr;

    // The Markdown of the same pieces, and what its last piece changed.
    PrMarkdown* mirror = nullptr;
    PrMarkdownChange lastChange{};
    uint32_t rebuilt = 0;           // the blocks that the pieces have built again in all

    // The pane in the last frame, in the window's logical pixels, and the scrolling of the page in it: whether it
    // follows the end of the page as it grows, and the distance that the wheel scrolled it since the last frame.
    PrRect pane{0, 0, 640, 480};
    bool follow = true;
    double scrollBy = 0;

    PrDrawList* recorded = nullptr; // the page as the last frame recorded it, frozen
    PrAppDisplay display = prAppDisplayDefault();
    PrAppUserPreferences preferences{};
};

// The page's clock: a monotonic clock in milliseconds.
double now(void* PR_NULLABLE) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

PrDomDocument* documentOf(const Stream& s) {
    return prHtmlWindowDocument(prHtmlPageWindow(s.page));
}

// Returns the options that the chat's answers parse with.
PrMarkdownOptions chatOptions() {
    PrMarkdownOptions options = prMarkdownOptionsDefault();
    options.extensions |= PrMarkdownExtensionFlag_Footnotes;
    return options;
}

// Starts the chat's next message: its element, under the body, and for an answer, a stream into it. A question shows
// whole.
void startMessage(Stream& s) {
    if (s.message >= countOf(kChat))
        return;
    const Message& message = kChat[s.message];
    PrDomDocument* document = documentOf(s);
    PrDomElement* element = nullptr;
    if (prDomElementCreate(document, "div", nullptr, &element) != PrStatus_Ok)
        return;
    prDomElementSetAttribute(element, "class", message.fromUser ? "message user" : "message assistant");
    PrDomElement* body = prDomDocumentBody(document);
    if (body != nullptr)
        prDomNodeAppendChild(prDomElementAsNode(body), prDomElementAsNode(element));
    s.element = element;
    PrMarkdownOptions options = chatOptions();
    if (!message.fromUser)
        s.markdownStream = prHtmlMarkdownStreamCreate(element, &options);
    if (s.mirror != nullptr)
        prMarkdownClear(s.mirror);
}

// Frees what the stream made, and makes the sample's page again from its start.
void closeStream(Stream& s) {
    prHtmlMarkdownStreamDestroy(s.markdownStream);
    s.markdownStream = nullptr;
    if (s.element != nullptr)
        prDomNodeRelease(prDomElementAsNode(s.element));
    s.element = nullptr;
    prHtmlViewDestroy(s.view);
    s.view = nullptr;
    prHtmlPageDestroy(s.page);
    s.page = nullptr;
    prMarkdownDestroy(s.mirror);
    s.mirror = nullptr;
    prDrawListRelease(s.recorded);
    s.recorded = nullptr;
}

void restart(Stream& s) {
    closeStream(s);
    PrHtmlHost host = prHtmlHostDefault();
    host.font = s.font;
    host.shaper = s.shaper;
    host.now = now;
    s.page = prHtmlPageCreate(&host);
    PrHtmlViewDesc desc{.page = s.page};
    s.view = prHtmlViewCreate(&desc);
    s.delivered = 0;
    s.budget = 0;
    s.pieces = 0;
    s.message = 0;
    s.rebuilt = 0;
    s.lastChange = PrMarkdownChange{};
    s.text.clear();
    s.follow = true;
    s.scrollBy = 0;
    PrMarkdownOptions options = s.sample == Sample_Chat ? chatOptions() : prMarkdownOptionsDefault();
    if (s.sample != Sample_Html)
        s.mirror = prMarkdownCreate(&options);
    switch (s.sample) {
        case Sample_Markdown: {
            PrHtmlResponseHeaders headers{.contentType = "text/markdown; charset=utf-8"};
            prHtmlPageBeginLoad(s.page, "about:answer.md", &headers);
            s.text = kAnswer;
            break;
        }
        case Sample_Chat: {
            prHtmlPageLoad(s.page, kChatPage, "about:chat", nullptr);
            for (const Message& message : kChat)
                s.text += message.markdown;
            startMessage(s);
            break;
        }
        case Sample_Html: {
            PrHtmlResponseHeaders headers{.contentType = "text/html; charset=utf-8"};
            prHtmlPageBeginLoad(s.page, "about:page.html", &headers);
            s.text = kPage;
            break;
        }
    }
}

// Returns where the chat's message `index` ends in the stream's text.
size_t messageEnd(uint32_t index) {
    size_t end = 0;
    for (uint32_t i = 0; i <= index && i < countOf(kChat); ++i)
        end += std::strlen(kChat[i].markdown);
    return end;
}

// Notes what the piece changed in the Markdown that runs beside the page.
void noteChanges(Stream& s) {
    PrMarkdownChange changes[8];
    uint32_t count = 0;
    while ((count = prMarkdownTakeChanges(s.mirror, changes, 8)) != 0) {
        for (uint32_t i = 0; i < count; ++i) {
            s.lastChange = changes[i];
            s.rebuilt += changes[i].insertedCount;
        }
    }
}

// Delivers one piece of at most `size` bytes.
void deliverPiece(Stream& s, size_t size) {
    size_t end = s.sample == Sample_Chat ? messageEnd(s.message) : s.text.size();
    size = std::min(size, end - s.delivered);
    PrStringView piece{s.text.data() + s.delivered, size};
    switch (s.sample) {
        case Sample_Markdown:
        case Sample_Html:
            prHtmlPageAppendLoad(s.page, piece);
            break;
        case Sample_Chat:
            if (s.markdownStream != nullptr)
                prHtmlMarkdownStreamAppend(s.markdownStream, piece);
            break;
    }
    if (s.mirror != nullptr) {
        prMarkdownAppend(s.mirror, piece);
        noteChanges(s);
    }
    s.delivered += size;
    ++s.pieces;
    if (s.delivered < end)
        return;
    // The source, or the chat's message, has ended.
    if (s.mirror != nullptr) {
        prMarkdownEnd(s.mirror);
        noteChanges(s);
    }
    if (s.sample != Sample_Chat) {
        prHtmlPageEndLoad(s.page);
        return;
    }
    if (s.markdownStream != nullptr) {
        prHtmlMarkdownStreamEnd(s.markdownStream);
    } else if (s.element != nullptr) {
        PrMarkdownOptions options = chatOptions();
        prHtmlSetMarkdown(s.element, PrStringView{
            s.text.data() + end - std::strlen(kChat[s.message].markdown),
            std::strlen(kChat[s.message].markdown),
        }, &options);
    }
    prHtmlMarkdownStreamDestroy(s.markdownStream);
    s.markdownStream = nullptr;
    prDomNodeRelease(prDomElementAsNode(s.element));
    s.element = nullptr;
    ++s.message;
    startMessage(s);
}

bool finished(const Stream& s) {
    return s.delivered >= s.text.size();
}

// Advances the stream by `delta` seconds: delivers the pieces that its rate allows, runs the page's due tasks, scrolls
// the page by the wheel, or to its end while it follows, and records the page for the pane.
void advance(Stream& s, double delta) {
    if (!s.paused && !finished(s)) {
        s.budget += double(s.rate) * delta;
        size_t chunk = size_t(std::max(1.0f, std::round(s.chunk)));
        while (!finished(s) && s.budget >= double(chunk)) {
            // A question in the chat shows whole.
            bool question = s.sample == Sample_Chat && kChat[s.message].fromUser;
            size_t size = question ? messageEnd(s.message) - s.delivered : chunk;
            deliverPiece(s, size);
            s.budget -= double(question ? chunk : size);
        }
    }
    PrDomAgent* agent = prHtmlPageAgent(s.page);
    double deadline = now(nullptr) + kTaskBudget;
    while (now(nullptr) < deadline && prDomAgentNextTaskDelay(agent) == 0 && prDomAgentRunTask(agent)) {
    }
    prHtmlViewSetDisplay(s.view, &s.display);
    prHtmlViewSetUserPreferences(s.view, &s.preferences);
    prHtmlViewSetViewport(s.view, PrVec2{s.pane.right - s.pane.left, s.pane.bottom - s.pane.top});
    PrHtmlWindow* window = prHtmlPageWindow(s.page);
    if (s.scrollBy != 0) {
        PrCssScrollToOptions by{.hasTop = true, .top = s.scrollBy};
        prHtmlWindowScrollBy(window, &by);
        s.scrollBy = 0;
    }
    if (s.follow) {
        // The offset is clamped to the end of the page.
        PrCssScrollToOptions end{.hasTop = true, .top = 1e9};
        prHtmlWindowScroll(window, &end);
    }
    PrDrawList* recorded = prDrawListCreate();
    prHtmlViewPaint(s.view, recorded);
    prDrawListFreeze(recorded);
    prDrawListRelease(s.recorded);
    s.recorded = recorded;
}

// ---------------------------------------------------------------------------------------------------------------------
// The controls

// Draws the page that the context, a frozen draw list, holds, clipped to the pane.
void paintPage(void* PR_NULLABLE context, PrVec2 extent, PrDrawList* list) {
    prDrawListPushClipRect(list, PrRect{0, 0, extent.x, extent.y}, PrDrawEdges_Aliased);
    prDrawListEmbed(list, static_cast<PrDrawList*>(context), PrDVec2{0, 0});
    prDrawListPopClip(list);
}

void retainPage(void* PR_NULLABLE context) {
    prDrawListRetain(static_cast<PrDrawList*>(context));
}

void releasePage(void* PR_NULLABLE context) {
    prDrawListRelease(static_cast<PrDrawList*>(context));
}

// Returns the status line: what has been delivered, and what the last piece changed.
std::string statusOf(const Stream& s) {
    char line[256];
    int written = std::snprintf(line, sizeof line, "%zu of %zu bytes in %u pieces%s", s.delivered, s.text.size(),
        s.pieces, s.paused ? ", paused" : finished(s) ? ", done" : "");
    std::string status(line, size_t(std::max(written, 0)));
    if (s.mirror != nullptr) {
        written = std::snprintf(line, sizeof line,
            ".  Markdown: %u blocks, %u final; the last change replaced %u blocks at %u with %u; "
                "%u blocks built in all", prMarkdownBlockCount(s.mirror), prMarkdownFinalCount(s.mirror),
            s.lastChange.removedCount, s.lastChange.first, s.lastChange.insertedCount, s.rebuilt);
        status.append(line, size_t(std::max(written, 0)));
    }
    return status;
}

// Declares the round: a bar of the stream's controls, the pane that shows the page, and the status line.
void buildApp(void* context, PrKit* kit, PrLayoutNode ground) {
    Stream& s = *static_cast<Stream*>(context);
    PrLayoutTree* tree = kit->tree;
    PrKitSurfaceStyle header{
        .box = {
            .direction = PrKitDirection_Row,
            .align = PrKitAlign_Center,
            .gap = 14,
            .padding = prKitEdgesXY(10, 14),
        },
        .ground = PrKitGround_Hud,
        .rules = PrKitRuleFlag_Bottom,
    };
    PrLayoutNode bar = prKitSurface(kit, ground, {}, header);
    PrKitTextStyle title{.role = PrKitTextRole_Heading};
    prKitLabel(kit, bar, {.text = "Incremental"}, title);

    PrLayoutNode samples = prKitSegmented(kit, bar, {}, {});
    for (Sample sample = 0; sample < Sample_Count; ++sample) {
        PrKitChoice choice{.label = kSampleLabels[sample], .selected = s.sample == sample};
        if (prKitActivated(kit, prKitSegment(kit, samples, choice)) && s.sample != sample) {
            s.sample = sample;
            restart(s);
        }
    }
    PrKitButtonStyle small{.size = PrKitControlSize_Sm};
    if (prKitActivated(kit, prKitButton(kit, bar, {.text = s.paused ? "Play" : "Pause"}, small)))
        s.paused = !s.paused;
    if (prKitActivated(kit, prKitButton(kit, bar, {.text = "Restart"}, small)))
        restart(s);
    prKitToggle(kit, bar, {.value = &s.follow, .label = "Follow"}, {});

    PrKitTextStyle meta{.role = PrKitTextRole_Meta, .tone = PrKitTone_Secondary};
    PrKitPlace slider{.width = prKitPx(160), .flex = PrKitFlex_Fixed};
    char label[64];
    int written = std::snprintf(label, sizeof label, "%.0f bytes/s", double(s.rate));
    prKitLabel(kit, bar, {.text = PrStringView{label, size_t(std::max(written, 0))}}, meta);
    PrLayoutNode rate = prKitSlider(kit, bar, {
        .value = prKitScalarF32(&s.rate),
        .min = kMinRate,
        .max = kMaxRate,
        .scale = PrKitSliderScale_Ratio,
    }, {});
    prKitPlace(kit, rate, &slider);
    written = std::snprintf(label, sizeof label, "%.0f bytes a piece", double(std::round(s.chunk)));
    prKitLabel(kit, bar, {.text = PrStringView{label, size_t(std::max(written, 0))}}, meta);
    PrLayoutNode chunk = prKitSlider(kit, bar, {
        .value = prKitScalarF32(&s.chunk),
        .min = kMinChunk,
        .max = kMaxChunk,
        .scale = PrKitSliderScale_Ratio,
    }, {});
    prKitPlace(kit, chunk, &slider);
    PrLayoutNode progress = prKitProgress(
        kit, bar, {.value = s.text.empty() ? 0.0f : float(s.delivered) / float(s.text.size())});
    PrKitPlace grow{.flex = PrKitFlex_Grow};
    prKitPlace(kit, progress, &grow);

    // The pane is a replaced element that fills the room under the bar, and its painter embeds the page.
    PrLayoutNode pane = prLayoutTreeFrameReplaced(tree, ground, 0, 0);
    PrKitPlace fill{.flex = PrKitFlex_Fill};
    prKitPlace(kit, pane, &fill);
    if (s.recorded != nullptr) {
        PrLayoutContent content{.kind = PrLayoutContentKind_Painter};
        content.painter = PrLayoutPainter{paintPage, s.recorded, retainPage, releasePage};
        prLayoutTreeFrameSetContent(tree, pane, &content);
    }
    PrRect bounds = prLayoutRectToPx(prLayoutNodeBounds(tree, pane, PrLayoutCoordinates_Viewport));
    if (bounds.right - bounds.left >= 1 && bounds.bottom - bounds.top >= 1)
        s.pane = bounds;

    PrKitSurfaceStyle footer{
        .box = {.padding = prKitEdgesXY(6, 14)},
        .ground = PrKitGround_Hud,
        .rules = PrKitRuleFlag_Top,
    };
    PrLayoutNode status = prKitSurface(kit, ground, {}, footer);
    std::string line = statusOf(s);
    prKitLabel(kit, status, {.text = line}, meta);
}

// ---------------------------------------------------------------------------------------------------------------------
// The window

// The example in a window: the stream, its UI, and the device that it renders on.
struct Window {
    Stream stream;
    PrVec2 pointer{};               // where the pointer last moved, in logical pixels

    PrFontSet* fonts = nullptr;
    PrTextShaper* shaper = nullptr;
    PrLayoutTree* tree = nullptr;
    PrUi* ui = nullptr;
    PrKit* kit = nullptr;
    const PrLayoutScene* scene = nullptr;   // the scene of the last frame, which its list plays
    PrDrawList* list = nullptr;
    std::vector<PrLayoutSceneScroll> scrolls;

    PrApp* app = nullptr;
    PrGpuDevice* device = nullptr;
    PrGpuSwapChain* swapChain = nullptr;
    PrDrawRenderer* renderer = nullptr;
    PrDrawFrame* frame = nullptr;
};

// Declares a round: a surface on the app's ground that fills the viewport, and the example under it.
void build(void* context, PrUi*, const PrUiRound*) {
    Window& w = *static_cast<Window*>(context);
    PrKitSurfaceStyle style{
        .box = {.overflow = PrKitOverflow_Clip, .place = {.width = prKitPercent(100), .height = prKitPercent(100)}},
        .ground = PrKitGround_App,
    };
    buildApp(&w.stream, w.kit, prKitSurface(w.kit, kPrLayoutNodeNone, {}, style));
}

// Frees what the window made on its device, and the device.
void closeDevice(Window& w) {
    if (w.device != nullptr)
        prGpuDeviceWaitIdle(w.device);
    prDrawFrameDestroy(w.frame);
    prDrawRendererDestroy(w.renderer);
    prGpuSwapChainDestroy(w.swapChain);
    prGpuDeviceDestroy(w.device);
    w.frame = nullptr;
    w.renderer = nullptr;
    w.swapChain = nullptr;
    w.device = nullptr;
}

// Makes the platform's device, a swap chain over the window for SDR, and the renderer. Returns false if the window has
// no device to render with.
bool openDevice(Window& w) {
    PrAppSurface surface{};
    PrGpuSwapChainDesc desc{.intent = PrGpuSwapChainIntent_Sdr};
    if (prGpuDeviceCreate(PrGpuBackend_Default, nullptr, &w.device) != PrStatus_Ok ||
            prAppSurface(w.app, &surface) != PrStatus_Ok ||
            prGpuSwapChainCreate(w.device, &surface, &desc, &w.swapChain) != PrStatus_Ok ||
            prDrawRendererCreate(PrDrawRendererBackend_Default, w.device, nullptr, &w.renderer) != PrStatus_Ok) {
        closeDevice(w);
        return false;
    }
    w.frame = prDrawFrameCreate(w.renderer);
    return true;
}

// Renders the list into the swap chain's texture at `extent` target pixels, clearing it to the canvas, and presents it.
void render(Window& w, PrUVec2 extent, PrColor canvas) {
    PrGpuTexture drawable{};
    PrGpuCommands* commands = nullptr;
    if (prGpuSwapChainAcquire(w.swapChain, extent, &drawable) != PrStatus_Ok ||
            !prGpuCommandsBegin(w.device, nullptr, &commands))
        return;
    PrDrawTarget into{.extent = extent, .format = PrPixelFormat_Bgra8, .commands = commands};
    prDrawFramePrepare(w.frame, w.list, &into);
    PrGpuSubresources all{};
    prGpuCommandsTextureBarrier(commands, drawable, all, PrGpuAccessFlag_Discard, PrGpuAccessFlag_ColorAttachment);
    PrGpuPassDesc pass = prGpuPassDescColor(drawable, PrGpuLoad_Clear, canvas);
    PrGpuPass* encoder = prGpuPassBegin(commands, &pass);
    prDrawFrameRender(w.frame, encoder, PrIVec2{0, 0});
    prGpuPassEnd(encoder);
    prGpuCommandsTextureBarrier(commands, drawable, all, PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_Present);
    prGpuSwapChainPresent(w.swapChain, commands);
    prGpuCommandsSubmit(commands);
}

void start(void* context, PrApp* app, const PrAppFrame*) {
    Window& w = *static_cast<Window*>(context);
    w.app = app;
    if (!openDevice(w)) {
        std::fprintf(stderr, "incremental: no GPU device to render with\n");
        prAppStop(app);
        return;
    }
    prAppRequestFrame(app);
}

PrAppOutput frame(void* context, PrApp*, const PrAppFrame* f) {
    Window& w = *static_cast<Window*>(context);
    // A lost device is made again, with everything on it.
    if (w.device != nullptr && prGpuDeviceIsLost(w.device))
        closeDevice(w);
    if (w.device == nullptr && !openDevice(w))
        return {};
    prGpuSwapChainWait(w.swapChain);
    w.stream.display = f->display;
    w.stream.preferences = f->preferences;
    advance(w.stream, f->delta);

    PrUiFrameDesc desc{
        .time = f->time,
        .delta = f->delta,
        .extent = f->extent,
        .maxRounds = 4,
        .display = f->display,
        .preferences = f->preferences,
    };
    prUiFrame(w.ui, &desc, build, &w);

    // The tree's scene plays into the list in target pixels, each scroll container at the tree's offset for it. The
    // pass clears to the canvas's color, so the list leaves the canvas unfilled.
    const PrLayoutScene* scene = prLayoutTreeScene(w.tree);
    prLayoutSceneRelease(w.scene);
    w.scene = scene;
    w.scrolls.resize(prLayoutSceneScrolls(w.tree, scene, nullptr, 0));
    prLayoutSceneScrolls(w.tree, scene, w.scrolls.data(), uint32_t(w.scrolls.size()));
    PrLayoutScenePlayOptions options = prLayoutScenePlayOptionsDefault();
    options.scrolls = w.scrolls.data();
    options.scrollCount = uint32_t(w.scrolls.size());
    options.canvasFilled = true;
    float ratio = f->display.pixelRatio;
    prDrawListClear(w.list);
    prDrawListPushTransformAffine2(w.list, prAffine2Scale(PrVec2{ratio, ratio}));
    prLayoutScenePlay(scene, w.list, &options);
    prDrawListPopTransform(w.list);
    PrColor canvas = prColorFromColor16(prLayoutSceneCanvasColor(scene));

    PrUVec2 extent{uint32_t(f->extent.x * ratio + 0.5f), uint32_t(f->extent.y * ratio + 0.5f)};
    if (extent.x != 0 && extent.y != 0)
        render(w, extent, PrColor{canvas.r * canvas.a, canvas.g * canvas.a, canvas.b * canvas.a, canvas.a});
    // A stream that is delivering, or a page that has tasks, needs the next frame.
    PrAppOutput output = prUiOutput(w.ui);
    bool streaming = !w.stream.paused && !finished(w.stream);
    if (streaming || prDomAgentNextTaskDelay(prHtmlPageAgent(w.stream.page)) >= 0)
        output.wantsFrame = true;
    return output;
}

void event(void* context, PrApp* app, const PrAppEvent* e) {
    Window& w = *static_cast<Window*>(context);
    if (e->kind == PrAppEventKind_KeyDown && e->key == PrAppKey_Escape) {
        prAppStop(app);
        return;
    }
    if (e->kind == PrAppEventKind_PointerMove)
        w.pointer = e->position;
    // The wheel over the pane scrolls the page, and scrolling up stops it following the end.
    const PrRect& pane = w.stream.pane;
    bool overPane =
        w.pointer.x >= pane.left && w.pointer.x < pane.right && w.pointer.y >= pane.top && w.pointer.y < pane.bottom;
    if (e->kind == PrAppEventKind_Scroll && overPane) {
        float delta = prAppEventScrollPixels(e, kLineHeight).y;
        w.stream.scrollBy += double(delta);
        if (delta < 0)
            w.stream.follow = false;
        prAppRequestFrame(app);
        return;
    }
    prUiPushEvent(w.ui, e);
    prAppRequestFrame(app);
}

void shutdown(void* context, PrApp*) {
    closeDevice(*static_cast<Window*>(context));
}

} // namespace

int main() {
    prCrashHandler();
    static Window w;
    w.fonts = prFontSetCreate(nullptr);
    if (prFontBundleInstall(w.fonts, PrFontBundle_Noto, PR_FONT_BUNDLE_DIR) == 0) {
        std::fprintf(stderr, "incremental: the fonts under %s could not be installed\n", PR_FONT_BUNDLE_DIR);
        return 1;
    }
    w.shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    PrLayoutTreeDesc treeDesc{.shaper = w.shaper};
    w.tree = prLayoutTreeCreate(&treeDesc);
    w.ui = prUiCreate(w.tree);
    PrKitTheme theme = prKitThemeStudio(PrKitThemeVariant_Dark, w.fonts);
    w.kit = prKitCreate(w.ui, &theme);
    w.list = prDrawListCreate();

    // The stream's page renders its text in the same fonts and shaper as the UI.
    PrTextFontDesc font{.set = w.fonts, .style = prFontStyleDefault()};
    w.stream.font = prTextFontCreate(&font);
    w.stream.shaper = w.shaper;
    restart(w.stream);

    PrAppDesc desc{.title = "Prax Incremental", .extent = {1280, 860}};
    PrApp* app = prAppCreate(PrAppBackend_Default, &desc);
    PrAppHandler handler{&w, start, frame, event, shutdown};
    prAppRun(app, &handler);
    prAppDestroy(app);

    closeStream(w.stream);
    prTextFontRelease(w.stream.font);
    prDrawListRelease(w.list);
    prLayoutSceneRelease(w.scene);
    prKitDestroy(w.kit);
    prUiDestroy(w.ui);
    prLayoutTreeDestroy(w.tree);
    prTextShaperDestroy(w.shaper);
    prFontSetDestroy(w.fonts);
    return 0;
}
