// The css-parsing-tests suite, with results in its own JSON format.
//
// servo/rust-cssparser carries implementation-independent tests for the parser entry points. Each file is a JSON array
// that pairs each input with its expected result. This program reads framed inputs on stdin and writes one JSON result
// per line, which tools/suite/css_parsing.py pairs with the expectations.
//
// css-syntax-3 - https://drafts.csswg.org/css-syntax/

#include "pr/assert.h"
#include "pr/float.h"
#include "pr/print.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/utf8.h"

#include "css/src/css_parser.h"
#include "css/src/css_syntax.h"
#include "tools/harness/harness.h"

using namespace harness;

using namespace pr;

using namespace pr_impl;

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// JSON

// Writes a token's value, with escapes resolved.
void writeValue(PrTextBuffer* out, PrStringView text) {
    PrTextBuffer value = {};
    cssValueAppend(&value, text);
    harnessWriteString(out, prTextBufferView(&value));
    prTextBufferReset(&value);
}

void writeNumber(PrTextBuffer* out, double value) {
    char text[kPrF64TextMaxBytes];
    size_t length = prF64Format(value, text);
    prTextBufferAppend(out, PrStringView{text, uint32_t(length)});
}

void writeError(PrTextBuffer* out, PrStringView kind) {
    prTextBufferAppend(out, "[\"error\", ");
    harnessWriteString(out, kind);
    prTextBufferAppendChar(out, ']');
}

// ---------------------------------------------------------------------------------------------------------------------
// Component values (the suite's README, "Result representation")

void writeComponentValues(PrTextBuffer* out, const CssTokenStream* ts, CssTokenSpan span);

// Returns the bytes that a numeric token was written with, which the suite calls its representation.
PrStringView representation(const CssTokenStream* ts, uint32_t index) {
    PrStringView text = cssTokenText(ts->tokens, index);
    if (cssTokenKind(ts->tokens, index) == CssTokenKind_Percentage)
        --text.size;
    else if (cssTokenKind(ts->tokens, index) == CssTokenKind_Dimension)
        text.size -= cssTokenUnitText(ts->tokens, index).size;
    return text;
}

void writeNumeric(PrTextBuffer* out, const CssTokenStream* ts, uint32_t index, PrStringView kind) {
    prTextBufferAppendChar(out, '[');
    harnessWriteString(out, kind);
    prTextBufferAppend(out, ", ");
    harnessWriteString(out, representation(ts, index));
    prTextBufferAppend(out, ", ");
    writeNumber(out, cssTokenNumber(ts->tokens, index));
    prTextBufferAppend(out, ", ");
    harnessWriteString(out, (cssTokenFlags(ts->tokens, index) & CssTokenFlag_Integer) != 0 ? "integer" : "number");
    if (cssTokenKind(ts->tokens, index) == CssTokenKind_Dimension) {
        prTextBufferAppend(out, ", ");
        writeValue(out, cssTokenUnitText(ts->tokens, index));
    }
    prTextBufferAppendChar(out, ']');
}

// Returns the contents of the block that `span` covers, which is `span` without its brackets. A block that ran to the
// end of the input has no closing bracket to drop.
CssTokenSpan blockContents(const CssTokenStream* ts, CssTokenSpan span, CssTokenKind closer) {
    CssTokenSpan inner = CssTokenSpan{span.start + 1, span.count - 1};
    if (inner.count != 0 && cssTokenKind(ts->tokens, inner.start + inner.count - 1) == closer)
        --inner.count;
    return inner;
}

// Writes one component value. `span` is the span that cssConsumeComponentValue returned for it.
void writeComponentValue(PrTextBuffer* out, const CssTokenStream* ts, CssTokenSpan span) {
    uint32_t index = span.start;

    switch (cssTokenKind(ts->tokens, index)) {
        case CssTokenKind_Ident:
            prTextBufferAppend(out, "[\"ident\", ");
            writeValue(out, cssTokenValueText(ts->tokens, index));
            prTextBufferAppendChar(out, ']');
            return;
        case CssTokenKind_AtKeyword:
            prTextBufferAppend(out, "[\"at-keyword\", ");
            writeValue(out, cssTokenValueText(ts->tokens, index));
            prTextBufferAppendChar(out, ']');
            return;
        case CssTokenKind_Hash:
            prTextBufferAppend(out, "[\"hash\", ");
            writeValue(out, cssTokenValueText(ts->tokens, index));
            prTextBufferAppend(out, (cssTokenFlags(ts->tokens, index) & CssTokenFlag_Id) != 0 ? ", \"id\"]" :
                ", \"unrestricted\"]");
            return;
        case CssTokenKind_String:
            prTextBufferAppend(out, "[\"string\", ");
            writeValue(out, cssTokenValueText(ts->tokens, index));
            prTextBufferAppendChar(out, ']');
            return;
        case CssTokenKind_BadString:
            prTextBufferAppend(out, "[\"error\", \"bad-string\"]");
            return;
        case CssTokenKind_Url:
            prTextBufferAppend(out, "[\"url\", ");
            writeValue(out, cssTokenValueText(ts->tokens, index));
            prTextBufferAppendChar(out, ']');
            return;
        case CssTokenKind_BadUrl:
            prTextBufferAppend(out, "[\"error\", \"bad-url\"]");
            return;

        case CssTokenKind_Delim: {
            char utf8[kPrUtf8MaxBytes];
            uint32_t width = prUtf8Encode(cssTokenDelim(ts->tokens, index), utf8);
            harnessWriteString(out, PrStringView{utf8, width});
            return;
        }

        case CssTokenKind_Number:     writeNumeric(out, ts, index, "number"); return;
        case CssTokenKind_Percentage: writeNumeric(out, ts, index, "percentage"); return;
        case CssTokenKind_Dimension:  writeNumeric(out, ts, index, "dimension"); return;

        case CssTokenKind_UnicodeRange: {
            uint32_t first = 0;
            uint32_t last = 0;
            cssTokenRange(ts->tokens, index, &first, &last);
            prTextBufferAppend(out, "[\"unicode-range\", ");
            writeNumber(out, first);
            prTextBufferAppend(out, ", ");
            writeNumber(out, last);
            prTextBufferAppendChar(out, ']');
            return;
        }

        case CssTokenKind_Whitespace:   prTextBufferAppend(out, "\" \""); return;
        case CssTokenKind_Cdo:          prTextBufferAppend(out, "\"<!--\""); return;
        case CssTokenKind_Cdc:          prTextBufferAppend(out, "\"-->\""); return;
        case CssTokenKind_Colon:        prTextBufferAppend(out, "\":\""); return;
        case CssTokenKind_Semicolon:    prTextBufferAppend(out, "\";\""); return;
        case CssTokenKind_Comma:        prTextBufferAppend(out, "\",\""); return;
        case CssTokenKind_CloseBracket: prTextBufferAppend(out, "\"]\""); return;
        case CssTokenKind_CloseParen:   prTextBufferAppend(out, "\")\""); return;
        case CssTokenKind_CloseBrace:   prTextBufferAppend(out, "\"}\""); return;

        case CssTokenKind_Function:
            prTextBufferAppend(out, "[\"function\", ");
            writeValue(out, cssTokenValueText(ts->tokens, index));
            writeComponentValues(out, ts, blockContents(ts, span, CssTokenKind_CloseParen));
            prTextBufferAppendChar(out, ']');
            return;
        case CssTokenKind_OpenParen:
            prTextBufferAppend(out, "[\"()\"");
            writeComponentValues(out, ts, blockContents(ts, span, CssTokenKind_CloseParen));
            prTextBufferAppendChar(out, ']');
            return;
        case CssTokenKind_OpenBracket:
            prTextBufferAppend(out, "[\"[]\"");
            writeComponentValues(out, ts, blockContents(ts, span, CssTokenKind_CloseBracket));
            prTextBufferAppendChar(out, ']');
            return;
        case CssTokenKind_OpenBrace:
            prTextBufferAppend(out, "[\"{}\"");
            writeComponentValues(out, ts, blockContents(ts, span, CssTokenKind_CloseBrace));
            prTextBufferAppendChar(out, ']');
            return;

        case CssTokenKind_Eof:
            return;
    }
}

// Writes every component value in `span`, each preceded by a comma.
void writeComponentValues(PrTextBuffer* out, const CssTokenStream* ts, CssTokenSpan span) {
    CssTokenStream scan = cssSubStream(ts, span);
    CssTokenSpan value;
    while (cssConsumeComponentValue(&scan, &value)) {
        prTextBufferAppend(out, ", ");
        writeComponentValue(out, ts, value);
    }
}

// Writes every component value in `span` as an array of its own.
void writeComponentValueList(PrTextBuffer* out, const CssTokenStream* ts, CssTokenSpan span) {
    CssTokenStream scan = cssSubStream(ts, span);
    CssTokenSpan value;
    bool first = true;
    prTextBufferAppendChar(out, '[');
    while (cssConsumeComponentValue(&scan, &value)) {
        if (!first)
            prTextBufferAppend(out, ", ");
        first = false;
        writeComponentValue(out, ts, value);
    }
    prTextBufferAppendChar(out, ']');
}

// ---------------------------------------------------------------------------------------------------------------------
// Rules and declarations

void writeDeclaration(PrTextBuffer* out, const CssTokenStream* ts, const CssSyntaxDeclaration& declaration) {
    prTextBufferAppend(out, "[\"declaration\", ");
    writeValue(out, cssTokenValueText(ts->tokens, declaration.name));
    prTextBufferAppend(out, ", ");
    writeComponentValueList(out, ts, cssTrimWhitespace(ts->tokens, declaration.value));
    prTextBufferAppend(out, declaration.important ? ", true]" : ", false]");
}

void writeRule(PrTextBuffer* out, const CssTokenStream* ts, const CssSyntaxRule& rule) {
    if (rule.type == CssSyntaxRuleType_At) {
        prTextBufferAppend(out, "[\"at-rule\", ");
        writeValue(out, cssTokenValueText(ts->tokens, rule.name));
        prTextBufferAppend(out, ", ");
        writeComponentValueList(out, ts, rule.prelude);
        prTextBufferAppend(out, ", ");
        if ((rule.flags & CssSyntaxRuleFlag_HasBlock) != 0)
            writeComponentValueList(out, ts, rule.block);
        else
            prTextBufferAppend(out, "null");
        prTextBufferAppendChar(out, ']');
        return;
    }

    prTextBufferAppend(out, "[\"qualified rule\", ");
    writeComponentValueList(out, ts, rule.prelude);
    prTextBufferAppend(out, ", ");
    writeComponentValueList(out, ts, rule.block);
    prTextBufferAppendChar(out, ']');
}

// Writes the items of a parsed block, in written order.
void writeItems(PrTextBuffer* out, const CssTokenStream* ts, const CssParser& parser, CssBlock parsed) {
    bool first = true;
    prTextBufferAppendChar(out, '[');
    for (uint32_t i = 0; i < parsed.items.count; ++i) {
        const CssItem& item = parser.items.data[parsed.items.start + i];
        if (!first)
            prTextBufferAppend(out, ", ");
        first = false;

        if (item.type == CssItemType_Declaration)
            writeDeclaration(out, ts, parser.declarations.data[item.index]);
        else if (item.type == CssItemType_Rule)
            writeRule(out, ts, parser.rules.data[item.index]);
        else
            writeError(out, "invalid");
    }
    prTextBufferAppendChar(out, ']');
}

// ---------------------------------------------------------------------------------------------------------------------
// Suites

// Returns true if the stream holds only whitespace.
bool isEmpty(CssTokenStream* ts) {
    cssSkipWhitespace(ts);
    return cssPeek(ts) == CssTokenKind_Eof;
}

void runOneComponentValue(PrTextBuffer* out, const CssTokenStream* ts) {
    CssTokenStream scan = *ts;
    if (isEmpty(&scan)) {
        writeError(out, "empty");
        return;
    }

    CssTokenSpan value;
    cssConsumeComponentValue(&scan, &value);
    if (!isEmpty(&scan)) {
        writeError(out, "extra-input");
        return;
    }
    writeComponentValue(out, ts, value);
}

void runOneDeclaration(PrTextBuffer* out, const CssTokenStream* ts) {
    CssTokenStream scan = *ts;
    if (isEmpty(&scan)) {
        writeError(out, "empty");
        return;
    }

    CssSyntaxDeclaration declaration;
    if (!cssParseDeclaration(&scan, &declaration)) {
        writeError(out, "invalid");
        return;
    }
    writeDeclaration(out, ts, declaration);
}

void runOneRule(PrTextBuffer* out, const CssTokenStream* ts) {
    CssTokenStream scan = *ts;
    if (isEmpty(&scan)) {
        writeError(out, "empty");
        return;
    }

    CssSyntaxRule rule;
    if (!cssParseRule(&scan, &rule)) {
        writeError(out, "invalid");
        return;
    }
    if (!isEmpty(&scan)) {
        writeError(out, "extra-input");
        return;
    }
    writeRule(out, ts, rule);
}

void runBlock(PrTextBuffer* out, const CssTokenStream* ts, bool stylesheet) {
    CssTokenStream scan = *ts;
    CssParser parser;
    cssParserInit(&parser);
    CssBlock parsed = {};
    if (stylesheet)
        cssParseStylesheetContents(&scan, &parser, &parsed);
    else
        cssParseBlockContents(&scan, &parser, &parsed);

    writeItems(out, ts, parser, parsed);
    cssParserReset(&parser);
}

void runAnPlusB(PrTextBuffer* out, const CssTokenStream* ts) {
    CssTokenStream scan = *ts;
    cssSkipWhitespace(&scan);

    int32_t a = 0;
    int32_t b = 0;
    if (!cssConsumeAnPlusB(&scan, &a, &b) || !isEmpty(&scan)) {
        prTextBufferAppend(out, "null");
        return;
    }
    prTextBufferAppendChar(out, '[');
    writeNumber(out, a);
    prTextBufferAppend(out, ", ");
    writeNumber(out, b);
    prTextBufferAppendChar(out, ']');
}

// Writes a comma-separated list of ranges. An item is a range if the whole item is one <unicode-range-token> within the
// bounds that css-fonts-4/#unicode-range-desc gives, and null otherwise.
void runUrange(PrTextBuffer* out, const CssTokenStream* ts) {
    CssTokenStream scan = *ts;
    bool first = true;
    prTextBufferAppendChar(out, '[');
    for (;;) {
        uint32_t start = scan.pos;
        while (cssPeek(&scan) != CssTokenKind_Comma && cssPeek(&scan) != CssTokenKind_Eof)
            ++scan.pos;

        CssTokenSpan item = cssTrimWhitespace(ts->tokens, CssTokenSpan{start, scan.pos - start});
        uint32_t rangeFirst = 0;
        uint32_t rangeLast = 0;
        if (item.count == 1 && cssTokenKind(ts->tokens, item.start) == CssTokenKind_UnicodeRange)
            cssTokenRange(ts->tokens, item.start, &rangeFirst, &rangeLast);

        if (!first)
            prTextBufferAppend(out, ", ");
        first = false;

        if (rangeLast != 0 && rangeLast <= kPrUtf8MaxCodePoint && rangeFirst <= rangeLast) {
            prTextBufferAppendChar(out, '[');
            writeNumber(out, rangeFirst);
            prTextBufferAppend(out, ", ");
            writeNumber(out, rangeLast);
            prTextBufferAppendChar(out, ']');
        } else {
            prTextBufferAppend(out, "null");
        }

        if (cssPeek(&scan) != CssTokenKind_Comma)
            break;
        ++scan.pos;
    }
    prTextBufferAppendChar(out, ']');
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    if (argc != 2) {
        printError("usage: css_parsing <suite>\n");
        return 2;
    }

    PrStringView suite = argv[1];
    bool ranges = suite == "urange";

    HarnessCase test = {};
    PrTextBuffer out = {};
    while (harnessReadCase(&test)) {
        CssTokenScratch scratch = {};
        CssTokenArray tokens = {};
        PrStringView source = prTextBufferView(&test.input);
        cssTokenize(&tokens, &scratch, source, ranges ? CssTokenizeFlag_UnicodeRanges : 0);
        CssTokenStream ts = cssStream(&tokens);

        prTextBufferClear(&out);
        if (suite == "component_value_list")
            writeComponentValueList(&out, &ts, cssStreamAll(&ts));
        else if (suite == "one_component_value")
            runOneComponentValue(&out, &ts);
        else if (suite == "one_declaration")
            runOneDeclaration(&out, &ts);
        else if (suite == "declaration_list")
            runBlock(&out, &ts, false);
        else if (suite == "one_rule")
            runOneRule(&out, &ts);
        else if (suite == "rule_list" || suite == "stylesheet")
            runBlock(&out, &ts, true);
        else if (suite == "An+B")
            runAnPlusB(&out, &ts);
        else if (ranges)
            runUrange(&out, &ts);
        else {
            printError("no suite named {}\n", suite);
            return 2;
        }

        harnessWriteLine(prTextBufferView(&out));
        cssTokenArrayReset(&tokens);
        cssTokenScratchReset(&scratch);
    }

    harnessCaseReset(&test);
    prTextBufferReset(&out);
    return 0;
}
