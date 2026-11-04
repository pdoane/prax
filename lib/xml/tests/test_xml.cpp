#include "pr/string_ops.h"
#include "pr/xml.h"

#include "tools/harness/test.h"

#include <string>
#include <vector>

namespace pr_impl {
namespace {

std::string text(PrStringView view) {
    return std::string(view.data != nullptr ? view.data : "", view.size);
}

// Writes what a parse reports as a tree, one line per node, indented two spaces per level. An element is shown as its
// namespace URL and qualified name, followed by its attributes. Each other node is shown as its kind and data.
struct Recorder {
    std::string out;
    int depth = 0;

    void line(const std::string& text) {
        out += std::string(size_t(depth) * 2, ' ') + text + "\n";
    }
};

std::string qualified(const PrXmlName& name) {
    return (name.prefix.data != nullptr ? text(name.prefix) + ":" : "") + text(name.local);
}

PrStatus recordStart(void* context, PrXmlParser*, const PrXmlName* name, const PrXmlAttribute* attributes, uint32_t count) {
    Recorder* r = static_cast<Recorder*>(context);
    std::string line = "<" + text(name->space) + " " + qualified(*name) + ">";
    for (uint32_t i = 0; i < count; ++i)
        line += " " + text(attributes[i].name.space) + " " + qualified(attributes[i].name) + "=" +
            text(attributes[i].value);
    r->line(line);
    ++r->depth;
    return PrStatus_Ok;
}

PrStatus recordEnd(void* context, PrXmlParser*, const PrXmlName*) {
    --static_cast<Recorder*>(context)->depth;
    return PrStatus_Ok;
}

PrStatus recordText(void* context, PrXmlParser*, PrStringView data) {
    static_cast<Recorder*>(context)->line("\"" + text(data) + "\"");
    return PrStatus_Ok;
}

PrStatus recordCdata(void* context, PrXmlParser*, PrStringView data) {
    static_cast<Recorder*>(context)->line("<![CDATA[" + text(data) + "]]>");
    return PrStatus_Ok;
}

PrStatus recordComment(void* context, PrXmlParser*, PrStringView data) {
    static_cast<Recorder*>(context)->line("<!--" + text(data) + "-->");
    return PrStatus_Ok;
}

PrStatus recordInstruction(void* context, PrXmlParser*, PrStringView target, PrStringView data) {
    static_cast<Recorder*>(context)->line("<?" + text(target) + " " + text(data) + "?>");
    return PrStatus_Ok;
}

PrStatus recordDoctype(void* context, PrXmlParser*, PrStringView name, PrStringView publicId, PrStringView systemId) {
    static_cast<Recorder*>(context)->line("<!DOCTYPE " + text(name) + " \"" + text(publicId) + "\" \"" + text(systemId) + "\">");
    return PrStatus_Ok;
}

// An external subset that holds one entity.
PrStringView testEntity(void*, PrStringView publicId, PrStringView name) {
    bool known = publicId == "-//test//EN" && name == "nbsp";
    return known ? "\xC2\xA0" : PrStringView{};
}

// A handler that reports every call to `recorder`.
PrXmlHandler recordingHandler(Recorder* recorder) {
    return PrXmlHandler{
        .context = recorder,
        .startElement = recordStart,
        .endElement = recordEnd,
        .text = recordText,
        .cdataSection = recordCdata,
        .comment = recordComment,
        .processingInstruction = recordInstruction,
        .doctype = recordDoctype,
        .entity = testEntity,
    };
}

// Returns the tree that a parse recorded, or the parser's error and its position.
std::string outcome(const Recorder& recorder, PrStatus status, const PrXmlError& error) {
    if (status == PrStatus_Ok)
        return recorder.out;
    return "error " + std::to_string(error.line) + ":" + std::to_string(error.column) + " " + text(error.message);
}

// Returns what `source` parses to whole.
std::string parse(const std::string& source) {
    Recorder recorder;
    PrXmlHandler handler = recordingHandler(&recorder);
    PrXmlError error = {};
    PrStatus status = prXmlParse(source, &handler, &error);
    return outcome(recorder, status, error);
}

// Returns what `source` parses to when its input arrives in the pieces that `breaks`, offsets in increasing order,
// divide it into, with a run after each piece.
std::string parsePieces(const std::string& source, const std::vector<size_t>& breaks) {
    Recorder recorder;
    PrXmlHandler handler = recordingHandler(&recorder);
    PrXmlParser* parser = prXmlParserCreate(&handler);
    PrStatus status = PrStatus_Ok;
    size_t start = 0;
    for (size_t i = 0; i <= breaks.size() && status == PrStatus_Ok; ++i) {
        size_t end = i < breaks.size() ? breaks[i] : source.size();
        prXmlParserAppend(parser, PrStringView{source.data() + start, end - start});
        start = end;
        if (i == breaks.size())
            prXmlParserEnd(parser);
        status = prXmlParserRun(parser);
    }
    CHECK(prXmlParserIsEnded(parser));
    std::string out = outcome(recorder, status, prXmlParserError(parser));
    prXmlParserDestroy(parser);
    return out;
}

// A handler that finds the document wrong at its first element, and one that stops at its first element's end with a
// status of its own.
PrStatus refuseStart(void*, PrXmlParser* parser, const PrXmlName*, const PrXmlAttribute*, uint32_t) {
    return prXmlParserFail(parser, "refused");
}

PrStatus stopAtEnd(void* context, PrXmlParser* parser, const PrXmlName*) {
    *static_cast<PrXmlPosition*>(context) = prXmlParserContentStart(parser);
    return PrStatus_NotAllowed;
}

// Records the end of an element, and pauses the parse at the end of each element named s.
PrStatus pauseAtS(void* context, PrXmlParser* parser, const PrXmlName* name) {
    if (text(name->local) == "s")
        prXmlParserPause(parser);
    return recordEnd(context, parser, name);
}

} // namespace
} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("elements take the namespace their prefix or the default is bound to") {
    CHECK(parse("<svg xmlns='http://www.w3.org/2000/svg' xmlns:h='http://www.w3.org/1999/xhtml'>"
        "<h:p a='1' h:b='2'/><g/></svg>") ==
        "<http://www.w3.org/2000/svg svg> http://www.w3.org/2000/xmlns/ xmlns=http://www.w3.org/2000/svg "
        "http://www.w3.org/2000/xmlns/ xmlns:h=http://www.w3.org/1999/xhtml\n"
        "  <http://www.w3.org/1999/xhtml h:p>  a=1 http://www.w3.org/1999/xhtml h:b=2\n"
        "  <http://www.w3.org/2000/svg g>\n");
    CHECK(parse("<a xmlns='urn:x'><b xmlns=''/></a>") ==
        "<urn:x a> http://www.w3.org/2000/xmlns/ xmlns=urn:x\n  < b> http://www.w3.org/2000/xmlns/ xmlns=\n");
}

TEST_CASE("text, CDATA sections, comments and processing instructions are reported with their data") {
    CHECK(parse("<?xml version='1.0'?>\r\n<!--c--><?pi data?><r>a&lt;&#x41;&#66;\r<![CDATA[<x>]]></r>") ==
        "<!--c-->\n<?pi data?>\n< r>\n  \"a<AB\n\"\n  <![CDATA[<x>]]>\n");
}

TEST_CASE("the internal subset's entities expand, markup and all, and an external subset's resolve") {
    CHECK(parse("<!DOCTYPE r [<!ENTITY e \"<b>&#65;</b>\"> <!ELEMENT r ANY>]><r a='&e;'>&e;</r>") ==
        "error 1:65 an attribute value holds a '<'");
    CHECK(parse("<!DOCTYPE r [<!ENTITY e \"<b>&#65;</b>\">]><r>&e;&e;</r>") ==
        "<!DOCTYPE r \"\" \"\">\n< r>\n  < b>\n    \"A\"\n  < b>\n    \"A\"\n");
    CHECK(parse("<!DOCTYPE p PUBLIC \"-//test//EN\" \"x\"><p>&nbsp;</p>") ==
        "<!DOCTYPE p \"-//test//EN\" \"x\">\n< p>\n  \"\xC2\xA0\"\n");
    CHECK(parse("<p>&nbsp;</p>") == "error 1:4 an entity is referred to that is not declared");
    CHECK(parse("<!DOCTYPE r [<!ENTITY a \"&b;\"><!ENTITY b \"&a;\">]><r>&a;</r>") == "error 1:53 an entity refers to itself");
}

TEST_CASE("a document that is not well-formed is an error where it goes wrong") {
    CHECK(parse("<a><b></a>") == "error 1:11 an end tag does not match the element it closes");
    CHECK(parse("<a/><b/>") == "error 1:6 an element follows the document element");
    CHECK(parse("<p:a/>") == "error 1:7 an element's prefix is not bound to a namespace");
    CHECK(parse("<a x='1' x='2'/>") == "error 1:15 an attribute is given twice");
    CHECK(parse("<a xmlns:p='u' xmlns:q='u' p:x='1' q:x='2'/>") == "error 1:45 an attribute is given twice in one namespace");
    CHECK(parse("text<a/>") == "error 1:1 text is outside the document element");
    CHECK(parse("<a>]]></a>") == "error 1:4 text holds \"]]>\"");
    CHECK(parse("<a><!-- a -- b --></a>") == "error 1:11 a comment holds \"--\"");
    CHECK(parse("<a>") == "error 1:4 an element is not closed");
    CHECK(parse("") == "error 1:1 the document has no document element");
}

TEST_CASE("a handler can find a document wrong, or stop its parse") {
    PrXmlHandler refusing = {.startElement = refuseStart};
    PrXmlError error = {};
    CHECK(prXmlParse("<a/>", &refusing, &error) == PrStatus_Syntax);
    CHECK(text(error.message) == "refused");
    CHECK(error.line == 1);
    CHECK(error.column == 5);

    PrXmlPosition content = {};
    PrXmlHandler stopping = {.context = &content, .endElement = stopAtEnd};
    CHECK(prXmlParse("<a>\n  <b>x</b></a>", &stopping, &error) == PrStatus_NotAllowed);
    CHECK(error.message.data == nullptr);
    CHECK(content.line == 2);
    CHECK(content.column == 6);
}

TEST_CASE("a document whose input arrives in pieces parses as it does whole, wherever the pieces divide it") {
    const std::string documents[] = {
        "\xEF\xBB\xBF<?xml version='1.0' encoding='utf-8'?>\r\n<!--c--><?pi data?>"
            "<r a='x>y' b=\"'\">a&lt;&#x41;&#66;\r\n<![CDATA[<x>]]]]>]\r</r>",
        "<!DOCTYPE r [<!ENTITY e \"<b>&#65;</b>\"> <!-- ]> --> <?pi ]>?> %p; <!ATTLIST r a CDATA '>'>]><r>&e;&e;</r>",
        "<!DOCTYPE p PUBLIC \"-//test//EN\" \"x\"><p>&nbsp;\xC3\xA9</p>",
        "<a><b></a>",
        "<a>]]></a>",
        "<a><!-- a -- b --></a>",
        "<a>&amp</a>",
        "<a x='1",
        "<a>",
        "",
    };
    for (const std::string& document : documents) {
        std::string whole = parse(document);
        for (size_t at = 0; at <= document.size(); ++at)
            CHECK_MESSAGE(parsePieces(document, {at}) == whole, document << " divided at " << at);
        std::vector<size_t> bytes;
        for (size_t at = 1; at < document.size(); ++at)
            bytes.push_back(at);
        CHECK_MESSAGE(parsePieces(document, bytes) == whole, document << " in bytes");
    }
}

TEST_CASE("a parse waits for the rest of a piece of content before it reports the piece") {
    Recorder recorder;
    PrXmlHandler handler = recordingHandler(&recorder);
    PrXmlParser* parser = prXmlParserCreate(&handler);
    prXmlParserAppend(parser, "<r><a x='>");
    CHECK(prXmlParserRun(parser) == PrStatus_Ok);
    CHECK(recorder.out == "< r>\n");
    prXmlParserAppend(parser, "'/>text<!-- c -");
    CHECK(prXmlParserRun(parser) == PrStatus_Ok);
    CHECK(recorder.out == "< r>\n  < a>  x=>\n");
    prXmlParserAppend(parser, "-></r>");
    CHECK(prXmlParserRun(parser) == PrStatus_Ok);
    CHECK_FALSE(prXmlParserIsEnded(parser));
    prXmlParserEnd(parser);
    CHECK(prXmlParserRun(parser) == PrStatus_Ok);
    CHECK(prXmlParserIsEnded(parser));
    CHECK(recorder.out == "< r>\n  < a>  x=>\n  \"text\"\n  <!-- c -->\n");
    prXmlParserDestroy(parser);
}

TEST_CASE("a handler pauses a parse, which the next run continues where it paused") {
    Recorder recorder;
    PrXmlHandler handler = recordingHandler(&recorder);
    handler.endElement = pauseAtS;
    PrXmlParser* parser = prXmlParserCreate(&handler);
    // A pause in an entity's expansion takes effect once the expansion ends.
    prXmlParserAppend(parser, "<!DOCTYPE r [<!ENTITY e \"<s/><t/>\">]><r><s/><u/>&e;<v/></r>");
    prXmlParserEnd(parser);
    CHECK(prXmlParserRun(parser) == PrStatus_Ok);
    CHECK(prXmlParserIsPaused(parser));
    CHECK(recorder.out == "<!DOCTYPE r \"\" \"\">\n< r>\n  < s>\n");
    CHECK(prXmlParserRun(parser) == PrStatus_Ok);
    CHECK(recorder.out == "<!DOCTYPE r \"\" \"\">\n< r>\n  < s>\n  < u>\n  < s>\n  < t>\n");
    CHECK_FALSE(prXmlParserIsEnded(parser));
    CHECK(prXmlParserRun(parser) == PrStatus_Ok);
    CHECK_FALSE(prXmlParserIsPaused(parser));
    CHECK(prXmlParserIsEnded(parser));
    CHECK(recorder.out == "<!DOCTYPE r \"\" \"\">\n< r>\n  < s>\n  < u>\n  < s>\n  < t>\n  < v>\n");
    prXmlParserDestroy(parser);
}

TEST_CASE("the start of a document's bytes decides its declared charset once its XML declaration has arrived") {
    PrCharset charset = PrCharset_None;
    CHECK_FALSE(prXmlDeclaredCharset("<?xm", false, &charset));
    CHECK_FALSE(prXmlDeclaredCharset("<?xml version='1.0' encoding='koi8-r'", false, &charset));
    CHECK_FALSE(prXmlDeclaredCharset(PrStringView{"<\0?", 3}, false, &charset));
    CHECK(prXmlDeclaredCharset("<?xml version='1.0' encoding='koi8-r'?>", false, &charset));
    CHECK(charset == PrCharset_Koi8R);
    CHECK(prXmlDeclaredCharset(PrStringView{"<\0?\0", 4}, false, &charset));
    CHECK(charset == PrCharset_Utf16Le);
    CHECK(prXmlDeclaredCharset("<?xml-stylesheet href='a'?>", false, &charset));
    CHECK(charset == PrCharset_None);
    CHECK(prXmlDeclaredCharset("<r", false, &charset));
    CHECK(charset == PrCharset_None);
    CHECK(prXmlDeclaredCharset("<?xm", true, &charset));
    CHECK(charset == PrCharset_None);
    CHECK(prXmlDeclaredCharset("<?xml version='1.0' encoding='utf-16'?>", true, &charset));
    CHECK(charset == PrCharset_Utf8);
}

TEST_CASE("names follow XML's Name production") {
    CHECK(prXmlIsName("a"));
    CHECK(prXmlIsName("_a-1.b:c"));
    CHECK(prXmlIsName("\xC3\xA9t\xC3\xA9"));
    CHECK(!prXmlIsName(""));
    CHECK(!prXmlIsName("1a"));
    CHECK(!prXmlIsName("a b"));
    CHECK(!prXmlIsName("-a"));
}
