#include "pr/string_ops.h"

#include "html/src/html_tokenizer.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

namespace pr_impl {

namespace {

// A tokenizer over `text`, in the state that the tree builder would have set.
struct Reader {
    std::string source;
    HtmlTokenizer tokenizer = {};
    RawArray<HtmlError> errors;

    explicit Reader(std::string text, HtmlState state = HtmlState_Data, PrStringView lastStartTag = PrStringView{}) :
            source(std::move(text)) {
        htmlTokenizerStart(&tokenizer, source, &errors);
        htmlTokenizerSetState(&tokenizer, state, lastStartTag);
    }

    ~Reader() {
        htmlTokenizerReset(&tokenizer);
        errors.reset();
    }

    // Reads the next token, and returns true if it was anything but the end of the input.
    bool next() {
        htmlTokenizerNext(&tokenizer);
        return tokenizer.token.kind != HtmlTokenKind_Eof;
    }

    std::string text(HtmlSpan span) const {
        PrStringView bytes = htmlSpanText(&tokenizer, span);
        return std::string(bytes.data, bytes.size);
    }

    // Returns the current input, which is the stream that the tokenizer reads.
    std::string input() const {
        PrStringView bytes = htmlInputText(&tokenizer);
        return std::string(bytes.data, bytes.size);
    }
};

// Appends one token, with adjacent characters joined as both corpora write them.
void append(std::vector<std::string>* out, const Reader& reader) {
    const HtmlToken& token = reader.tokenizer.token;
    switch (token.kind) {
        case HtmlTokenKind_Character:
            if (!out->empty() && out->back().compare(0, 6, "chars ") == 0)
                out->back() += reader.text(token.data);
            else
                out->push_back("chars " + reader.text(token.data));
            return;

        case HtmlTokenKind_StartTag:
        case HtmlTokenKind_EndTag: {
            std::string entry = token.kind == HtmlTokenKind_StartTag ? "start " : "end ";
            entry += reader.text(token.name);
            for (uint32_t i = 0; i < reader.tokenizer.attributes.count; ++i) {
                const HtmlAttribute& attribute = reader.tokenizer.attributes.data[i];
                entry += " " + reader.text(attribute.name) + "=" + reader.text(attribute.value);
            }
            if ((token.flags & HtmlTokenFlag_SelfClosing) != 0)
                entry += " /";
            out->push_back(entry);
            return;
        }

        case HtmlTokenKind_Comment:
            out->push_back("comment " + reader.text(token.data));
            return;

        case HtmlTokenKind_ProcessingInstruction:
            out->push_back("pi " + reader.text(token.name) + " " + reader.text(token.data));
            return;

        case HtmlTokenKind_Doctype: {
            std::string entry = "doctype ";
            entry += (token.flags & HtmlTokenFlag_NoName) != 0 ? "-" : reader.text(token.name);
            if ((token.flags & HtmlTokenFlag_NoPublicId) == 0)
                entry += " public=" + reader.text(token.publicId);
            if ((token.flags & HtmlTokenFlag_NoSystemId) == 0)
                entry += " system=" + reader.text(token.systemId);
            if ((token.flags & HtmlTokenFlag_ForceQuirks) != 0)
                entry += " quirks";
            out->push_back(entry);
            return;
        }

        case HtmlTokenKind_Eof:
            return;
    }
}

std::string join(const std::vector<std::string>& parts) {
    std::string out;
    for (const std::string& part : parts) {
        if (!out.empty())
            out += " | ";
        out += part;
    }
    return out;
}

// Returns every token in `text`, read in `state` for an element named `lastStartTag`.
std::string tokens(std::string text, HtmlState state = HtmlState_Data, PrStringView lastStartTag = PrStringView{}) {
    Reader reader(std::move(text), state, lastStartTag);
    std::vector<std::string> out;
    while (reader.next())
        append(&out, reader);
    return join(out);
}

// Returns every character token in `text`, one entry each, with the flags that it carries, which the tree builder
// branches on.
std::string runs(std::string text, HtmlState state = HtmlState_Data, PrStringView lastStartTag = PrStringView{}) {
    Reader reader(std::move(text), state, lastStartTag);
    std::vector<std::string> out;
    while (reader.next()) {
        const HtmlToken& token = reader.tokenizer.token;
        if (token.kind != HtmlTokenKind_Character)
            continue;

        std::string entry = reader.text(token.data);
        if ((token.flags & HtmlTokenFlag_Whitespace) != 0)
            entry += " space";
        if ((token.flags & HtmlTokenFlag_Text) != 0)
            entry += " text";
        if ((token.flags & HtmlTokenFlag_Null) != 0)
            entry += " null";
        out.push_back(entry);
    }
    return join(out);
}

// Returns the same as tokens, for a tokenizer whose adjusted current node is a foreign element.
std::string foreignTokens(std::string text) {
    Reader reader(std::move(text));
    reader.tokenizer.foreign = true;
    std::vector<std::string> out;
    while (reader.next())
        append(&out, reader);
    return join(out);
}

// Returns the parse errors that `text` reports, each as its name and the byte offset at which it was reported.
std::string errorsOf(std::string text, HtmlState state = HtmlState_Data) {
    Reader reader(std::move(text), state);
    while (reader.next()) {
    }

    std::vector<std::string> out;
    for (uint32_t i = 0; i < reader.errors.count; ++i) {
        PrStringView name = kHtmlErrorName[reader.errors.data[i].code];
        out.push_back(std::string(name.data, name.size) + "@" + std::to_string(reader.errors.data[i].at));
    }
    return join(out);
}

// Returns the column of `to` on its line, counting from the start of `text`.
uint32_t columnAt(const std::string& text, uint32_t to) {
    uint32_t at = 0;
    uint32_t line = 1;
    uint32_t column = 1;
    htmlInputPosition(text, &at, to, &line, &column);
    return column;
}

uint32_t lineAt(const std::string& text, uint32_t to) {
    uint32_t at = 0;
    uint32_t line = 1;
    uint32_t column = 1;
    htmlInputPosition(text, &at, to, &line, &column);
    return line;
}

// Returns the tokens of `text` written to an open input one byte at a time and then closed. The tokenizer reads a token
// only once its current input determines it.
std::string written(const std::string& text) {
    Reader reader("");
    reader.tokenizer.open = true;
    std::vector<std::string> out;
    auto drain = [&] {
        while (htmlTokenizerNext(&reader.tokenizer) && reader.tokenizer.token.kind != HtmlTokenKind_Eof)
            append(&out, reader);
    };
    for (char c : text) {
        htmlTokenizerAppend(&reader.tokenizer, PrStringView{&c, 1});
        drain();
    }
    reader.tokenizer.open = false;
    drain();
    return join(out);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// ---------------------------------------------------------------------------------------------------------------------
// Text (html/#data-state to html/#plaintext-state)

TEST_CASE("text is characters, one token a code point") {
    CHECK(tokens("abc") == "chars abc");
    CHECK(tokens("a\xc3\xa9\xf0\x9f\x98\x80") == "chars a\xc3\xa9\xf0\x9f\x98\x80");
    CHECK(tokens("") == "");
}

TEST_CASE("a null in text is kept and reported") {
    CHECK(tokens(std::string("a\0b", 3)) == std::string("chars a\0b", 9));
    CHECK(errorsOf(std::string("a\0b", 3)) == "unexpected-null-character@1");
}

TEST_CASE("PLAINTEXT reads everything as text") {
    CHECK(tokens("<b>&amp;", HtmlState_Plaintext) == "chars <b>&amp;");
}

// ---------------------------------------------------------------------------------------------------------------------
// Tags (html/#tag-open-state to html/#tag-name-state)

TEST_CASE("a tag name is lowercased") {
    CHECK(tokens("<div>") == "start div");
    CHECK(tokens("<DiV>") == "start div");
    CHECK(tokens("<div></DIV>") == "start div | end div");
    CHECK(tokens("<br/>") == "start br /");
}

TEST_CASE("a tag nothing opened is text") {
    CHECK(tokens("<>") == "chars <>");
    CHECK(tokens("a < b") == "chars a < b");
    CHECK(tokens("<") == "chars <");
    CHECK(errorsOf("<>") == "invalid-first-character-of-tag-name@1");
    CHECK(errorsOf("<") == "eof-before-tag-name@1");
}

TEST_CASE("an end tag with no name closes nothing") {
    CHECK(tokens("a</>b") == "chars ab");
    CHECK(errorsOf("a</>b") == "missing-end-tag-name@3");
    CHECK(tokens("</2>") == "comment 2");
    CHECK(tokens("</") == "chars </");
}

TEST_CASE("a tag the input ends inside is abandoned") {
    CHECK(tokens("a<div") == "chars a");
    CHECK(errorsOf("a<div") == "eof-in-tag@5");
}

// ---------------------------------------------------------------------------------------------------------------------
// Attributes (html/#before-attribute-name-state to html/#self-closing-start-tag-state)

TEST_CASE("an attribute takes a quoted, unquoted or missing value") {
    CHECK(tokens("<a b='c' d=\"e\" f=g h>") == "start a b=c d=e f=g h=");
    CHECK(tokens("<a B=C>") == "start a b=C");
    CHECK(tokens("<a b = c>") == "start a b=c");
}

TEST_CASE("a repeated attribute name is dropped") {
    CHECK(tokens("<a b=1 b=2>") == "start a b=1");
    CHECK(tokens("<a b=1 B=2>") == "start a b=1");
    CHECK(errorsOf("<a b=1 b=2>") == "duplicate-attribute@8");
}

TEST_CASE("an attribute reports what is written oddly") {
    CHECK(tokens("<a b='c'd='e'>") == "start a b=c d=e");
    CHECK(errorsOf("<a b='c'd='e'>") == "missing-whitespace-between-attributes@8");
    CHECK(errorsOf("<a b=`c`>") == "unexpected-character-in-unquoted-attribute-value@5 | "
        "unexpected-character-in-unquoted-attribute-value@7");
    CHECK(errorsOf("<a =b>") == "unexpected-equals-sign-before-attribute-name@3");
    CHECK(errorsOf("<a/ b>") == "unexpected-solidus-in-tag@3");
}

TEST_CASE("an end tag carries no attributes") {
    CHECK(tokens("</a b='c'>") == "end a");
    CHECK(errorsOf("</a b='c'>") == "end-tag-with-attributes@9");
}

// ---------------------------------------------------------------------------------------------------------------------
// Comments (html/#bogus-comment-state to html/#comment-end-bang-state)

TEST_CASE("a comment is what sits between the dashes") {
    CHECK(tokens("<!--x-->") == "comment x");
    CHECK(tokens("<!---->") == "comment ");
    CHECK(tokens("<!-- a -- b -->") == "comment  a -- b ");
    CHECK(tokens("<!----->") == "comment -");
}

TEST_CASE("a comment the input ends inside is still emitted") {
    CHECK(tokens("<!--x") == "comment x");
    CHECK(errorsOf("<!--x") == "eof-in-comment@5");
    CHECK(tokens("<!-->") == "comment ");
    CHECK(errorsOf("<!-->") == "abrupt-closing-of-empty-comment@4");
    CHECK(errorsOf("<!--x--!>") == "incorrectly-closed-comment@8");
}

TEST_CASE("markup that opens no declaration is a bogus comment") {
    CHECK(tokens("<!x>") == "comment x");
    CHECK(errorsOf("<!x>") == "incorrectly-opened-comment@2");
    CHECK(tokens("<![CDATA[x]]>") == "comment [CDATA[x]]");
    CHECK(errorsOf("<![CDATA[x]]>") == "cdata-in-html-content@8");
}

// ---------------------------------------------------------------------------------------------------------------------
// Doctypes (html/#doctype-state to html/#bogus-doctype-state)

TEST_CASE("a doctype name is lowercased, and its identifiers are not") {
    CHECK(tokens("<!DOCTYPE html>") == "doctype html");
    CHECK(tokens("<!doctype HtMl>") == "doctype html");
    CHECK(tokens("<!DOCTYPE a PUBLIC \"P\" \"S\">") == "doctype a public=P system=S");
    CHECK(tokens("<!DOCTYPE a SYSTEM 'S'>") == "doctype a system=S");
}

TEST_CASE("a doctype written wrongly forces quirks") {
    CHECK(tokens("<!DOCTYPE>") == "doctype - quirks");
    CHECK(errorsOf("<!DOCTYPE>") == "missing-doctype-name@9");
    CHECK(tokens("<!DOCTYPE html") == "doctype html quirks");
    CHECK(errorsOf("<!DOCTYPE html") == "eof-in-doctype@14");
    CHECK(tokens("<!DOCTYPE") == "doctype - quirks");
    CHECK(errorsOf("<!DOCTYPE") == "eof-in-doctype@9");
    CHECK(tokens("<!DOCTYPE ") == "doctype - quirks");
    CHECK(tokens("<!DOCTYPE a PUBLIC \"P\"") == "doctype a public=P quirks");
    CHECK(tokens("<!DOCTYPE a BOGUS>") == "doctype a quirks");
}

// ---------------------------------------------------------------------------------------------------------------------
// Character references (html/#character-reference-state to html/#numeric-character-reference-end-state)

TEST_CASE("a named reference expands, with or without its semicolon") {
    CHECK(tokens("&amp;") == "chars &");
    CHECK(tokens("&amp") == "chars &");
    CHECK(errorsOf("&amp") == "missing-semicolon-after-character-reference@4");
    CHECK(tokens("&notin;") == "chars \xe2\x88\x89");
    CHECK(tokens("&notit;") == "chars \xc2\xacit;");
}

TEST_CASE("a reference no name matches is text") {
    CHECK(tokens("&unknown;") == "chars &unknown;");
    CHECK(errorsOf("&unknown;") == "unknown-named-character-reference@8");
    CHECK(tokens("&") == "chars &");
    CHECK(tokens("a & b") == "chars a & b");
}

TEST_CASE("a numeric reference is its code point") {
    CHECK(tokens("&#65;") == "chars A");
    CHECK(tokens("&#x41;") == "chars A");
    CHECK(tokens("&#X41;") == "chars A");
    CHECK(tokens("&#0;") == "chars \xef\xbf\xbd");
    CHECK(errorsOf("&#0;") == "null-character-reference@4");
    CHECK(tokens("&#x110000;") == "chars \xef\xbf\xbd");
    CHECK(tokens("&#xD800;") == "chars \xef\xbf\xbd");
    CHECK(errorsOf("&#") == "absence-of-digits-in-numeric-character-reference@2");
}

TEST_CASE("a numeric reference naming a C1 control takes the windows-1252 code point") {
    CHECK(tokens("&#128;") == "chars \xe2\x82\xac");         // U+20AC EURO SIGN
    CHECK(errorsOf("&#128;") == "control-character-reference@6");
    CHECK(tokens("&#129;") == "chars \xc2\x81");             // no row in the table
}

TEST_CASE("a reference in an attribute follows the historical rule") {
    CHECK(tokens("<a b='&amp;'>") == "start a b=&");
    CHECK(tokens("<a b='&notit;'>") == "start a b=&notit;");
    CHECK(tokens("<a b='&notin;'>") == "start a b=\xe2\x88\x89");
    CHECK(tokens("<a b='&amp'>") == "start a b=&");
}

// ---------------------------------------------------------------------------------------------------------------------
// Elements whose content is text (html/#rcdata-less-than-sign-state to html/#script-data-double-escape-end-state)

TEST_CASE("RCDATA ends at the tag it was entered for") {
    CHECK(tokens("<b>x</title>", HtmlState_Rcdata, "title") == "chars <b>x | end title");
    CHECK(tokens("x</b>y", HtmlState_Rcdata, "title") == "chars x</b>y");
    CHECK(tokens("&amp;", HtmlState_Rcdata, "title") == "chars &");
}

TEST_CASE("RAWTEXT resolves no reference") {
    CHECK(tokens("&amp;</style>", HtmlState_Rawtext, "style") == "chars &amp; | end style");
}

TEST_CASE("script data runs through comment-like text") {
    CHECK(tokens("a</script>", HtmlState_ScriptData, "script") == "chars a | end script");
    CHECK(tokens("<!--<b>--></script>", HtmlState_ScriptData, "script") == "chars <!--<b>--> | end script");

    // A nested <script> inside the comment-like text keeps the end tag from closing until its own end tag.
    CHECK(tokens("<!--<script></script>--></script>", HtmlState_ScriptData, "script") == "chars <!--<script></script>--> | end script");
    CHECK(errorsOf("<!--", HtmlState_ScriptData) == "eof-in-script-html-comment-like-text@4");
}

// ---------------------------------------------------------------------------------------------------------------------
// CDATA sections (html/#cdata-section-state to html/#cdata-section-end-state)

TEST_CASE("a CDATA section opens only in foreign content") {
    CHECK(foreignTokens("<![CDATA[x]]>y") == "chars xy");
    CHECK(foreignTokens("<![CDATA[]]]>") == "chars ]");
    CHECK(foreignTokens("<![CDATA[a]]b]]>") == "chars a]]b");
    CHECK(foreignTokens("<![CDATA[x") == "chars x");
}

// ---------------------------------------------------------------------------------------------------------------------
// Processing instructions (html/#processing-instruction-open-state to html/#processing-instruction-questionable-state)

TEST_CASE("a processing instruction is a target and its data") {
    CHECK(tokens("<?pi some data?>") == "pi pi some data");
    CHECK(tokens("<?pi?>") == "pi pi ");
    CHECK(tokens("<?pi>") == "pi pi ");
}

TEST_CASE("a target html/#parsing disallows is a comment instead") {
    CHECK(tokens("<?xml version='1.0'?>") == "comment ?xml version='1.0'?");
    CHECK(errorsOf("<?xml?>") == "disallowed-processing-instruction-target@5");
    CHECK(tokens("<?0?>") == "comment ?0?");
    CHECK(errorsOf("<?0?>") == "invalid-first-character-of-processing-instruction-target@2");
    CHECK(tokens("<? x?>") == "comment ? x?");
}

// ---------------------------------------------------------------------------------------------------------------------
// The input stream (html/#preprocessing-the-input-stream)

TEST_CASE("the copy holds no carriage return") {
    CHECK(Reader("a\rb").input() == "a\nb");
    CHECK(Reader("a\r\nb").input() == "a\nb");
    CHECK(Reader("a\r\r\nb").input() == "a\n\nb");
    CHECK(Reader("a\r\n\nb").input() == "a\n\nb");
}

TEST_CASE("an ill-formed sequence reads as U+FFFD") {
    // encoding/#utf-8-decoder: one for each maximal subpart.
    CHECK(Reader("a\xff" "b").input() == "a\xef\xbf\xbd" "b");
    CHECK(Reader("a\xe2\x82" "b").input() == "a\xef\xbf\xbd" "b");
    CHECK(Reader("a\xe0\x80" "b").input() == "a\xef\xbf\xbd\xef\xbf\xbd" "b");
    CHECK(Reader("\xc3\xa9").input() == "\xc3\xa9");
    CHECK(tokens("<p title=\"\xff\">") == "start p title=\xef\xbf\xbd");

    Reader reader("ab");
    htmlTokenizerInsert(&reader.tokenizer, "\xf0\x9f\x98\r");
    CHECK(reader.input() == "\xef\xbf\xbd\nab");
}

TEST_CASE("a code point html/#parsing names is reported once") {
    CHECK(errorsOf("\xef\xb7\x90") == "noncharacter-in-input-stream@0");       // U+FDD0
    CHECK(errorsOf("\xed\xa0\x80") == "surrogate-in-input-stream@0");          // U+D800
    CHECK(errorsOf("\x0b") == "control-character-in-input-stream@0");
    CHECK(errorsOf("\t\n\f ") == "");
}

TEST_CASE("a character token is every character the state read in one run") {
    CHECK(runs("hello world") == "hello world space text");
    CHECK(runs("  \n\t") == "  \n\t space");
    CHECK(runs("a<p>b") == "a text | b text");
    CHECK(runs("a&amp;b") == "a text | & text | b text");
    CHECK(runs(std::string("a\0\0b", 4)) == std::string("a text | \0\0 null | b text", 25));
    CHECK(runs("a\xc3\xa9" "b") == "a\xc3\xa9" "b text");
}

TEST_CASE("script data is one run through the comment it is written inside") {
    CHECK(runs("<!--a-->b", HtmlState_ScriptData, "script") == "<!--a-->b text");
    CHECK(runs("a</script>", HtmlState_ScriptData, "script") == "a text");
    CHECK(runs("<!--<script>a</script>-->b", HtmlState_ScriptData, "script") == "<!--<script>a</script>-->b text");
    CHECK(runs("a<!--b", HtmlState_ScriptData, "script") == "a<!--b text");
}

TEST_CASE("an insertion is read next") {
    Reader reader("a<p>");
    REQUIRE(reader.next());
    CHECK(reader.text(reader.tokenizer.token.data) == "a");

    htmlTokenizerInsert(&reader.tokenizer, "xy");
    CHECK(reader.input() == "axy<p>");

    std::vector<std::string> out;
    while (reader.next())
        append(&out, reader);
    CHECK(join(out) == "chars xy | start p");
}

TEST_CASE("an insertion is normalized, and pairs with the line feed it meets") {
    Reader reader("\nb");
    htmlTokenizerInsert(&reader.tokenizer, "a\r");
    CHECK(reader.input() == "a\nb");

    Reader other("\nb");
    htmlTokenizerInsert(&other.tokenizer, "a\r\r");
    CHECK(other.input() == "a\n\nb");
}

TEST_CASE("an appended carriage return pairs with a line feed appended after it") {
    Reader reader("");
    reader.tokenizer.open = true;
    htmlTokenizerAppend(&reader.tokenizer, "a\r");
    htmlTokenizerAppend(&reader.tokenizer, "\nb\r");
    htmlTokenizerAppend(&reader.tokenizer, "c");
    CHECK(reader.input() == "a\nb\nc");
}

TEST_CASE("an input written a byte at a time reads as it would whole") {
    CHECK(written("<p>One<p>Two") == "start p | chars One | start p | chars Two");
    CHECK(written("I'm &notin; I &not;") == "chars I'm ∉ I ¬");
    CHECK(written("a<!-- b --! >c-->d") == "chars a | comment  b --! >c | chars d");
    CHECK(written("<a title='x>y'>z") == "start a title=x>y | chars z");
}

TEST_CASE("a span read before an insertion still names its own text") {
    Reader reader("<tag>b");
    REQUIRE(reader.next());
    REQUIRE(reader.tokenizer.token.kind == HtmlTokenKind_StartTag);
    HtmlSpan name = reader.tokenizer.token.name;
    CHECK(reader.text(name) == "tag");

    // The insertion is at the read point, so it moves no byte that the name covers, however far the buffer must grow to
    // hold it.
    std::string written(4096, 'x');
    htmlTokenizerInsert(&reader.tokenizer, written);
    CHECK(reader.text(name) == "tag");
}

// ---------------------------------------------------------------------------------------------------------------------
// Spans (html/#tokenization)

TEST_CASE("a name the input does not spell is read out of the values") {
    Reader reader("<DiV>");
    REQUIRE(reader.next());
    CHECK((reader.tokenizer.token.name.start & kHtmlSpanValues) != 0);
    CHECK(reader.text(reader.tokenizer.token.name) == "div");

    Reader plain("<div>");
    REQUIRE(plain.next());
    CHECK((plain.tokenizer.token.name.start & kHtmlSpanValues) == 0);
    CHECK(plain.text(plain.tokenizer.token.name) == "div");
}

TEST_CASE("a run of characters into the values survives being drained") {
    // The reference expands to two code points, so the second is read from the values after the token that stored them
    // has been returned.
    CHECK(tokens("&NotEqualTilde;") == "chars \xe2\x89\x82\xcc\xb8");
    CHECK(errorsOf("&NotEqualTilde;") == "");
}

// ---------------------------------------------------------------------------------------------------------------------
// Error positions (html/#parse-errors)

TEST_CASE("a column counts one per code point") {
    CHECK(columnAt("abc", 3) == 4);
    CHECK(columnAt("\xc3\xa9x", 3) == 3);            // U+00E9, two bytes and one column
    CHECK(columnAt("\xe2\x82\xacx", 4) == 3);        // U+20AC, three bytes and one column
}

TEST_CASE("a code point outside the Basic Multilingual Plane counts two columns") {
    CHECK(columnAt("\xf4\x80\x80\x80", 4) == 3);     // U+100000
    CHECK(columnAt("a\xf4\x80\x80\x80", 5) == 4);
}

TEST_CASE("a lone surrogate is one code point and one column") {
    CHECK(columnAt("\xed\xa0\x80x", 3) == 2);
    CHECK(columnAt("\xed\xa0\x80\xed\xbf\xbf", 6) == 3);
}

TEST_CASE("a line feed starts a line") {
    CHECK(lineAt("a\nb", 3) == 2);
    CHECK(columnAt("a\nb", 3) == 2);
    CHECK(lineAt("a\n\nb", 4) == 3);
}
