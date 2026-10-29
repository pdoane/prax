// The html5lib tokenizer suite, reported in its own JSON format.
//
// html5lib/html5lib-tests pairs an input with the tokens that it produces and the errors that it reports. This program
// reads framed cases from stdin and writes one JSON result per line. tools/suite/html_tokens.py matches the results to
// the cases.
//
// html/#tokenization - https://html.spec.whatwg.org/multipage/parsing.html#tokenization

#include "pr/assert.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"

#include "html/src/html_tokenizer.h"
#include "tools/harness/harness.h"

#include <stdio.h>
#include <stdlib.h>

using namespace harness;

using namespace pr_impl;

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Tokens (the suite's README, "Test results")

void writeSpan(TextBuffer* out, const HtmlTokenizer* tokenizer, HtmlSpan span) {
    harnessWriteString(out, htmlSpanText(tokenizer, span));
}

// Writes a string that the suite writes as null if the token has none.
void writeOptional(TextBuffer* out, const HtmlTokenizer* tokenizer, HtmlSpan span,
                   HtmlTokenFlags absent) {
    if ((tokenizer->token.flags & absent) != 0)
        textAppend(out, "null");
    else
        writeSpan(out, tokenizer, span);
}

void writeAttributes(TextBuffer* out, const HtmlTokenizer* tokenizer) {
    textAppendChar(out, '{');
    for (uint32_t i = 0; i < tokenizer->attributes.count; ++i) {
        const HtmlAttribute& attribute = tokenizer->attributes.data[i];
        if (i != 0)
            textAppend(out, ", ");
        writeSpan(out, tokenizer, attribute.name);
        textAppend(out, ": ");
        writeSpan(out, tokenizer, attribute.value);
    }
    textAppendChar(out, '}');
}

void writeToken(TextBuffer* out, const HtmlTokenizer* tokenizer) {
    const HtmlToken& token = tokenizer->token;
    switch (token.kind) {
        case HtmlTokenKind_Doctype:
            textAppend(out, "[\"DOCTYPE\", ");
            writeOptional(out, tokenizer, token.name, HtmlTokenFlag_NoName);
            textAppend(out, ", ");
            writeOptional(out, tokenizer, token.publicId, HtmlTokenFlag_NoPublicId);
            textAppend(out, ", ");
            writeOptional(out, tokenizer, token.systemId, HtmlTokenFlag_NoSystemId);
            // The suite's correctness flag is the force-quirks flag, inverted.
            textAppend(out, (token.flags & HtmlTokenFlag_ForceQuirks) != 0 ? ", false]" : ", true]");
            return;

        case HtmlTokenKind_StartTag:
            textAppend(out, "[\"StartTag\", ");
            writeSpan(out, tokenizer, token.name);
            textAppend(out, ", ");
            writeAttributes(out, tokenizer);
            // A start tag written with a trailing '/' gets a fourth entry. One without it has three, which is what the
            // suite records.
            textAppend(out, (token.flags & HtmlTokenFlag_SelfClosing) != 0 ? ", true]" : "]");
            return;

        case HtmlTokenKind_EndTag:
            textAppend(out, "[\"EndTag\", ");
            writeSpan(out, tokenizer, token.name);
            textAppendChar(out, ']');
            return;

        case HtmlTokenKind_Comment:
            textAppend(out, "[\"Comment\", ");
            writeSpan(out, tokenizer, token.data);
            textAppendChar(out, ']');
            return;

        case HtmlTokenKind_Character:
            textAppend(out, "[\"Character\", ");
            writeSpan(out, tokenizer, token.data);
            textAppendChar(out, ']');
            return;

        case HtmlTokenKind_ProcessingInstruction:
            // The suite predates html/#parsing's processing instructions, and has no form for one.
            textAppend(out, "[\"ProcessingInstruction\", ");
            writeSpan(out, tokenizer, token.name);
            textAppend(out, ", ");
            writeSpan(out, tokenizer, token.data);
            textAppendChar(out, ']');
            return;

        case HtmlTokenKind_Eof:
            return;
    }
}

void writeErrors(TextBuffer* out, const HtmlTokenizer* tokenizer, const Array<HtmlError>& errors) {
    PrStringView input = htmlInputText(tokenizer);
    uint32_t at = 0;
    uint32_t line = 1;
    uint32_t column = 1;

    textAppendChar(out, '[');
    for (uint32_t i = 0; i < errors.count; ++i) {
        htmlInputPosition(input, &at, errors.data[i].at, &line, &column);

        if (i != 0)
            textAppend(out, ", ");
        textAppend(out, "{\"code\": ");
        harnessWriteString(out, kHtmlErrorName[errors.data[i].code]);
        textAppend(out, ", \"line\": ");
        harnessWriteNumber(out, line);
        textAppend(out, ", \"col\": ");
        harnessWriteNumber(out, column);
        textAppendChar(out, '}');
    }
    textAppendChar(out, ']');
}

// ---------------------------------------------------------------------------------------------------------------------
// Cases

HtmlState stateFromName(PrStringView name) {
    for (HtmlState state = 0; state < HtmlState_Count; ++state) {
        if (name == kHtmlStateName[state])
            return state;
    }
    fprintf(stderr, "no state named %.*s\n", int(name.size), name.data);
    exit(2);
}

// A case's fields are its state and its last start tag, which is "-" if the case names none.
void run(TextBuffer* out, HtmlTokenizer* tokenizer, const HarnessCase& test,
         Array<HtmlError>* errors) {
    errors->count = 0;
    PrStringView input = textView(&test.input);
    htmlTokenizerStart(tokenizer, input, errors);

    // The suite names the state together with the tag that an end tag must match, as the tree builder sets them.
    PrStringView tag = test.fields[1];
    if (tag == "-")
        tag = PrStringView{};
    htmlTokenizerSetState(tokenizer, stateFromName(test.fields[0]), tag);

    textAppend(out, "{\"tokens\": [");
    bool first = true;
    for (;;) {
        htmlTokenizerNext(tokenizer);
        if (tokenizer->token.kind == HtmlTokenKind_Eof)
            break;
        if (!first)
            textAppend(out, ", ");
        first = false;
        writeToken(out, tokenizer);
    }
    textAppend(out, "], \"errors\": ");
    writeErrors(out, tokenizer, *errors);
    textAppend(out, "}");
}

} // namespace

int main() {
    prCrashHandler();
    HarnessCase test = {};
    TextBuffer out;
    HtmlTokenizer tokenizer = {};
    Array<HtmlError> errors;

    while (harnessReadCase(&test)) {
        if (test.fieldCount != 2) {
            fprintf(stderr, "a case names a state and a last start tag\n");
            return 2;
        }
        textClear(&out);
        run(&out, &tokenizer, test, &errors);
        harnessWriteLine(textView(&out));
    }

    htmlTokenizerReset(&tokenizer);
    errors.reset();
    harnessCaseReset(&test);
    textReset(&out);
    return 0;
}
