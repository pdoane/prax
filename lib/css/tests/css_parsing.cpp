// The css-parsing-tests suite, with results in its own JSON format.
//
// servo/rust-cssparser carries implementation-independent tests for the parser entry points. Each file is a JSON array
// that pairs each input with its expected result. This program reads framed inputs on stdin and writes one JSON result
// per line, which tools/suite/css_parsing.py pairs with the expectations.
//
// css-syntax-3 - https://drafts.csswg.org/css-syntax/

#include "pr/assert.h"
#include "pr/float.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/utf8.h"

#include "css/src/css_parser.h"
#include "css/src/css_syntax.h"
#include "tools/harness/harness.h"

#include <stdio.h>
#include <string.h>

using namespace harness;

using namespace pr_impl;

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// JSON

// Writes a token's value, with escapes resolved.
void writeValue(TextBuffer* out, PrStringView text) {
    TextBuffer value = {};
    cssValueAppend(&value, text);
    harnessWriteString(out, textView(&value));
    textReset(&value);
}

void writeNumber(TextBuffer* out, double value) {
    char text[kF64TextMaxBytes];
    size_t length = formatF64(value, text);
    textAppend(out, text, uint32_t(length));
}

void writeError(TextBuffer* out, PrStringView kind) {
    textAppend(out, "[\"error\", ");
    harnessWriteString(out, kind);
    textAppendChar(out, ']');
}

// ---------------------------------------------------------------------------------------------------------------------
// Component values (the suite's README, "Result representation")

void writeComponentValues(TextBuffer* out, const CssTokenStream* ts, CssTokenSpan span);

// Returns the bytes that a numeric token was written with, which the suite calls its representation.
PrStringView representation(const CssTokenStream* ts, uint32_t index) {
    PrStringView text = cssTokenText(ts->tokens, index);
    if (cssTokenKind(ts->tokens, index) == CssTokenKind_Percentage)
        --text.size;
    else if (cssTokenKind(ts->tokens, index) == CssTokenKind_Dimension)
        text.size -= cssTokenUnitText(ts->tokens, index).size;
    return text;
}

void writeNumeric(TextBuffer* out, const CssTokenStream* ts, uint32_t index, PrStringView kind) {
    textAppendChar(out, '[');
    harnessWriteString(out, kind);
    textAppend(out, ", ");
    harnessWriteString(out, representation(ts, index));
    textAppend(out, ", ");
    writeNumber(out, cssTokenNumber(ts->tokens, index));
    textAppend(out, ", ");
    harnessWriteString(out, (cssTokenFlags(ts->tokens, index) & CssTokenFlag_Integer) != 0
                                ? "integer"
                                : "number");
    if (cssTokenKind(ts->tokens, index) == CssTokenKind_Dimension) {
        textAppend(out, ", ");
        writeValue(out, cssTokenUnitText(ts->tokens, index));
    }
    textAppendChar(out, ']');
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
void writeComponentValue(TextBuffer* out, const CssTokenStream* ts, CssTokenSpan span) {
    uint32_t index = span.start;

    switch (cssTokenKind(ts->tokens, index)) {
        case CssTokenKind_Ident:
            textAppend(out, "[\"ident\", ");
            writeValue(out, cssTokenValueText(ts->tokens, index));
            textAppendChar(out, ']');
            return;
        case CssTokenKind_AtKeyword:
            textAppend(out, "[\"at-keyword\", ");
            writeValue(out, cssTokenValueText(ts->tokens, index));
            textAppendChar(out, ']');
            return;
        case CssTokenKind_Hash:
            textAppend(out, "[\"hash\", ");
            writeValue(out, cssTokenValueText(ts->tokens, index));
            textAppend(out, (cssTokenFlags(ts->tokens, index) & CssTokenFlag_Id) != 0
                                ? ", \"id\"]"
                                : ", \"unrestricted\"]");
            return;
        case CssTokenKind_String:
            textAppend(out, "[\"string\", ");
            writeValue(out, cssTokenValueText(ts->tokens, index));
            textAppendChar(out, ']');
            return;
        case CssTokenKind_BadString:
            textAppend(out, "[\"error\", \"bad-string\"]");
            return;
        case CssTokenKind_Url:
            textAppend(out, "[\"url\", ");
            writeValue(out, cssTokenValueText(ts->tokens, index));
            textAppendChar(out, ']');
            return;
        case CssTokenKind_BadUrl:
            textAppend(out, "[\"error\", \"bad-url\"]");
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
            textAppend(out, "[\"unicode-range\", ");
            writeNumber(out, first);
            textAppend(out, ", ");
            writeNumber(out, last);
            textAppendChar(out, ']');
            return;
        }

        case CssTokenKind_Whitespace:   textAppend(out, "\" \""); return;
        case CssTokenKind_Cdo:          textAppend(out, "\"<!--\""); return;
        case CssTokenKind_Cdc:          textAppend(out, "\"-->\""); return;
        case CssTokenKind_Colon:        textAppend(out, "\":\""); return;
        case CssTokenKind_Semicolon:    textAppend(out, "\";\""); return;
        case CssTokenKind_Comma:        textAppend(out, "\",\""); return;
        case CssTokenKind_CloseBracket: textAppend(out, "\"]\""); return;
        case CssTokenKind_CloseParen:   textAppend(out, "\")\""); return;
        case CssTokenKind_CloseBrace:   textAppend(out, "\"}\""); return;

        case CssTokenKind_Function:
            textAppend(out, "[\"function\", ");
            writeValue(out, cssTokenValueText(ts->tokens, index));
            writeComponentValues(out, ts, blockContents(ts, span, CssTokenKind_CloseParen));
            textAppendChar(out, ']');
            return;
        case CssTokenKind_OpenParen:
            textAppend(out, "[\"()\"");
            writeComponentValues(out, ts, blockContents(ts, span, CssTokenKind_CloseParen));
            textAppendChar(out, ']');
            return;
        case CssTokenKind_OpenBracket:
            textAppend(out, "[\"[]\"");
            writeComponentValues(out, ts, blockContents(ts, span, CssTokenKind_CloseBracket));
            textAppendChar(out, ']');
            return;
        case CssTokenKind_OpenBrace:
            textAppend(out, "[\"{}\"");
            writeComponentValues(out, ts, blockContents(ts, span, CssTokenKind_CloseBrace));
            textAppendChar(out, ']');
            return;

        case CssTokenKind_Eof:
            return;
    }
}

// Writes every component value in `span`, each preceded by a comma.
void writeComponentValues(TextBuffer* out, const CssTokenStream* ts, CssTokenSpan span) {
    CssTokenStream scan = cssSubStream(ts, span);
    CssTokenSpan value;
    while (cssConsumeComponentValue(&scan, &value)) {
        textAppend(out, ", ");
        writeComponentValue(out, ts, value);
    }
}

// Writes every component value in `span` as an array of its own.
void writeComponentValueList(TextBuffer* out, const CssTokenStream* ts, CssTokenSpan span) {
    CssTokenStream scan = cssSubStream(ts, span);
    CssTokenSpan value;
    bool first = true;
    textAppendChar(out, '[');
    while (cssConsumeComponentValue(&scan, &value)) {
        if (!first)
            textAppend(out, ", ");
        first = false;
        writeComponentValue(out, ts, value);
    }
    textAppendChar(out, ']');
}

// ---------------------------------------------------------------------------------------------------------------------
// Rules and declarations

void writeDeclaration(TextBuffer* out, const CssTokenStream* ts,
                      const CssSyntaxDeclaration& declaration) {
    textAppend(out, "[\"declaration\", ");
    writeValue(out, cssTokenValueText(ts->tokens, declaration.name));
    textAppend(out, ", ");
    writeComponentValueList(out, ts, cssTrimWhitespace(ts->tokens, declaration.value));
    textAppend(out, declaration.important ? ", true]" : ", false]");
}

void writeRule(TextBuffer* out, const CssTokenStream* ts, const CssSyntaxRule& rule) {
    if (rule.type == CssSyntaxRuleType_At) {
        textAppend(out, "[\"at-rule\", ");
        writeValue(out, cssTokenValueText(ts->tokens, rule.name));
        textAppend(out, ", ");
        writeComponentValueList(out, ts, rule.prelude);
        textAppend(out, ", ");
        if ((rule.flags & CssSyntaxRuleFlag_HasBlock) != 0)
            writeComponentValueList(out, ts, rule.block);
        else
            textAppend(out, "null");
        textAppendChar(out, ']');
        return;
    }

    textAppend(out, "[\"qualified rule\", ");
    writeComponentValueList(out, ts, rule.prelude);
    textAppend(out, ", ");
    writeComponentValueList(out, ts, rule.block);
    textAppendChar(out, ']');
}

// Writes the items of a parsed block, in written order.
void writeItems(TextBuffer* out, const CssTokenStream* ts, const CssParser& parser,
                CssBlock parsed) {
    bool first = true;
    textAppendChar(out, '[');
    for (uint32_t i = 0; i < parsed.items.count; ++i) {
        const CssItem& item = parser.items.data[parsed.items.start + i];
        if (!first)
            textAppend(out, ", ");
        first = false;

        if (item.type == CssItemType_Declaration)
            writeDeclaration(out, ts, parser.declarations.data[item.index]);
        else if (item.type == CssItemType_Rule)
            writeRule(out, ts, parser.rules.data[item.index]);
        else
            writeError(out, "invalid");
    }
    textAppendChar(out, ']');
}

// ---------------------------------------------------------------------------------------------------------------------
// Suites

// Returns true if the stream holds only whitespace.
bool isEmpty(CssTokenStream* ts) {
    cssSkipWhitespace(ts);
    return cssPeek(ts) == CssTokenKind_Eof;
}

void runOneComponentValue(TextBuffer* out, const CssTokenStream* ts) {
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

void runOneDeclaration(TextBuffer* out, const CssTokenStream* ts) {
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

void runOneRule(TextBuffer* out, const CssTokenStream* ts) {
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

void runBlock(TextBuffer* out, const CssTokenStream* ts, bool stylesheet) {
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

void runAnPlusB(TextBuffer* out, const CssTokenStream* ts) {
    CssTokenStream scan = *ts;
    cssSkipWhitespace(&scan);

    int32_t a = 0;
    int32_t b = 0;
    if (!cssConsumeAnPlusB(&scan, &a, &b) || !isEmpty(&scan)) {
        textAppend(out, "null");
        return;
    }
    textAppendChar(out, '[');
    writeNumber(out, a);
    textAppend(out, ", ");
    writeNumber(out, b);
    textAppendChar(out, ']');
}

// Writes a comma-separated list of ranges. An item is a range if the whole item is one <unicode-range-token> within the
// bounds that css-fonts-4/#unicode-range-desc gives, and null otherwise.
void runUrange(TextBuffer* out, const CssTokenStream* ts) {
    CssTokenStream scan = *ts;
    bool first = true;
    textAppendChar(out, '[');
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
            textAppend(out, ", ");
        first = false;

        if (rangeLast != 0 && rangeLast <= kPrUtf8MaxCodePoint && rangeFirst <= rangeLast) {
            textAppendChar(out, '[');
            writeNumber(out, rangeFirst);
            textAppend(out, ", ");
            writeNumber(out, rangeLast);
            textAppendChar(out, ']');
        } else {
            textAppend(out, "null");
        }

        if (cssPeek(&scan) != CssTokenKind_Comma)
            break;
        ++scan.pos;
    }
    textAppendChar(out, ']');
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    if (argc != 2) {
        fprintf(stderr, "usage: css_parsing <suite>\n");
        return 2;
    }

    const char* suite = argv[1];
    bool ranges = strcmp(suite, "urange") == 0;

    HarnessCase test = {};
    TextBuffer out;
    while (harnessReadCase(&test)) {
        CssTokenScratch scratch = {};
        CssTokenArray tokens = {};
        PrStringView source = textView(&test.input);
        cssTokenize(&tokens, &scratch, source, ranges ? CssTokenizeFlag_UnicodeRanges : 0);
        CssTokenStream ts = cssStream(&tokens);

        textClear(&out);
        if (strcmp(suite, "component_value_list") == 0)
            writeComponentValueList(&out, &ts, cssStreamAll(&ts));
        else if (strcmp(suite, "one_component_value") == 0)
            runOneComponentValue(&out, &ts);
        else if (strcmp(suite, "one_declaration") == 0)
            runOneDeclaration(&out, &ts);
        else if (strcmp(suite, "declaration_list") == 0)
            runBlock(&out, &ts, false);
        else if (strcmp(suite, "one_rule") == 0)
            runOneRule(&out, &ts);
        else if (strcmp(suite, "rule_list") == 0 || strcmp(suite, "stylesheet") == 0)
            runBlock(&out, &ts, true);
        else if (strcmp(suite, "An+B") == 0)
            runAnPlusB(&out, &ts);
        else if (ranges)
            runUrange(&out, &ts);
        else {
            fprintf(stderr, "no suite named %s\n", suite);
            return 2;
        }

        harnessWriteLine(textView(&out));
        cssTokenArrayReset(&tokens);
        cssTokenScratchReset(&scratch);
    }

    harnessCaseReset(&test);
    textReset(&out);
    return 0;
}
