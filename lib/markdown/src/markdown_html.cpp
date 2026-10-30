// Markdown as HTML: the tags and text of a document's HTML, and the HTML text that they make.

#include "pr/markdown_html.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/text_buffer.h"

namespace pr_impl {

namespace {

// Returns true if a URL holds `c` as it is when it is percent-encoded: an ASCII letter or digit, or a punctuation
// character other than the quote, the angle brackets, the square and curly brackets, the backslash, the caret, the
// backtick and the vertical bar. A '%' is kept, so a URL that is percent-encoded already stays as it is.
bool isUrlSafe(char c) {
    if (asciiIsLetter(c) || asciiIsDigit(c))
        return true;
    switch (c) {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '(':
        case ')':
        case '*':
        case '+':
        case ',':
        case '-':
        case '.':
        case '/':
        case ':':
        case ';':
        case '=':
        case '?':
        case '@':
        case '_':
        case '~':
            return true;
        default:
            return false;
    }
}

void encodeUrl(PrStringView url, TextBuffer* out) {
    static const char kHex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < url.size; ++i) {
        char c = url.data[i];
        if (isUrlSafe(c)) {
            textAppendChar(out, c);
        } else {
            textAppendChar(out, '%');
            textAppendChar(out, kHex[(uint8_t(c) >> 4) & 0xf]);
            textAppendChar(out, kHex[uint8_t(c) & 0xf]);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Events

// The most attributes that a start tag has.
constexpr uint32_t kMaxAttributes = 5;

// A range of the writer's values.
struct ValueRange {
    uint32_t start;
    uint32_t size;
};

// A start tag being made, whose views stay valid until it is written.
struct Tag {
    PrStringView name;
    bool selfClosing;
    PrStringView names[kMaxAttributes];
    ValueRange values[kMaxAttributes];  // a value whose start is kNoValue has no value
    uint32_t count;
};

constexpr uint32_t kNoValue = 0xffffffffu;

// Makes the events of a walk's steps.
struct Writer {
    PrMarkdownHtmlFn fn;
    void* context;
    bool atLineStart;               // true if nothing is written yet, or the last text written ends with a line feed

    TextBuffer values;              // the text of the values of the tag being made
    TextBuffer description;         // an image's description, which is its alt value
    uint32_t imageDepth;            // the depth of images whose descriptions are being collected

    // The footnote definition being written: the depth of the open nodes at which its children are, or 0 outside one.
    // The end of a child paragraph waits until the next child or the definition's end, since the links back to the
    // footnote's references go inside the definition's last paragraph.
    uint32_t depth;
    uint32_t footnoteDepth;
    bool paragraphWaits;

    bool inFrontMatter;             // true while the steps of front matter, which has no HTML, are walked
};

void emit(Writer* w, const PrMarkdownHtmlEvent& event) {
    w->fn(w->context, &event);
}

void text(Writer* w, PrStringView characters) {
    if (characters.size == 0)
        return;
    PrMarkdownHtmlEvent event{};
    event.kind = PrMarkdownHtmlEventKind_Text;
    event.text = characters;
    emit(w, event);
    w->atLineStart = characters.data[characters.size - 1] == '\n';
}

void html(Writer* w, PrStringView raw) {
    if (raw.size == 0)
        return;
    PrMarkdownHtmlEvent event{};
    event.kind = PrMarkdownHtmlEventKind_Html;
    event.text = raw;
    emit(w, event);
    w->atLineStart = raw.data[raw.size - 1] == '\n';
}

// Writes a line feed unless the HTML is at the start of a line.
void cr(Writer* w) {
    if (!w->atLineStart)
        text(w, "\n");
}

Tag startTag(Writer* w, PrStringView name, bool selfClosing = false) {
    textClear(&w->values);
    Tag tag{};
    tag.name = name;
    tag.selfClosing = selfClosing;
    return tag;
}

// Adds an attribute whose value is `value`, or that has no value if `value` is the null view.
void attribute(Writer* w, Tag* tag, PrStringView name, PrStringView value) {
    PR_ASSERT(tag->count < kMaxAttributes);
    tag->names[tag->count] = name;
    if (value.data == nullptr) {
        tag->values[tag->count] = ValueRange{kNoValue, 0};
    } else {
        uint32_t mark = w->values.text.count;
        textAppend(&w->values, value);
        tag->values[tag->count] = ValueRange{mark, w->values.text.count - mark};
    }
    ++tag->count;
}

// Adds an attribute whose value is `url` percent-encoded, after `prefix`.
void urlAttribute(Writer* w, Tag* tag, PrStringView name, PrStringView prefix, PrStringView url) {
    uint32_t mark = w->values.text.count;
    textAppend(&w->values, prefix);
    encodeUrl(url, &w->values);
    tag->names[tag->count] = name;
    tag->values[tag->count] = ValueRange{mark, w->values.text.count - mark};
    ++tag->count;
}

void writeTag(Writer* w, const Tag& tag) {
    PrMarkdownHtmlAttribute attributes[kMaxAttributes];
    for (uint32_t i = 0; i < tag.count; ++i) {
        attributes[i].name = tag.names[i];
        // An empty value is the empty view, which is not null, though the values may hold no storage yet.
        if (tag.values[i].start == kNoValue)
            attributes[i].value = PrStringView{};
        else if (tag.values[i].size == 0)
            attributes[i].value = "";
        else
            attributes[i].value = PrStringView{w->values.text.data + tag.values[i].start, tag.values[i].size};
    }
    PrMarkdownHtmlEvent event{};
    event.kind = PrMarkdownHtmlEventKind_StartTag;
    event.selfClosing = tag.selfClosing;
    event.name = tag.name;
    event.attributes = tag.count != 0 ? attributes : nullptr;
    event.attributeCount = tag.count;
    emit(w, event);
    w->atLineStart = false;
}

void start(Writer* w, PrStringView name) {
    writeTag(w, startTag(w, name));
}

void end(Writer* w, PrStringView name) {
    PrMarkdownHtmlEvent event{};
    event.kind = PrMarkdownHtmlEventKind_EndTag;
    event.name = name;
    emit(w, event);
    w->atLineStart = false;
}

// Returns the decimal digits of `number`, written into `buffer`.
PrStringView decimal(char (&buffer)[16], uint32_t number) {
    size_t size = 0;
    char digits[16];
    do {
        digits[size++] = char('0' + number % 10);
        number /= 10;
    } while (number != 0);
    for (size_t i = 0; i < size; ++i)
        buffer[i] = digits[size - 1 - i];
    return PrStringView{buffer, size};
}

// Writes the links from a footnote's definition back to each of its references.
void writeBackReferences(Writer* w, const PrMarkdownStep* step) {
    char numberText[16];
    PrStringView number = decimal(numberText, step->number);
    for (uint32_t i = 1; i <= step->referenceCount; ++i) {
        char indexText[16];
        PrStringView index = decimal(indexText, i);
        if (i > 1)
            text(w, " ");
        // A reference after the first is numbered after its footnote's number.
        TextBuffer suffix;
        if (i > 1) {
            textAppendChar(&suffix, '-');
            textAppend(&suffix, index);
        }
        Tag tag = startTag(w, "a");
        TextBuffer href;
        textAppend(&href, "#fnref-");
        encodeUrl(step->label, &href);
        textAppend(&href, textView(&suffix));
        attribute(w, &tag, "href", textView(&href));
        attribute(w, &tag, "class", "footnote-backref");
        attribute(w, &tag, "data-footnote-backref", PrStringView{});
        TextBuffer idx;
        textAppend(&idx, number);
        textAppend(&idx, textView(&suffix));
        attribute(w, &tag, "data-footnote-backref-idx", textView(&idx));
        TextBuffer label;
        textAppend(&label, "Back to reference ");
        textAppend(&label, textView(&idx));
        attribute(w, &tag, "aria-label", textView(&label));
        writeTag(w, tag);
        text(w, "\xe2\x86\xa9");
        if (i > 1) {
            writeTag(w, [&] {
                Tag sup = startTag(w, "sup");
                attribute(w, &sup, "class", "footnote-ref");
                return sup;
            }());
            text(w, index);
            end(w, "sup");
        }
        end(w, "a");
        textReset(&suffix);
        textReset(&href);
        textReset(&idx);
        textReset(&label);
    }
}

// Writes the end of a footnote definition's paragraph that waited to learn whether it was the definition's last child.
void endWaitingParagraph(Writer* w) {
    if (!w->paragraphWaits)
        return;
    w->paragraphWaits = false;
    end(w, "p");
    text(w, "\n");
}

void writeStep(void* PR_NULLABLE context, const PrMarkdownStep* step) {
    Writer* w = static_cast<Writer*>(context);
    bool enter = step->kind == PrMarkdownStepKind_Enter;
    if (step->node == PrMarkdownNodeKind_FrontMatter && step->kind != PrMarkdownStepKind_Text) {
        w->inFrontMatter = enter;
        return;
    }
    if (w->inFrontMatter)
        return;
    if (w->footnoteDepth != 0 && step->kind != PrMarkdownStepKind_Text) {
        if (enter && w->depth == w->footnoteDepth)
            endWaitingParagraph(w);
        if (!enter && w->depth == w->footnoteDepth + 1 && step->node == PrMarkdownNodeKind_Paragraph) {
            w->paragraphWaits = true;
            --w->depth;
            return;
        }
    }
    if (step->kind == PrMarkdownStepKind_Enter)
        ++w->depth;
    else if (step->kind == PrMarkdownStepKind_Leave)
        --w->depth;

    // An image's description is the text of its descendants, which is its alt value.
    if (w->imageDepth != 0) {
        if (step->kind == PrMarkdownStepKind_Text) {
            textAppend(&w->description, step->text);
        } else if (step->node == PrMarkdownNodeKind_Image) {
            if (enter)
                ++w->imageDepth;
            else
                --w->imageDepth;
            if (w->imageDepth == 0) {
                Tag tag = startTag(w, "img", true);
                urlAttribute(w, &tag, "src", "", step->destination);
                attribute(w, &tag, "alt", textView(&w->description));
                if (step->title.size != 0)
                    attribute(w, &tag, "title", step->title);
                writeTag(w, tag);
            }
        } else if (enter && (step->node == PrMarkdownNodeKind_SoftBreak || step->node == PrMarkdownNodeKind_HardBreak)) {
            textAppendChar(&w->description, ' ');
        }
        return;
    }
    if (step->kind == PrMarkdownStepKind_Text) {
        if (step->textKind == PrMarkdownTextKind_Html)
            html(w, step->text);
        else
            text(w, step->text);
        return;
    }

    switch (step->node) {
        case PrMarkdownNodeKind_Paragraph:
            if (enter) {
                cr(w);
                start(w, "p");
            } else {
                end(w, "p");
                text(w, "\n");
            }
            break;
        case PrMarkdownNodeKind_Heading: {
            static const char* const kNames[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
            PrStringView name = kNames[step->level >= 1 && step->level <= 6 ? step->level - 1 : 0];
            if (enter) {
                cr(w);
                Tag tag = startTag(w, name);
                if (step->id.size != 0)
                    attribute(w, &tag, "id", step->id);
                writeTag(w, tag);
            } else {
                end(w, name);
                text(w, "\n");
            }
            break;
        }
        case PrMarkdownNodeKind_ThematicBreak:
            if (enter) {
                cr(w);
                writeTag(w, startTag(w, "hr", true));
                text(w, "\n");
            }
            break;
        case PrMarkdownNodeKind_BlockQuote:
            cr(w);
            if (enter)
                start(w, "blockquote");
            else
                end(w, "blockquote");
            text(w, "\n");
            break;
        case PrMarkdownNodeKind_List: {
            PrStringView name = step->list == PrMarkdownListKind_Ordered ? "ol" : "ul";
            if (!enter) {
                end(w, name);
                text(w, "\n");
                break;
            }
            cr(w);
            Tag tag = startTag(w, name);
            char startText[16];
            if (step->list == PrMarkdownListKind_Ordered && step->start != 1)
                attribute(w, &tag, "start", decimal(startText, step->start));
            writeTag(w, tag);
            text(w, "\n");
            break;
        }
        case PrMarkdownNodeKind_Item:
            if (!enter) {
                end(w, "li");
                text(w, "\n");
                break;
            }
            cr(w);
            start(w, "li");
            if (step->task != PrMarkdownTask_None) {
                Tag tag = startTag(w, "input", true);
                attribute(w, &tag, "type", "checkbox");
                if (step->task == PrMarkdownTask_Checked)
                    attribute(w, &tag, "checked", "");
                attribute(w, &tag, "disabled", "");
                writeTag(w, tag);
                text(w, " ");
            }
            break;
        case PrMarkdownNodeKind_Table:
            cr(w);
            if (enter) {
                start(w, "table");
            } else {
                end(w, "table");
                cr(w);
            }
            break;
        case PrMarkdownNodeKind_TableHead:
            cr(w);
            if (enter)
                start(w, "thead");
            else
                end(w, "thead");
            break;
        case PrMarkdownNodeKind_TableBody:
            cr(w);
            if (enter)
                start(w, "tbody");
            else
                end(w, "tbody");
            break;
        case PrMarkdownNodeKind_TableRow:
            cr(w);
            if (enter)
                start(w, "tr");
            else
                end(w, "tr");
            break;
        case PrMarkdownNodeKind_TableCell: {
            PrStringView name = step->header ? "th" : "td";
            if (!enter) {
                end(w, name);
                break;
            }
            cr(w);
            Tag tag = startTag(w, name);
            static const char* const kAlignments[] = {"", "left", "center", "right"};
            if (step->alignment != PrMarkdownAlignment_None && step->alignment < countOf(kAlignments))
                attribute(w, &tag, "align", kAlignments[step->alignment]);
            writeTag(w, tag);
            break;
        }
        case PrMarkdownNodeKind_CodeBlock: {
            if (!enter) {
                end(w, "code");
                end(w, "pre");
                text(w, "\n");
                break;
            }
            cr(w);
            start(w, "pre");
            Tag tag = startTag(w, "code");
            if (step->info.size != 0) {
                // The class is the info string's first word.
                size_t word = 0;
                while (word < step->info.size && !asciiIsSpace(step->info.data[word]))
                    ++word;
                TextBuffer value;
                textAppend(&value, "language-");
                textAppend(&value, PrStringView{step->info.data, word});
                attribute(w, &tag, "class", textView(&value));
                textReset(&value);
            }
            writeTag(w, tag);
            break;
        }
        case PrMarkdownNodeKind_HtmlBlock:
            cr(w);
            break;
        case PrMarkdownNodeKind_Alert: {
            // GitHub's markup for an alert: a div of its kind, whose first paragraph is the kind's title.
            static const char* const kClasses[] = {
                "", "markdown-alert markdown-alert-note", "markdown-alert markdown-alert-tip",
                "markdown-alert markdown-alert-important", "markdown-alert markdown-alert-warning",
                "markdown-alert markdown-alert-caution",
            };
            static const char* const kTitles[] = {"", "Note", "Tip", "Important", "Warning", "Caution"};
            uint32_t kind = step->alert < countOf(kClasses) ? step->alert : 0;
            cr(w);
            if (!enter) {
                end(w, "div");
                text(w, "\n");
                break;
            }
            Tag tag = startTag(w, "div");
            attribute(w, &tag, "class", kClasses[kind]);
            writeTag(w, tag);
            text(w, "\n");
            Tag title = startTag(w, "p");
            attribute(w, &title, "class", "markdown-alert-title");
            writeTag(w, title);
            text(w, kTitles[kind]);
            end(w, "p");
            text(w, "\n");
            break;
        }
        case PrMarkdownNodeKind_Math: {
            // TeX is text in a span, which a client typesets.
            if (!enter) {
                end(w, "span");
                break;
            }
            Tag tag = startTag(w, "span");
            attribute(w, &tag, "class", step->display ? "math math-display" : "math math-inline");
            writeTag(w, tag);
            break;
        }
        case PrMarkdownNodeKind_Highlight:
            if (enter)
                start(w, "mark");
            else
                end(w, "mark");
            break;
        case PrMarkdownNodeKind_Subscript:
            if (enter)
                start(w, "sub");
            else
                end(w, "sub");
            break;
        case PrMarkdownNodeKind_Superscript:
            if (enter)
                start(w, "sup");
            else
                end(w, "sup");
            break;
        case PrMarkdownNodeKind_SoftBreak:
            if (enter)
                text(w, "\n");
            break;
        case PrMarkdownNodeKind_HardBreak:
            if (enter) {
                writeTag(w, startTag(w, "br", true));
                text(w, "\n");
            }
            break;
        case PrMarkdownNodeKind_Code:
            if (enter)
                start(w, "code");
            else
                end(w, "code");
            break;
        case PrMarkdownNodeKind_Emphasis:
            if (enter)
                start(w, "em");
            else
                end(w, "em");
            break;
        case PrMarkdownNodeKind_Strong:
            if (enter)
                start(w, "strong");
            else
                end(w, "strong");
            break;
        case PrMarkdownNodeKind_Strikethrough:
            if (enter)
                start(w, "del");
            else
                end(w, "del");
            break;
        case PrMarkdownNodeKind_Link: {
            if (!enter) {
                end(w, "a");
                break;
            }
            Tag tag = startTag(w, "a");
            urlAttribute(w, &tag, "href", "", step->destination);
            if (step->title.size != 0)
                attribute(w, &tag, "title", step->title);
            writeTag(w, tag);
            break;
        }
        case PrMarkdownNodeKind_Image:
            if (enter) {
                textClear(&w->description);
                w->imageDepth = 1;
            }
            break;
        case PrMarkdownNodeKind_FootnoteReference: {
            if (!enter)
                break;
            Tag sup = startTag(w, "sup");
            attribute(w, &sup, "class", "footnote-ref");
            writeTag(w, sup);
            Tag tag = startTag(w, "a");
            urlAttribute(w, &tag, "href", "#fn-", step->label);
            char referenceText[16];
            TextBuffer id;
            textAppend(&id, "fnref-");
            encodeUrl(step->label, &id);
            if (step->reference > 1) {
                textAppendChar(&id, '-');
                textAppend(&id, decimal(referenceText, step->reference));
            }
            attribute(w, &tag, "id", textView(&id));
            attribute(w, &tag, "data-footnote-ref", PrStringView{});
            writeTag(w, tag);
            textReset(&id);
            char numberText[16];
            text(w, decimal(numberText, step->number));
            end(w, "a");
            end(w, "sup");
            break;
        }
        case PrMarkdownNodeKind_FootnoteDefinition:
            if (enter) {
                Tag tag = startTag(w, "li");
                urlAttribute(w, &tag, "id", "fn-", step->label);
                writeTag(w, tag);
                text(w, "\n");
                w->footnoteDepth = w->depth;
                w->paragraphWaits = false;
                break;
            }
            w->footnoteDepth = 0;
            if (w->paragraphWaits) {
                w->paragraphWaits = false;
                text(w, " ");
                writeBackReferences(w, step);
                end(w, "p");
                text(w, "\n");
            } else {
                writeBackReferences(w, step);
                text(w, "\n");
            }
            end(w, "li");
            text(w, "\n");
            break;
        default:
            break;
    }
}

void writerReset(Writer* w) {
    textReset(&w->values);
    textReset(&w->description);
}

// ---------------------------------------------------------------------------------------------------------------------
// HTML text

void escape(TextBuffer* out, PrStringView characters, bool url) {
    size_t run = 0;
    for (size_t i = 0; i < characters.size; ++i) {
        const char* reference = nullptr;
        switch (characters.data[i]) {
            case '&':
                reference = "&amp;";
                break;
            case '<':
                reference = "&lt;";
                break;
            case '>':
                reference = "&gt;";
                break;
            case '"':
                reference = "&quot;";
                break;
            case '\'':
                // A URL writes its quote as a reference, as the specification's examples do.
                if (!url)
                    continue;
                reference = "&#x27;";
                break;
            default:
                continue;
        }
        textAppend(out, characters.data + run, uint32_t(i - run));
        textAppend(out, reference);
        run = i + 1;
    }
    textAppend(out, characters.data + run, uint32_t(characters.size - run));
}

void writeText(void* PR_NULLABLE context, const PrMarkdownHtmlEvent* event) {
    TextBuffer* out = static_cast<TextBuffer*>(context);
    switch (event->kind) {
        case PrMarkdownHtmlEventKind_StartTag:
            textAppendChar(out, '<');
            textAppend(out, event->name);
            for (uint32_t i = 0; i < event->attributeCount; ++i) {
                const PrMarkdownHtmlAttribute& attribute = event->attributes[i];
                textAppendChar(out, ' ');
                textAppend(out, attribute.name);
                if (attribute.value.data == nullptr)
                    continue;
                textAppend(out, "=\"");
                // A URL, and the URL fragment that a footnote's id is, write their quotes as references.
                bool url = attribute.name == "href" || attribute.name == "src" || attribute.name == "id";
                escape(out, attribute.value, url);
                textAppendChar(out, '"');
            }
            textAppend(out, event->selfClosing ? " />" : ">");
            break;
        case PrMarkdownHtmlEventKind_EndTag:
            textAppend(out, "</");
            textAppend(out, event->name);
            textAppendChar(out, '>');
            break;
        case PrMarkdownHtmlEventKind_Text:
            escape(out, event->text, false);
            break;
        case PrMarkdownHtmlEventKind_Html:
            textAppend(out, event->text);
            break;
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

void prMarkdownWriteHtml(const PrMarkdown* document, uint32_t first, uint32_t count,
        PrMarkdownHtmlFn fn, void* PR_NULLABLE context) {
    Writer w{};
    w.fn = fn;
    w.context = context;
    w.atLineStart = true;
    prMarkdownWalk(document, first, count, writeStep, &w);
    writerReset(&w);
}

void prMarkdownWriteFootnotesHtml(const PrMarkdown* document, PrMarkdownHtmlFn fn, void* PR_NULLABLE context) {
    // The section is written only if a footnote is walked.
    struct Section {
        Writer writer;
        bool open;
    };
    Section section{};
    section.writer.fn = fn;
    section.writer.context = context;
    section.writer.atLineStart = true;
    prMarkdownWalkFootnotes(document, [](void* PR_NULLABLE sectionContext, const PrMarkdownStep* step) {
        Section* s = static_cast<Section*>(sectionContext);
        if (!s->open) {
            s->open = true;
            Writer* w = &s->writer;
            Tag tag = startTag(w, "section");
            attribute(w, &tag, "class", "footnotes");
            attribute(w, &tag, "data-footnotes", PrStringView{});
            writeTag(w, tag);
            text(w, "\n");
            start(w, "ol");
            text(w, "\n");
        }
        writeStep(&s->writer, step);
    }, &section);
    if (section.open) {
        end(&section.writer, "ol");
        text(&section.writer, "\n");
        end(&section.writer, "section");
        text(&section.writer, "\n");
    }
    writerReset(&section.writer);
}

void prMarkdownHtmlEventAppendText(const PrMarkdownHtmlEvent* event, PrString* out) {
    TextBuffer text;
    writeText(&text, event);
    prStringAppend(out, textView(&text));
    textReset(&text);
}

void prMarkdownEncodeUrl(PrStringView url, PrString* out) {
    TextBuffer encoded;
    encodeUrl(url, &encoded);
    prStringAppend(out, textView(&encoded));
    textReset(&encoded);
}

void prMarkdownAppendHtml(const PrMarkdown* document, uint32_t first, uint32_t count, PrString* out) {
    TextBuffer text;
    prMarkdownWriteHtml(document, first, count, writeText, &text);
    prStringAppend(out, textView(&text));
    textReset(&text);
}

void prMarkdownAppendFootnotesHtml(const PrMarkdown* document, PrString* out) {
    TextBuffer text;
    prMarkdownWriteFootnotesHtml(document, writeText, &text);
    prStringAppend(out, textView(&text));
    textReset(&text);
}
