#include "pr/markdown.h"
#include "pr/markdown_html.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

extern "C" int prTestMarkdownCApi(void);

namespace pr_impl {

namespace {

std::string viewString(PrStringView view) {
    return std::string(view.data != nullptr ? view.data : "", view.size);
}

std::string blocksHtml(const PrMarkdown* document, uint32_t first, uint32_t count) {
    PrString out = prStringEmpty();
    prMarkdownAppendHtml(document, first, count, &out);
    std::string html = viewString(prStringView(&out));
    prStringReset(&out);
    return html;
}

// Returns the HTML of the whole document: its blocks', then its footnotes'.
std::string documentHtml(const PrMarkdown* document) {
    std::string html = blocksHtml(document, 0, prMarkdownBlockCount(document));
    PrString footnotes = prStringEmpty();
    prMarkdownAppendFootnotesHtml(document, &footnotes);
    html += viewString(prStringView(&footnotes));
    prStringReset(&footnotes);
    return html;
}

// GFM's extensions, whose HTML is the specifications'.
const PrMarkdownOptions* gfm() {
    static PrMarkdownOptions options = [] {
        PrMarkdownOptions gfmOptions = prMarkdownOptionsDefault();
        gfmOptions.extensions = PrMarkdownExtensionFlag_Gfm;
        return gfmOptions;
    }();
    return &options;
}

// Returns the HTML of `source` parsed whole with `options`, or with GFM's.
std::string html(PrStringView source, const PrMarkdownOptions* options = nullptr) {
    PrMarkdown* document = prMarkdownCreate(options != nullptr ? options : gfm());
    prMarkdownParse(document, source);
    std::string out = documentHtml(document);
    prMarkdownDestroy(document);
    return out;
}

// A client that keeps the HTML of each block and applies the document's changes to it.
struct Client {
    std::vector<std::string> blocks;
    std::vector<PrMarkdownChange> changes;

    void update(PrMarkdown* document) {
        changes.clear();
        PrMarkdownChange taken[2];
        uint32_t count = 0;
        while ((count = prMarkdownTakeChanges(document, taken, 2)) != 0) {
            for (uint32_t i = 0; i < count; ++i) {
                const PrMarkdownChange& change = taken[i];
                changes.push_back(change);
                blocks.erase(blocks.begin() + change.first, blocks.begin() + change.first + change.removedCount);
                for (uint32_t b = 0; b < change.insertedCount; ++b)
                    blocks.insert(blocks.begin() + change.first + b, blocksHtml(document, change.first + b, 1));
            }
        }
    }

    std::string joined() const {
        std::string out;
        for (const std::string& block : blocks)
            out += block;
        return out;
    }
};

bool hasChange(const Client& client, uint32_t first, uint32_t removed, uint32_t inserted) {
    for (const PrMarkdownChange& change : client.changes) {
        if (change.first == first && change.removedCount == removed && change.insertedCount == inserted)
            return true;
    }
    return false;
}

// Every extension, with raw HTML passed through.
PrMarkdownOptions everything() {
    PrMarkdownOptions options = prMarkdownOptionsDefault();
    options.extensions = PrMarkdownExtensionFlag_GitHub | PrMarkdownExtensionFlag_SmartPunctuation
        | PrMarkdownExtensionFlag_Highlight | PrMarkdownExtensionFlag_Scripts | PrMarkdownExtensionFlag_FrontMatter;
    return options;
}

// Returns true if parsing `source` a byte at a time gives the HTML of parsing it whole, both as a client's blocks and
// at the end.
bool streams(PrStringView source, const PrMarkdownOptions& options) {
    PrMarkdown* document = prMarkdownCreate(&options);
    Client client;
    for (size_t i = 0; i < source.size; ++i) {
        prMarkdownAppend(document, stringSlice(source, i, 1));
        client.update(document);
    }
    prMarkdownEnd(document);
    client.update(document);
    PrString footnotes = prStringEmpty();
    prMarkdownAppendFootnotesHtml(document, &footnotes);
    std::string streamed = client.joined() + viewString(prStringView(&footnotes));
    prStringReset(&footnotes);
    bool same = streamed == html(source, &options) && documentHtml(document) == streamed;
    prMarkdownDestroy(document);
    return same;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestMarkdownCApi() == 0);
}

TEST_CASE("blocks and inlines write the HTML of the specification") {
    CHECK(html("# Title\n\nSome *emphasis* and **strong**.\n")
          == "<h1>Title</h1>\n<p>Some <em>emphasis</em> and <strong>strong</strong>.</p>\n");
    CHECK(html("- a\n- b\n") == "<ul>\n<li>a</li>\n<li>b</li>\n</ul>\n");
    CHECK(html("[x]\n\n[x]: /u \"t\"\n") == "<p><a href=\"/u\" title=\"t\">x</a></p>\n");
}

TEST_CASE("blocks before the first open block are final, and later lines do not change them") {
    PrMarkdown* document = prMarkdownCreate(gfm());
    prMarkdownAppend(document, "# One\n\npara");
    CHECK(prMarkdownBlockCount(document) == 2);
    CHECK(prMarkdownFinalCount(document) == 1);

    // The paragraph is open until a line closes it, and a setext underline makes it a heading.
    prMarkdownAppend(document, "graph\n");
    CHECK(prMarkdownFinalCount(document) == 1);
    prMarkdownAppend(document, "===\n");
    CHECK(blocksHtml(document, 1, 1) == "<h1>paragraph</h1>\n");
    // The heading is final once a line after it closes it.
    CHECK(prMarkdownFinalCount(document) == 1);
    prMarkdownAppend(document, "\n- a\n");
    CHECK(prMarkdownFinalCount(document) == 2);
    CHECK(!prMarkdownIsEnded(document));
    prMarkdownEnd(document);
    CHECK(prMarkdownIsEnded(document));
    CHECK(prMarkdownFinalCount(document) == 3);
    prMarkdownDestroy(document);
}

TEST_CASE("a line that is not complete closes no block") {
    PrMarkdown* document = prMarkdownCreate(gfm());
    // "#" could become "#x", which continues the paragraph, so the paragraph stays open.
    prMarkdownAppend(document, "para\n#");
    CHECK(prMarkdownFinalCount(document) == 0);
    prMarkdownAppend(document, "x\n");
    CHECK(prMarkdownBlockCount(document) == 1);
    CHECK(blocksHtml(document, 0, 1) == "<p>para\n#x</p>\n");
    prMarkdownDestroy(document);
}

TEST_CASE("a CR at the end of a piece may be the first half of a CRLF") {
    PrMarkdown* document = prMarkdownCreate(gfm());
    prMarkdownAppend(document, "a\r");
    prMarkdownAppend(document, "\nb\r\n\r\n");
    prMarkdownEnd(document);
    CHECK(documentHtml(document) == "<p>a\nb</p>\n");
    prMarkdownDestroy(document);
}

TEST_CASE("changes replay onto a client's blocks, and final blocks are kept") {
    const char* source = "# Title\n\nFirst *para*.\n\n- one\n- two\n\n  three\n\n> quote\n\n```c\ncode\n```\n";
    PrMarkdown* document = prMarkdownCreate(gfm());
    Client client;
    PrStringView text = source;
    for (size_t i = 0; i < text.size; i += 3) {
        size_t size = text.size - i < 3 ? text.size - i : 3;
        uint32_t final = prMarkdownFinalCount(document);
        prMarkdownAppend(document, stringSlice(text, i, size));
        client.update(document);
        // Only the blocks that were tentative are replaced.
        for (const PrMarkdownChange& change : client.changes)
            CHECK(change.first >= final);
    }
    prMarkdownEnd(document);
    client.update(document);
    CHECK(client.joined() == html(source));
    prMarkdownDestroy(document);
}

TEST_CASE("a list that becomes loose is rebuilt whole while it is open") {
    PrMarkdown* document = prMarkdownCreate(gfm());
    Client client;
    prMarkdownAppend(document, "- a\n- b\n");
    client.update(document);
    CHECK(client.joined() == "<ul>\n<li>a</li>\n<li>b</li>\n</ul>\n");
    prMarkdownAppend(document, "\n- c\n");
    client.update(document);
    CHECK(client.joined() == "<ul>\n<li>\n<p>a</p>\n</li>\n<li>\n<p>b</p>\n</li>\n<li>\n<p>c</p>\n</li>\n</ul>\n");
    prMarkdownDestroy(document);
}

TEST_CASE("a late definition resolves the references of final blocks, which are reported one by one") {
    PrMarkdown* document = prMarkdownCreate(gfm());
    Client client;
    prMarkdownAppend(document, "see [x]\n\nmiddle\n\nand [x]\n\n");
    client.update(document);
    CHECK(prMarkdownFinalCount(document) == 3);
    CHECK(client.joined() == "<p>see [x]</p>\n<p>middle</p>\n<p>and [x]</p>\n");

    prMarkdownAppend(document, "[x]: /u\n");
    client.update(document);
    CHECK(hasChange(client, 0, 1, 1));
    CHECK(hasChange(client, 2, 1, 1));
    CHECK(!hasChange(client, 1, 1, 1));
    CHECK(client.joined() == "<p>see <a href=\"/u\">x</a></p>\n<p>middle</p>\n<p>and <a href=\"/u\">x</a></p>\n");

    // A tentative definition that turns out not to be one, here the header row of a table, gives the references back.
    prMarkdownClear(document);
    client.blocks.clear();
    prMarkdownAppend(document, "[y]\n\n[y]: /v\n");
    client.update(document);
    CHECK(client.joined() == "<p><a href=\"/v\">y</a></p>\n");
    prMarkdownAppend(document, "|-|\n");
    client.update(document);
    CHECK(client.joined() == "<p>[y]</p>\n<table>\n<thead>\n<tr>\n<th>[y]: /v</th>\n</tr>\n</thead>\n</table>\n");
    prMarkdownDestroy(document);
}

TEST_CASE("a client that waits for final blocks sees each block once") {
    const char* source = "Para one\nstill one.\n\n## Two\n\n- a\n- b\n\n***\n";
    PrMarkdown* document = prMarkdownCreate(gfm());
    std::string written;
    uint32_t walked = 0;
    for (const char* c = source; *c != '\0'; ++c) {
        prMarkdownAppend(document, PrStringView{c, 1});
        uint32_t final = prMarkdownFinalCount(document);
        written += blocksHtml(document, walked, final - walked);
        walked = final;
    }
    prMarkdownEnd(document);
    written += blocksHtml(document, walked, prMarkdownFinalCount(document) - walked);
    CHECK(written == html(source));
    prMarkdownDestroy(document);
}

TEST_CASE("footnotes are numbered by their first reference, and their section reports its changes") {
    PrMarkdownOptions options = prMarkdownOptionsDefault();
    options.extensions |= PrMarkdownExtensionFlag_Footnotes;
    PrMarkdown* document = prMarkdownCreate(&options);
    prMarkdownAppend(document, "a[^b] c[^a] d[^b] e[^none]\n\n[^a]: A\n");
    CHECK(prMarkdownTakeFootnotesChanged(document));
    CHECK(!prMarkdownTakeFootnotesChanged(document));
    prMarkdownAppend(document, "\n[^b]: B\n");
    CHECK(prMarkdownTakeFootnotesChanged(document));
    prMarkdownEnd(document);
    std::string out = documentHtml(document);
    CHECK(out.find("href=\"#fn-b\" id=\"fnref-b\" data-footnote-ref>1</a>") != std::string::npos);
    CHECK(out.find("href=\"#fn-a\" id=\"fnref-a\" data-footnote-ref>2</a>") != std::string::npos);
    CHECK(out.find("id=\"fnref-b-2\" data-footnote-ref>1</a>") != std::string::npos);
    CHECK(out.find("e[^none]") != std::string::npos);
    CHECK(out.find("<li id=\"fn-b\">") < out.find("<li id=\"fn-a\">"));
    prMarkdownDestroy(document);
}

TEST_CASE("raw HTML passes, is filtered or is escaped, as the options say") {
    PrMarkdownOptions options = prMarkdownOptionsDefault();
    const char* source = "<div>\n<script>x</script>\n</div>\n\n*a* <b>b</b>\n";
    CHECK(html(source, &options) == "<div>\n<script>x</script>\n</div>\n<p><em>a</em> <b>b</b></p>\n");
    options.rawHtml = PrMarkdownRawHtml_Filter;
    CHECK(html(source, &options) == "<div>\n&lt;script>x&lt;/script>\n</div>\n<p><em>a</em> <b>b</b></p>\n");
    options.rawHtml = PrMarkdownRawHtml_Escape;
    CHECK(html(source, &options)
          == "<p>&lt;div&gt;\n&lt;script&gt;x&lt;/script&gt;\n&lt;/div&gt;</p>\n<p><em>a</em> &lt;b&gt;b&lt;/b&gt;</p>\n");
}

TEST_CASE("safe destinations drop the schemes that run code or read files") {
    PrMarkdownOptions options = prMarkdownOptionsDefault();
    options.destinations = PrMarkdownDestinations_Safe;
    CHECK(html("[a](javascript:alert(1)) [b](https://x.y) ![c](data:image/png;base64,AA) [d](data:text/html,x)",
               &options)
          == "<p><a href=\"\">a</a> <a href=\"https://x.y\">b</a> <img src=\"data:image/png;base64,AA\" alt=\"c\" /> "
             "<a href=\"\">d</a></p>\n");
}

TEST_CASE("HTML events carry tags, attributes and unescaped text") {
    PrMarkdown* document = prMarkdownCreate(gfm());
    prMarkdownParse(document, "[a & b](</x y> \"t\") <i>raw</i>\n\n---\n");
    std::vector<std::string> events;
    prMarkdownWriteHtml(
        document, 0, prMarkdownBlockCount(document),
        [](void* context, const PrMarkdownHtmlEvent* event) {
            std::string line;
            switch (event->kind) {
                case PrMarkdownHtmlEventKind_StartTag:
                    line = "<" + viewString(event->name);
                    for (uint32_t i = 0; i < event->attributeCount; ++i)
                        line += " " + viewString(event->attributes[i].name) + "="
                            + viewString(event->attributes[i].value);
                    line += event->selfClosing ? "/>" : ">";
                    break;
                case PrMarkdownHtmlEventKind_EndTag:
                    line = "</" + viewString(event->name) + ">";
                    break;
                case PrMarkdownHtmlEventKind_Text:
                    line = "text:" + viewString(event->text);
                    break;
                case PrMarkdownHtmlEventKind_Html:
                    line = "html:" + viewString(event->text);
                    break;
            }
            static_cast<std::vector<std::string>*>(context)->push_back(line);
        },
        &events);
    std::vector<std::string> expected = {
        "<p>", "<a href=/x%20y title=t>", "text:a & b", "</a>", "text: ", "html:<i>", "text:raw", "html:</i>", "</p>",
        "text:\n", "<hr/>", "text:\n",
    };
    CHECK(events == expected);
    prMarkdownDestroy(document);
}

TEST_CASE("an alert is a block quote whose first line is its kind's marker") {
    PrMarkdownOptions options = everything();
    CHECK(html("> [!NOTE]\n> Useful.\n", &options)
          == "<div class=\"markdown-alert markdown-alert-note\">\n<p class=\"markdown-alert-title\">Note</p>\n"
             "<p>Useful.</p>\n</div>\n");
    CHECK(html("> [!caution]\n\n> [!TIP] text\n", &options)
          == "<div class=\"markdown-alert markdown-alert-caution\">\n<p class=\"markdown-alert-title\">Caution</p>\n"
             "</div>\n<blockquote>\n<p>[!TIP] text</p>\n</blockquote>\n");
    CHECK(streams("> [!WARNING]\n> One\n>\n> Two\n\nAfter\n", options));
}

TEST_CASE("heading ids are slugs, numbered after the first heading that takes them") {
    PrMarkdownOptions options = everything();
    CHECK(html("# Hello, World!\n\n## Hello World\n\n### `Code` _and_ émphasis\n\n# Hello World\n", &options)
          == "<h1 id=\"hello-world\">Hello, World!</h1>\n<h2 id=\"hello-world-1\">Hello World</h2>\n"
             "<h3 id=\"code-and-émphasis\"><code>Code</code> <em>and</em> émphasis</h3>\n"
             "<h1 id=\"hello-world-2\">Hello World</h1>\n");
    CHECK(streams("# A\n\nA\n=\n\n# A\n", options));
}

TEST_CASE("highlights, subscripts and superscripts pair runs of their characters") {
    PrMarkdownOptions options = everything();
    CHECK(html("==mark== H~2~O x^2^ ~~gone~~ ~one==\n", &options)
          == "<p><mark>mark</mark> H<sub>2</sub>O x<sup>2</sup> <del>gone</del> ~one==</p>\n");
    PrMarkdownOptions plain = *gfm();
    CHECK(html("==mark== ~one~ x^2^\n", &plain) == "<p>==mark== <del>one</del> x^2^</p>\n");
}

TEST_CASE("math is TeX between dollar signs") {
    PrMarkdownOptions options = everything();
    CHECK(html("$x^2$ and $$\\sum_i i$$ and $`a$b`$\n\nbut $5 and $6.\n\n$ x $, \\$y\\$\n", &options)
          == "<p><span class=\"math math-inline\">x^2</span> and <span class=\"math math-display\">\\sum_i i</span> "
             "and <span class=\"math math-inline\">a$b</span></p>\n<p>but $5 and $6.</p>\n<p>$ x $, $y$</p>\n");
    CHECK(streams("Inline $a*b*c$ *emphasis*\n\n$$\nx\n$$\n", options));
}

TEST_CASE("front matter is held as it is written, and keeps the blocks after its opening fence tentative") {
    PrMarkdownOptions options = everything();
    PrMarkdown* document = prMarkdownCreate(&options);
    prMarkdownAppend(document, "---\ntitle: x\n\n# Not yet\n\n");
    CHECK(prMarkdownFinalCount(document) == 0);
    prMarkdownAppend(document, "---\n# Body\n\nText\n");
    CHECK(prMarkdownFinalCount(document) == 2);
    std::string fields;
    prMarkdownWalk(
        document, 0, 1,
        [](void* context, const PrMarkdownStep* step) {
            if (step->kind == PrMarkdownStepKind_Text)
                *static_cast<std::string*>(context) += viewString(step->text);
        },
        &fields);
    CHECK(fields == "title: x\n\n# Not yet\n\n");
    prMarkdownEnd(document);
    CHECK(documentHtml(document) == "<h1 id=\"body\">Body</h1>\n<p>Text</p>\n");
    prMarkdownDestroy(document);

    CHECK(html("---\nno closing fence\n", &options) == "<hr />\n<p>no closing fence</p>\n");
    CHECK(streams("---\na: 1\n...\nBody\n", options));
}

TEST_CASE("a last line waits for its line ending only if the ending can change what it shows") {
    PrMarkdown* document = prMarkdownCreate(gfm());
    struct Piece {
        const char* text;
        const char* html;
    };
    const Piece pieces[] = {
        {"Text\n\n[s", "<p>Text</p>\n"},                                        // a definition, perhaps
        {"pec]: /u\n", "<p>Text</p>\n"},
        {"- an it", "<p>Text</p>\n<ul>\n<li>an it</li>\n</ul>\n"},              // an item's text
        {"em\n\nPara", "<p>Text</p>\n<ul>\n<li>an item</li>\n</ul>\n<p>Para</p>\n"},
        {"\n--", "<p>Text</p>\n<ul>\n<li>an item</li>\n</ul>\n<p>Para</p>\n"},  // a setext underline, perhaps
        {"-\n", "<p>Text</p>\n<ul>\n<li>an item</li>\n</ul>\n<h2>Para</h2>\n"},
    };
    for (const Piece& piece : pieces) {
        prMarkdownAppend(document, piece.text);
        CHECK(blocksHtml(document, 0, prMarkdownBlockCount(document)) == piece.html);
    }
    prMarkdownDestroy(document);
}

TEST_CASE("an extended autolink is not found in code") {
    CHECK(html("`a@b.co` and a@b.co and `www.x.com`\n")
          == "<p><code>a@b.co</code> and <a href=\"mailto:a@b.co\">a@b.co</a> and <code>www.x.com</code></p>\n");
}

TEST_CASE("every extension streams as it parses whole") {
    const char* source =
        "---\ntitle: t\n---\n# Answer\n\n> [!NOTE]\n> See [the docs][d] and the footnote.[^1]\n\n"
        "| a | b |\n|:--|--:|\n| ==x== | $y$ |\n\n- [x] done \"quoted\" -- dash...\n- [ ] H~2~O\n\n"
        "```python\nprint(1)\n```\n\n# Answer\n\n[^1]: A footnote.\n[d]: https://example.com\n";
    CHECK(streams(source, everything()));
}
