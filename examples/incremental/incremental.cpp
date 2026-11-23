// The incremental example: a document that arrives in pieces, as a language model's answer or a slow network delivers
// it, rendered as it arrives, beside Studio Kit's controls for the stream.
//
// Eight samples stream in their own way. The Markdown answer is a text/markdown document, loaded as it arrives
// (prHtmlPageBeginLoad): its final blocks keep their nodes, and the blocks after them are built again from each piece.
// The chat is an HTML page whose messages are Markdown, each streamed into its own element (pr/html_markdown.h) as the
// answer to a question arrives. The HTML page is a text/html document, loaded as it arrives. A Markdown document of the
// same pieces runs beside a Markdown sample and reports what each piece changed (prMarkdownTakeChanges), which the
// status line shows. The five images are fed to a blob feed (PrBlobFeed) and opened as their bytes arrive
// (prTextureFormatSetOpen), as the texture cache shows each revision of the source: the baseline JPEG and the PNG
// arrive row by row, the progressive JPEG shows the whole picture sharpening scan by scan, the interlaced PNG shows it
// in blocks that refine pass by pass, and the animated GIF plays the frames that have arrived. The pictures are a
// photograph, a render and an animation of some tens of kilobytes, which `./dev.sh fetch incremental` fetches, and the
// PNGs are encoded from the photograph. A build without them streams small stand-ins of the repository. Choosing an
// image sets the rate at which it arrives in a few seconds.
//
// The arguments `--sample <markdown|chat|html|baseline-jpeg|progressive-jpeg|png|interlaced-png|gif>` and `--rate
// <bytes a second>` choose the sample and the rate that the example starts with, and `--size <width>x<height>` the
// window's size. `--capture <prefix>` writes a frame of the window to <prefix>_<n>.png every `--every` seconds, one
// second by default, while the sample streams, and once more after it ends, and then quits.
//
// The page runs on the window's thread. Before each frame, the stream delivers the pieces that its rate allows, the
// page runs its due tasks, and the view records the page into a draw list (prHtmlViewPaint) for the pane that the last
// frame laid out. The pane is a replaced element whose painter embeds that list.

#include "pr/app.h"
#include "pr/array.h"
#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/blob.h"
#include "pr/compress_select.h"
#include "pr/count_of.h"
#include "pr/css_view.h"
#include "pr/dom.h"
#include "pr/draw_list.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/float_convert.h"
#include "pr/font_bundle.h"
#include "pr/hash.h"
#include "pr/html.h"
#include "pr/html_markdown.h"
#include "pr/html_page.h"
#include "pr/html_view.h"
#include "pr/image_select.h"
#include "pr/image_texture_source.h"
#include "pr/kit_box.h"
#include "pr/kit_button.h"
#include "pr/kit_choice.h"
#include "pr/kit_image.h"
#include "pr/kit_input.h"
#include "pr/kit_panel.h"
#include "pr/kit_text.h"
#include "pr/layout_frame.h"
#include "pr/layout_unit.h"
#include "pr/markdown.h"
#include "pr/min_max.h"
#include "pr/png.h"
#include "pr/print.h"
#include "pr/resource.h"
#include "pr/string.h"
#include "pr/studio_app.h"
#include "pr/text.h"
#include "pr/text_font.h"
#include "pr/texture.h"
#include "pr/time.h"

namespace {

using namespace pr;

// ---------------------------------------------------------------------------------------------------------------------
// The samples

enum {
    Sample_Markdown,
    Sample_Chat,
    Sample_Html,
    Sample_BaselineJpeg,
    Sample_ProgressiveJpeg,
    Sample_Png,
    Sample_InterlacedPng,
    Sample_Gif,
    Sample_Count,
};
typedef uint32_t Sample;

constexpr const char* kSampleLabels[Sample_Count] = {
    "Markdown answer", "Markdown chat", "HTML page", "Baseline JPEG", "Progressive JPEG", "PNG", "Interlaced PNG",
    "Animated GIF",
};

// The names of the samples as the arguments give them, and the names of the image samples' files, whose extensions help
// to classify them.
constexpr const char* kSampleNames[Sample_Count] = {
    "markdown", "chat", "html", "baseline-jpeg", "progressive-jpeg", "png", "interlaced-png", "gif",
};
constexpr const char* kImageNames[Sample_Count] = {
    "", "", "", "baseline.jpg", "progressive.jpg", "clouds.png", "clouds-interlaced.png", "wave.gif",
};

// Returns true if the sample is an image, which is fed to a blob feed rather than to the page.
bool isImage(Sample sample) {
    return sample >= Sample_BaselineJpeg;
}

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
constexpr double kMaxRate = 100000;

// The rate that choosing a document sample sets, in bytes a second: about a language model's pace. The three documents
// are of similar lengths, so one rate suits them.
constexpr float kDocumentRate = 120;

// The seconds that an image sample takes to arrive at the rate that choosing it sets.
constexpr double kImageSeconds = 8;
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
    float rate = kDocumentRate;     // bytes a second
    float chunk = 6;                // bytes a piece
    bool paused = false;

    PrTextFont* font = nullptr;
    PrTextShaper* shaper = nullptr;
    PrHtmlPage* page = nullptr;
    PrHtmlView* view = nullptr;

    // The text that streams, and how much of it has been delivered. In the chat, the text is the messages in order, and
    // `messageEnds` is where each one ends.
    String text;
    size_t delivered = 0;
    double budget = 0;              // bytes that the rate has allowed and that are not yet delivered
    uint32_t pieces = 0;
    uint32_t message = 0;            // the chat's message that is streaming
    PrDomElement* element = nullptr; // the chat's message's element
    PrHtmlMarkdownStream* markdownStream = nullptr;

    // An image sample: the formats that open it, the bytes of each image, the feed that the pieces go to, and the
    // source once the bytes describe it, or why it did not open.
    const PrTextureFormatSet* formats = nullptr;
    String images[Sample_Count];
    PrBlobFeed* feed = nullptr;
    const PrTextureSource* image = nullptr;
    PrStatus imageStatus = PrStatus_Ok;

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
    PrAppUserPreferences preferences = prAppUserPreferencesDefault();
};

// Returns the rate at which `bytes` arrive in kImageSeconds.
float imageRate(const String& bytes) {
    return float(min(max(double(bytes.count()) / kImageSeconds, kMinRate), kMaxRate));
}

// Returns the rate at which `sample` starts when it is chosen.
float rateOf(const Stream& s, Sample sample) {
    return isImage(sample) ? imageRate(s.images[sample]) : kDocumentRate;
}

// Returns the bytes of the program's resource `name`.
String resource(PrStringView name) {
    String bytes;
    PrBlob* blob = prResourceOpen(name);
    if (blob != nullptr)
        bytes.append(prStringFromBytes(prBlobBytes(blob)));
    prBlobRelease(blob);
    return bytes;
}

// Returns the bytes of the resource `name`, or of `fallback` if the program was built without it, as before `./dev.sh
// fetch incremental` fetches the larger pictures.
String resourceOr(PrStringView name, PrStringView fallback) {
    String bytes = resource(name);
    return bytes.count() != 0 ? bytes : resource(fallback);
}

// Returns a PNG of rings of color over a gradient, interlaced if `interlaced` is set, whose bytes compress little, so
// that it streams for a while at the slow rates.
String ringsPng(const PrCompressCodecSet* compress, bool interlaced) {
    constexpr uint32_t kWidth = 480;
    constexpr uint32_t kHeight = 320;
    String pixels;
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            float dx = float(x) - kWidth * 0.5f;
            float dy = float(y) - kHeight * 0.5f;
            uint32_t ring = uint32_t(prF32Sqrt(dx * dx + dy * dy) / 12);
            // A little noise in each pixel keeps the encoding large enough to watch.
            uint32_t noise = (x * 2654435761u ^ y * 40503u) >> 27;
            char texel[4] = {
                char(uint8_t(x * 255 / kWidth + noise)), char(uint8_t(ring % 2 == 0 ? 220 : 60 + noise)),
                char(uint8_t(y * 255 / kHeight)), char(255),
            };
            pixels.append(PrStringView{texel, 4});
        }
    }
    PrPixelView view{reinterpret_cast<const uint8_t*>(pixels.data()), kWidth * 4, {kWidth, kHeight}};
    const PrPngEncodeFlags flags = interlaced ? PrPngEncodeFlag_Interlaced : 0;
    Array<uint8_t> encoded(uint32_t(prPngEncode(view, flags, compress, nullptr, 0)));
    prPngEncode(view, flags, compress, encoded.data(), encoded.count());
    String file;
    file.append(prStringFromBytes(PrByteView{encoded.data(), encoded.count()}));
    return file;
}

// Returns a PNG of the photograph in `jpeg`, reduced to `width` pixels across, interlaced if `interlaced` is set, or
// the rings if the photograph does not decode. A photograph's pixels compress little, so the PNG is large enough to
// watch.
String photoPng(const PrCompressCodecSet* compress, const PrImageCodecSet* images, const String& jpeg, bool interlaced) {
    constexpr uint32_t kWidth = 400;
    PrImageDecoder* decoder = prImageDecoderCreate(images);
    PrImageSource source{};
    source.memory = PrByteView{reinterpret_cast<const uint8_t*>(jpeg.data()), jpeg.count()};
    Array<uint8_t> full;
    PrUVec2 extent{};
    if (jpeg.count() != 0 && prImageDecoderOpen(decoder, source, nullptr) == PrStatus_Ok) {
        extent = prImageDecoderInfo(decoder)->extent;
        full.resize(extent.x * extent.y * 4);
        PrPixels into{full.data(), size_t(extent.x) * 4, extent};
        if (prImageDecoderReadRgba8(decoder, 0, PrImageAlpha_Straight, PrImageOrientation_Identity, into) !=
                PrStatus_Ok)
            extent = PrUVec2{};
    }
    prImageDecoderDestroy(decoder);
    if (extent.x < kWidth)
        return ringsPng(compress, interlaced);
    // Each pixel of the reduction is the mean of the block of the photograph's pixels that it covers.
    const uint32_t height = extent.y * kWidth / extent.x;
    Array<uint8_t> reduced(kWidth * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            const uint32_t x0 = x * extent.x / kWidth;
            const uint32_t x1 = max((x + 1) * extent.x / kWidth, x0 + 1);
            const uint32_t y0 = y * extent.y / height;
            const uint32_t y1 = max((y + 1) * extent.y / height, y0 + 1);
            uint32_t sums[4] = {};
            for (uint32_t sy = y0; sy < y1; ++sy) {
                for (uint32_t sx = x0; sx < x1; ++sx) {
                    for (uint32_t c = 0; c < 4; ++c)
                        sums[c] += full[(sy * extent.x + sx) * 4 + c];
                }
            }
            const uint32_t count = (x1 - x0) * (y1 - y0);
            for (uint32_t c = 0; c < 4; ++c)
                reduced[(y * kWidth + x) * 4 + c] = uint8_t(sums[c] / count);
        }
    }
    PrPixelView view{reduced.data(), kWidth * 4, {kWidth, height}};
    const PrPngEncodeFlags flags = interlaced ? PrPngEncodeFlag_Interlaced : 0;
    Array<uint8_t> encoded(uint32_t(prPngEncode(view, flags, compress, nullptr, 0)));
    prPngEncode(view, flags, compress, encoded.data(), encoded.count());
    String file;
    file.append(prStringFromBytes(PrByteView{encoded.data(), encoded.count()}));
    return file;
}

// The page's clock: a monotonic clock in milliseconds.
double now(void* PR_NULLABLE) {
    return prTimeMonotonicMilliseconds();
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
    // A feed that has not ended fails, which completes an open that waits for its header.
    prBlobFeedDestroy(s.feed);
    s.feed = nullptr;
    prTextureSourceRelease(s.image);
    s.image = nullptr;
    s.imageStatus = PrStatus_Ok;
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
    if (s.sample != Sample_Html && !isImage(s.sample))
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
                s.text.append(message.markdown);
            startMessage(s);
            break;
        }
        case Sample_Html: {
            PrHtmlResponseHeaders headers{.contentType = "text/html; charset=utf-8"};
            prHtmlPageBeginLoad(s.page, "about:page.html", &headers);
            s.text = kPage;
            break;
        }
        default: {
            s.text = s.images[s.sample];
            s.feed = prBlobFeedCreate(s.text.count());
            // The open completes when a piece describes the image, on this thread, inside the append.
            prTextureFormatSetOpen(s.formats, prBlobFeedSource(s.feed), kImageNames[s.sample], nullptr,
                [](void* PR_NULLABLE context, PrStatus status, const PrTextureSource* PR_NULLABLE source) {
                    Stream& opened = *static_cast<Stream*>(context);
                    opened.image = source;
                    opened.imageStatus = status;
                }, &s);
            break;
        }
    }
}

// Returns where the chat's message `index` ends in the stream's text.
size_t messageEnd(uint32_t index) {
    size_t end = 0;
    for (uint32_t i = 0; i <= index && i < countOf(kChat); ++i)
        end += PrStringView(kChat[i].markdown).size;
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
    size_t end = s.sample == Sample_Chat ? messageEnd(s.message) : s.text.count();
    size = min(size, end - s.delivered);
    PrStringView piece{s.text.data() + s.delivered, size};
    switch (s.sample) {
        case Sample_Markdown:
        case Sample_Html:
            prHtmlPageAppendLoad(s.page, piece);
            break;
        default:
            prBlobFeedAppend(s.feed, prStringBytes(piece));
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
    if (isImage(s.sample)) {
        prBlobFeedEnd(s.feed);
        return;
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
            s.text.data() + end - PrStringView(kChat[s.message].markdown).size,
            PrStringView(kChat[s.message].markdown).size,
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
    return s.delivered >= s.text.count();
}

// Advances the stream by `delta` seconds: delivers the pieces that its rate allows, runs the page's due tasks, scrolls
// the page by the wheel, or to its end while it follows, and records the page for the pane.
void advance(Stream& s, double delta) {
    if (!s.paused && !finished(s)) {
        s.budget += double(s.rate) * delta;
        size_t chunk = size_t(max(1.0f, prF32Round(s.chunk)));
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
String statusOf(const Stream& s) {
    PrStringView state = "";
    if (s.paused)
        state = ", paused";
    else if (finished(s))
        state = ", done";
    String status = format("{} of {} bytes in {} pieces{}", s.delivered, s.text.count(), s.pieces, state);
    if (isImage(s.sample) && s.image == nullptr)
        formatTo(status, s.imageStatus == PrStatus_Ok ? ".  Waiting for the header" : ".  It did not open: {}",
            prStatusName(s.imageStatus));
    if (s.image != nullptr) {
        PrTextureShow show{};
        prTextureSourceShow(s.image, 0, &show);
        PrUVec2 extent = prTextureSourceDesc(s.image)->extent;
        formatTo(status, ".  {}x{}, {}", extent.x, extent.y, show.complete ? "complete" : "a revision of the bytes so far");
        prTextureSourceRelease(show.picture);
    }
    if (s.mirror != nullptr) {
        formatTo(status, ".  Markdown: {} blocks, {} final; the last change replaced {} blocks at {} with {}; {} blocks "
            "built in all", prMarkdownBlockCount(s.mirror), prMarkdownFinalCount(s.mirror), s.lastChange.removedCount,
            s.lastChange.first, s.lastChange.insertedCount, s.rebuilt);
    }
    return status;
}

// Adds a group of the samples from `first` to before `end`, after a label, to `row`. Choosing another sample restarts
// the stream with it.
void sampleGroup(PrKit* kit, PrLayoutNode row, Stream& s, PrStringView label, Sample first, Sample end) {
    PrKitBoxStyle group{.direction = PrKitDirection_Row, .align = PrKitAlign_Center, .gap = 8};
    PrLayoutNode box = prKitBox(kit, row, {}, group);
    PrKitTextStyle meta{.role = PrKitTextRole_Meta, .tone = PrKitTone_Secondary, .wrap = PrKitWrap_Nowrap};
    prKitLabel(kit, box, {.text = label}, meta);
    PrLayoutNode samples = prKitSegmented(kit, box, {}, {});
    for (Sample sample = first; sample < end; ++sample) {
        PrKitChoice choice{.label = kSampleLabels[sample], .selected = s.sample == sample};
        if (prKitActivated(kit, prKitSegment(kit, samples, choice)) && s.sample != sample) {
            s.sample = sample;
            // Each sample starts at a rate of its own: a document at a reading pace, and an image over a few seconds.
            // The slider changes it until the next choice.
            s.rate = rateOf(s, sample);
            restart(s);
        }
    }
}

// Declares the round: a bar of the stream's controls over a row of the samples, the pane that shows the page or the
// image, and the status line. Each row wraps where the window is too narrow for it.
void buildApp(void* context, PrKit* kit, PrLayoutNode root) {
    Stream& s = *static_cast<Stream*>(context);
    PrLayoutTree* tree = kit->tree;
    PrKitPanelStyle header{
        .box = {.direction = PrKitDirection_Column, .gap = 10, .padding = prKitEdgesXY(10, 14)},
        .background = PrKitBackground_Hud,
        .rules = PrKitRuleFlag_Bottom,
    };
    PrLayoutNode panel = prKitPanel(kit, root, {}, header);
    PrKitBoxStyle row{.direction = PrKitDirection_Row, .align = PrKitAlign_Center, .wrap = true, .gap = 14};
    PrLayoutNode bar = prKitBox(kit, panel, {}, row);
    PrLayoutNode samples = prKitBox(kit, panel, {}, row);
    PrKitTextStyle title{.role = PrKitTextRole_Heading, .wrap = PrKitWrap_Nowrap};
    PrLayoutNode heading = prKitLabel(kit, bar, {.text = "Incremental"}, title);
    prKitPlace(kit, heading, {.flex = PrKitFlex_Fixed});
    sampleGroup(kit, samples, s, "Documents", Sample_Markdown, Sample_BaselineJpeg);
    sampleGroup(kit, samples, s, "Images", Sample_BaselineJpeg, Sample_Count);
    PrKitButtonStyle small{.size = PrKitControlSize_Sm};
    if (prKitActivated(kit, prKitButton(kit, bar, {.text = s.paused ? "Play" : "Pause"}, small)))
        s.paused = !s.paused;
    if (prKitActivated(kit, prKitButton(kit, bar, {.text = "Restart"}, small)))
        restart(s);
    prKitToggle(kit, bar, {.value = &s.follow, .label = "Follow"}, {});

    PrKitTextStyle meta{.role = PrKitTextRole_Meta, .tone = PrKitTone_Secondary};
    PrKitPlace slider{.width = prKitPx(160), .flex = PrKitFlex_Fixed};
    char label[64];
    prKitLabel(kit, bar, {.text = formatToBuffer(Span(label), "{:.0f} bytes/s", s.rate)}, meta);
    PrLayoutNode rate = prKitSlider(kit, bar, {
        .value = prKitScalarF32(&s.rate),
        .min = kMinRate,
        .max = kMaxRate,
        .scale = PrKitSliderScale_Ratio,
    }, {});
    prKitPlace(kit, rate, slider);
    prKitLabel(kit, bar, {.text = formatToBuffer(Span(label), "{:.0f} bytes a piece", prF32Round(s.chunk))}, meta);
    PrLayoutNode chunk = prKitSlider(kit, bar, {
        .value = prKitScalarF32(&s.chunk),
        .min = kMinChunk,
        .max = kMaxChunk,
        .scale = PrKitSliderScale_Ratio,
    }, {});
    prKitPlace(kit, chunk, slider);
    PrLayoutNode progress = prKitProgress(
        kit, bar, {.value = s.text.isEmpty() ? 0.0f : float(s.delivered) / float(s.text.count())});
    prKitPlace(kit, progress, {.minWidth = prKitPx(120), .flex = PrKitFlex_Grow});

    // The pane is a replaced element that fills the room under the bar, and its painter embeds the page. An image
    // sample shows the image there instead, in an image widget of its own key, as large as fits, once its header has
    // arrived. A replaced element keeps its content from one round to the next until a round sets it, so the pane sets
    // it every round, to nothing while it has nothing to show: a sample that starts again shows nothing of the last.
    PrLayoutNode pane{};
    if (isImage(s.sample) && s.image != nullptr) {
        pane = prKitImage(kit, root, {.key = prHashString("image", 0), .image = s.image}, {
            .fit = PrKitImageFit_Contain,
            .place = {.flex = PrKitFlex_Fill},
        });
    } else {
        pane = prLayoutTreeFrameReplaced(tree, root, 0, 0);
        prKitPlace(kit, pane, {.flex = PrKitFlex_Fill});
        PrLayoutContent content{};
        if (s.recorded != nullptr && !isImage(s.sample)) {
            content.kind = PrLayoutContentKind_Painter;
            content.painter = PrLayoutPainter{
                .context = s.recorded,
                .paint = paintPage,
                .retain = retainPage,
                .release = releasePage,
            };
        }
        prLayoutTreeFrameSetContent(tree, pane, &content);
    }
    PrRect bounds = prLayoutRectToPx(prLayoutNodeBounds(tree, pane, PrLayoutCoordinates_Viewport));
    if (bounds.right - bounds.left >= 1 && bounds.bottom - bounds.top >= 1)
        s.pane = bounds;

    PrKitPanelStyle footer{
        .box = {.padding = prKitEdgesXY(6, 14)},
        .background = PrKitBackground_Hud,
        .rules = PrKitRuleFlag_Top,
    };
    PrLayoutNode status = prKitPanel(kit, root, {}, footer);
    String line = statusOf(s);
    prKitLabel(kit, status, {.text = line}, meta);
}

// ---------------------------------------------------------------------------------------------------------------------
// The window

// The example in Studio's window: the stream, and where the pointer last moved, in logical pixels. A run that captures
// frames keeps the prefix of their files, the seconds between them, the codecs that encode them, and how far it is.
struct Example {
    Stream stream;
    PrVec2 pointer{};
    String capture;
    double every = 1;
    PrUVec2 size{1280, 860};            // of the window, in logical pixels
    bool rateGiven = false;             // the arguments chose the rate, which an image sample's then keeps
    const PrImageCodecSet* images = nullptr;
    double elapsed = 0;
    double nextCapture = 0;
    uint32_t captures = 0;
    bool last = false;                  // the capture that is asked for is the one after the stream ended
    Atomic<bool> capturing;
    Atomic<bool> done;
};

Example& exampleOf(void* context) {
    return *static_cast<Example*>(context);
}

// Opens the stream, whose page renders its text in the window's fonts and shaper, as the UI does.
bool open(void* context, PrStudioWindow* window) {
    Example& e = exampleOf(context);
    PrTextFontDesc font{.set = prStudioWindowFonts(window), .style = prFontStyleDefault()};
    e.stream.font = prTextFontCreate(&font);
    e.stream.shaper = prStudioWindowShaper(window);
    restart(e.stream);
    return true;
}

// Writes the frame that the window captured to the next file of the capture. Called on the GPU device's thread.
void writeCapture(void* PR_NULLABLE context, PrPixelView rgba) {
    Example& e = exampleOf(context);
    String path = format("{}_{:02}.png", e.capture, e.captures);
    size_t size = 0;
    bool written = false;
    if (rgba.data != nullptr) {
        prImageCodecSetEncode(e.images, "image/png", rgba, nullptr, 0, &size);
        Array<uint8_t> png;
        png.resize(uint32_t(size));
        written = prImageCodecSetEncode(e.images, "image/png", rgba, png.data(), png.count(), &size) == PrStatus_Ok &&
            prFileWrite(path, PrByteView{png.data(), size});
    }
    if (!written)
        printError("incremental: cannot write {}\n", path);
    ++e.captures;
    if (e.last)
        e.done.store(true);
    e.capturing.store(false);
}

// Delivers the pieces of the stream that its rate allows before the frame's rounds read the page, and asks for the
// captures that are due.
void beginFrame(void* context, PrStudioWindow* window, const PrAppFrame* f) {
    Example& e = exampleOf(context);
    e.stream.display = f->display;
    e.stream.preferences = f->preferences;
    advance(e.stream, f->delta);
    if (e.capture.isEmpty())
        return;
    if (e.done.load()) {
        prAppStop(prStudioWindowApp(window));
        return;
    }
    e.elapsed += f->delta;
    if (e.capturing.load() || e.last || e.elapsed < e.nextCapture)
        return;
    e.last = finished(e.stream);
    e.nextCapture += e.every;
    e.capturing.store(true);
    prStudioWindowCapture(window, writeCapture, &e);
}

// Takes Escape, which quits, and the wheel over the pane, which scrolls the page and stops it following the end when it
// scrolls up.
bool event(void* context, PrStudioWindow* window, const PrAppEvent* ev) {
    Example& e = exampleOf(context);
    if (ev->kind == PrAppEventKind_KeyDown && ev->keyboard.key == PrAppKey_Escape) {
        prAppStop(prStudioWindowApp(window));
        return true;
    }
    if (ev->kind == PrAppEventKind_PointerMove)
        e.pointer = ev->pointer.position;
    const PrRect& pane = e.stream.pane;
    bool overPane =
        e.pointer.x >= pane.left && e.pointer.x < pane.right && e.pointer.y >= pane.top && e.pointer.y < pane.bottom;
    if (ev->kind != PrAppEventKind_Scroll || !overPane)
        return false;
    float delta = prAppEventScrollPixels(ev, kLineHeight).y;
    e.stream.scrollBy += double(delta);
    if (delta < 0)
        e.stream.follow = false;
    return true;
}

// A stream that is delivering, a page that has tasks, or a run that captures needs the next frame.
bool laidOut(void* context, PrStudioWindow*) {
    const Example& e = exampleOf(context);
    const Stream& s = e.stream;
    bool streaming = !s.paused && !finished(s);
    return streaming || !e.capture.isEmpty() || prDomAgentNextTaskDelay(prHtmlPageAgent(s.page)) >= 0;
}

// Reads the sample, the rate and the capture that the arguments choose into `example`. Returns false for an argument
// that it does not know.
bool readArguments(int argc, char** argv, Example* example) {
    Stream* stream = &example->stream;
    for (int i = 1; i + 1 < argc; i += 2) {
        PrStringView name = argv[i];
        PrStringView value = argv[i + 1];
        if (name == "--rate" || name == "--every") {
            double number = 0;
            if (prF64ParsePrefix(value, &number) != value.size || number <= 0)
                return false;
            if (name == "--rate") {
                stream->rate = float(min(max(number, kMinRate), kMaxRate));
                example->rateGiven = true;
            } else {
                example->every = number;
            }
            continue;
        }
        if (name == "--size") {
            double width = 0;
            double height = 0;
            size_t at = prF64ParsePrefix(value, &width);
            if (at == 0 || at >= value.size || value.data[at] != 'x' ||
                    prF64ParsePrefix(PrStringView{value.data + at + 1, value.size - at - 1}, &height) == 0)
                return false;
            example->size = PrUVec2{uint32_t(width), uint32_t(height)};
            continue;
        }
        if (name == "--capture") {
            example->capture = value;
            continue;
        }
        if (name != "--sample")
            return false;
        bool found = false;
        for (Sample sample = 0; sample < Sample_Count; ++sample) {
            if (value == kSampleNames[sample]) {
                stream->sample = sample;
                found = true;
            }
        }
        if (!found)
            return false;
    }
    return argc % 2 == 1;
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    static Example e;
    if (!readArguments(argc, argv, &e)) {
        printError("usage: incremental [--sample markdown|chat|html|baseline-jpeg|progressive-jpeg|png|interlaced-png|gif] [--rate <bytes a second>] "
            "[--size <width>x<height>] [--capture <prefix> [--every <seconds>]]\n");
        return 2;
    }
    PrFontSet* fonts = prFontSetCreate(nullptr);
    const PrFontBundle bundles[] = {PrFontBundle_Complete};
    if (prFontBundleInstall(fonts, bundles, uint32_t(countOf(bundles))) == 0) {
        printError("incremental: the font bundles among its resources could not be installed\n");
        prFontSetDestroy(fonts);
        return 1;
    }
    PrStudioWindowDesc desc{
        .title = "Prax Incremental",
        .extent = e.size,
        .open = open,
        .palette = [](void*, PrStudioWindow*, const PrAppUserPreferences*, PrKitPalette* palette) {
            *palette = prKitPaletteStudio(PrKitThemeVariant_Dark);
        },
        .beginFrame = beginFrame,
        .event = event,
        .build = [](void* context, PrStudioWindow* window, PrLayoutNode root, PrVec2) {
            buildApp(&exampleOf(context).stream, prStudioWindowKit(window), root);
        },
        .laidOut = laidOut,
        .close = [](void* context, PrStudioWindow*, PrStatus) {
            Stream& s = exampleOf(context).stream;
            closeStream(s);
            prTextFontRelease(s.font);
        },
        .context = &e,
    };
    // The program decodes every format that Prax has.
    PrCompressCodecSet* compress = prCompressCodecSetCreateDefault();
    PrImageCodecSet* images = compress != nullptr ? prImageCodecSetCreateDefault(compress) : nullptr;
    PrTextureFormatSet* formats = nullptr;
    if (images != nullptr) {
        PrTextureFormat* format = prImageTextureFormatCreate(images, PrImageTextureColor_Srgb);
        formats = prTextureFormatSetCreate(&format, 1);
        e.stream.formats = formats;
        e.images = images;
        e.stream.images[Sample_BaselineJpeg] = resourceOr("incremental/baseline.jpg", "incremental/small/baseline.jpg");
        e.stream.images[Sample_ProgressiveJpeg] =
            resourceOr("incremental/progressive.jpg", "incremental/small/progressive.jpg");
        e.stream.images[Sample_Png] = photoPng(compress, images, e.stream.images[Sample_BaselineJpeg], false);
        e.stream.images[Sample_InterlacedPng] = photoPng(compress, images, e.stream.images[Sample_BaselineJpeg], true);
        e.stream.images[Sample_Gif] = resourceOr("incremental/wave.gif", "incremental/small/cycle.gif");
        if (!e.rateGiven)
            e.stream.rate = rateOf(e.stream, e.stream.sample);
    }
    PrStudioAppDesc app{.fonts = fonts, .compress = compress, .images = images, .window = &desc};
    PrStatus status = prStudioAppRun(&app);
    prTextureFormatSetDestroy(formats);
    prImageCodecSetDestroy(images);
    prCompressCodecSetDestroy(compress);
    prFontSetDestroy(fonts);
    if (status == PrStatus_NotSupported)
        printError("incremental: no GPU device to render with\n");
    return status == PrStatus_Ok ? 0 : 1;
}
