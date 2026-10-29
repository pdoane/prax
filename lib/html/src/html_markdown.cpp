// Markdown in the DOM: a Markdown document's blocks built into a document or into an element.

#include "pr/html_markdown.h"
#include "html/src/html_markdown.h"

#include "pr/alloc.h"
#include "pr/markdown_html.h"
#include "pr/memory.h"
#include "pr/string_ops.h"

#include "dom/src/dom_node.h"

namespace pr_impl {

namespace {

// The length of a last child that is not a Text node.
constexpr uint32_t kNotText = 0xffffffffu;

// The HTML that starts a document whose body holds the blocks. The doctype keeps the document out of quirks mode.
constexpr PrStringView kDocumentStart = "<!DOCTYPE html><html><head></head><body>";

// Feeds one event of lib/markdown's HTML to the parse. Tags and text become tokens that the builder makes while the
// tokenizer is idle, and HTML text that the tokenizer reads otherwise. Raw HTML is always the tokenizer's to read.
void feed(void* PR_NULLABLE context, const PrMarkdownHtmlEvent* event) {
    HtmlMarkdownBuilder* b = static_cast<HtmlMarkdownBuilder*>(context);
    HtmlTokenizer* tokenizer = &b->parser.tokenizer;
    bool tag = event->kind == PrMarkdownHtmlEventKind_StartTag || event->kind == PrMarkdownHtmlEventKind_EndTag;
    if (event->kind == PrMarkdownHtmlEventKind_Html || !htmlTokenizerIsIdle(tokenizer)) {
        prStringReplace(&b->html, "");
        prMarkdownHtmlEventAppendText(event, &b->html);
        PrStringView html = prStringView(&b->html);
        if (html.size != 0)
            htmlParserAppend(&b->parser, html);
        return;
    }
    if (!tag) {
        if (event->text.size == 0)
            return;
        htmlTokenizerMakeCharacters(tokenizer, event->text);
        htmlParserProcess(&b->parser);
        return;
    }
    bool start = event->kind == PrMarkdownHtmlEventKind_StartTag;
    htmlTokenizerMakeTag(tokenizer, start ? HtmlTokenKind_StartTag : HtmlTokenKind_EndTag, event->name);
    if (event->selfClosing)
        tokenizer->token.flags |= HtmlTokenFlag_SelfClosing;
    for (uint32_t i = 0; start && i < event->attributeCount; ++i) {
        const PrMarkdownHtmlAttribute& attribute = event->attributes[i];
        htmlTokenizerAddAttribute(tokenizer, attribute.name, attribute.value.data != nullptr ? attribute.value : "");
    }
    htmlParserProcess(&b->parser);
}

// ---------------------------------------------------------------------------------------------------------------------
// Clean points

void releaseCleanPoints(HtmlMarkdownBuilder* b, uint32_t from) {
    for (uint32_t i = from; i < b->cleanPoints.count; ++i) {
        for (uint32_t t = 0; t < b->targetCount; ++t)
            domNodeRelease(b->cleanPoints.data[i].lastChildren[t]);
    }
    b->cleanPoints.resize(from);
}

uint32_t textLengthOf(const PrDomNode* PR_NULLABLE node) {
    if (node == nullptr || node->type != PrDomNodeType_Text)
        return kNotText;
    return domCharacterDataLength(static_cast<const PrDomCharacterData*>(node));
}

// Records a clean point before `block` if the parse is at rest.
void recordCleanPoint(HtmlMarkdownBuilder* b, uint32_t block) {
    if (!htmlParserIsAtRest(&b->parser, b->base))
        return;
    // The input that the tokenizer has read is not needed again.
    HtmlParserRest rest = b->base;
    rest.framesetOk = b->parser.framesetOk;
    htmlParserRestore(&b->parser, rest);
    HtmlMarkdownCleanPoint& point = b->cleanPoints.emplace();
    point = HtmlMarkdownCleanPoint{};
    point.block = block;
    point.framesetOk = b->parser.framesetOk;
    for (uint32_t t = 0; t < b->targetCount; ++t) {
        PrDomNode* last = b->targets[t]->last;
        if (last != nullptr)
            domNodeRetain(last);
        point.lastChildren[t] = last;
        point.lastTextLengths[t] = textLengthOf(last);
    }
}

// Returns the DOM and the parse to the clean point at `index`, and returns false if the DOM before the point has
// changed in a way that the point cannot restore.
bool restoreCleanPoint(HtmlMarkdownBuilder* b, uint32_t index) {
    const HtmlMarkdownCleanPoint& point = b->cleanPoints.data[index];
    for (uint32_t t = 0; t < b->targetCount; ++t) {
        PrDomNode* last = point.lastChildren[t];
        if (last != nullptr && last->parent != b->targets[t])
            return false;
    }
    for (uint32_t t = 0; t < b->targetCount; ++t) {
        PrDomNode* target = b->targets[t];
        while (target->last != point.lastChildren[t])
            domRemove(target->last);
        uint32_t length = textLengthOf(point.lastChildren[t]);
        if (length != kNotText && length > point.lastTextLengths[t])
            domReplaceData(static_cast<PrDomCharacterData*>(point.lastChildren[t]), point.lastTextLengths[t],
                           length - point.lastTextLengths[t], "");
    }
    HtmlParserRest rest = b->base;
    rest.framesetOk = point.framesetOk;
    htmlParserRestore(&b->parser, rest);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Building

// Starts the parse into the builder's document or element, as it is before any block.
void startParse(HtmlMarkdownBuilder* b) {
    htmlParserReset(&b->parser);
    if (b->element != nullptr) {
        PrDomElement* element = b->element;
        htmlParserInit(&b->parser, element->document, b->flags);
        htmlParserOpen(&b->parser);
        htmlParserBeginFragment(&b->parser, element);
        b->parser.rootTarget = element;
        b->targets[0] = element;
        b->targetCount = 1;
    } else {
        htmlParserInit(&b->parser, b->document, b->flags);
        htmlParserOpen(&b->parser);
        htmlParserAppend(&b->parser, kDocumentStart);
        b->targets[0] = b->document;
        b->targets[1] = b->parser.open.data[0];
        b->targets[2] = b->parser.open.data[1];
        b->targetCount = 3;
    }
    b->base = htmlParserRest(&b->parser);
    b->built = 0;
}

// Removes what the builder built, and starts the parse again.
void restart(HtmlMarkdownBuilder* b) {
    releaseCleanPoints(b, 0);
    if (b->element != nullptr) {
        domReplaceAll(b->element, nullptr);
    } else {
        while (b->document->last != nullptr)
            domRemove(b->document->last);
    }
    startParse(b);
}

// Builds the blocks from `first` on, and then the footnotes. The footnotes follow the blocks, as lib/markdown's HTML of
// a whole document writes them, and are parsed from that HTML. A clean point before them lets a change to the footnotes
// alone rebuild only them.
void buildFrom(HtmlMarkdownBuilder* b, uint32_t first) {
    uint32_t count = prMarkdownBlockCount(b->markdown);
    for (uint32_t block = first; block <= count; ++block) {
        recordCleanPoint(b, block);
        if (block < count)
            prMarkdownWriteHtml(b->markdown, block, 1, feed, b);
    }
    b->built = count;
    prMarkdownWriteFootnotesHtml(b->markdown, feed, b);
}

// Builds the blocks that changed since the last update, from the last clean point before the first of them.
void update(HtmlMarkdownBuilder* b) {
    uint32_t first = 0xffffffffu;
    PrMarkdownChange changes[8];
    uint32_t count = 0;
    while ((count = prMarkdownTakeChanges(b->markdown, changes, 8)) != 0) {
        for (uint32_t i = 0; i < count; ++i)
            first = changes[i].first < first ? changes[i].first : first;
    }
    // A change to the footnotes alone rebuilds from the clean point before them.
    if (prMarkdownTakeFootnotesChanged(b->markdown) && first > b->built)
        first = b->built;
    if (first == 0xffffffffu)
        return;
    uint32_t point = b->cleanPoints.count;
    while (point != 0 && b->cleanPoints.data[point - 1].block > first)
        --point;
    if (point == 0 || !restoreCleanPoint(b, point - 1)) {
        restart(b);
        buildFrom(b, 0);
        return;
    }
    uint32_t block = b->cleanPoints.data[point - 1].block;
    releaseCleanPoints(b, point - 1);
    buildFrom(b, block);
}

void initBuilder(HtmlMarkdownBuilder* b, const PrMarkdownOptions* PR_NULLABLE options) {
    b->markdown = prMarkdownCreate(options);
    startParse(b);
}

} // namespace

void htmlMarkdownBuilderInitDocument(HtmlMarkdownBuilder* builder, PrDomDocument* document, HtmlParseFlags flags,
                                     const PrMarkdownOptions* PR_NULLABLE options) {
    *builder = HtmlMarkdownBuilder{};
    domNodeRetain(document);
    builder->document = document;
    builder->flags = HtmlParseFlags(flags & ~HtmlParseFlag_Scripting);
    initBuilder(builder, options);
}

void htmlMarkdownBuilderInitElement(HtmlMarkdownBuilder* builder, PrDomElement* element,
                                    const PrMarkdownOptions* PR_NULLABLE options) {
    *builder = HtmlMarkdownBuilder{};
    domNodeRetain(element);
    builder->element = element;
    domReplaceAll(element, nullptr);
    initBuilder(builder, options);
}

void htmlMarkdownBuilderAppend(HtmlMarkdownBuilder* builder, PrStringView text) {
    prMarkdownAppend(builder->markdown, text);
    update(builder);
}

void htmlMarkdownBuilderEnd(HtmlMarkdownBuilder* builder) {
    prMarkdownEnd(builder->markdown);
    update(builder);
    htmlParserClose(&builder->parser);
}

void htmlMarkdownBuilderReset(HtmlMarkdownBuilder* builder) {
    releaseCleanPoints(builder, 0);
    builder->cleanPoints.reset();
    htmlParserReset(&builder->parser);
    prMarkdownDestroy(builder->markdown);
    builder->markdown = nullptr;
    prStringReset(&builder->html);
    domNodeRelease(builder->element);
    domNodeRelease(builder->document);
    builder->element = nullptr;
    builder->document = nullptr;
}

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrHtmlMarkdownStream : HtmlMarkdownBuilder {};

void prHtmlSetMarkdown(PrDomElement* element, PrStringView markdown, const PrMarkdownOptions* PR_NULLABLE options) {
    HtmlMarkdownBuilder builder;
    htmlMarkdownBuilderInitElement(&builder, element, options);
    htmlMarkdownBuilderAppend(&builder, markdown);
    htmlMarkdownBuilderEnd(&builder);
    htmlMarkdownBuilderReset(&builder);
}

PrHtmlMarkdownStream* prHtmlMarkdownStreamCreate(PrDomElement* element, const PrMarkdownOptions* PR_NULLABLE options) {
    PrHtmlMarkdownStream* stream = allocZeroed<PrHtmlMarkdownStream>();
    htmlMarkdownBuilderInitElement(stream, element, options);
    return stream;
}

void prHtmlMarkdownStreamDestroy(PrHtmlMarkdownStream* PR_NULLABLE stream) {
    if (stream == nullptr)
        return;
    htmlMarkdownBuilderReset(stream);
    prMemoryFree(stream);
}

void prHtmlMarkdownStreamAppend(PrHtmlMarkdownStream* stream, PrStringView text) {
    htmlMarkdownBuilderAppend(stream, text);
}

void prHtmlMarkdownStreamEnd(PrHtmlMarkdownStream* stream) {
    htmlMarkdownBuilderEnd(stream);
}
