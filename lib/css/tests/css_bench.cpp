// A CSS parsing benchmark that measures the engine directly, without a shell around it.
//
// ./dev.sh bench css-parse runs this and prints its rows beside the rows that pr_shell reports. The measurements match
// the ones that tools/bench/scripts/css-parse.js makes and are taken the same way, so the two sets of rows compare
// directly. They measure the cost of replaceSync on its own, the cost of reading every declaration, and the cost of
// serializing every rule.
//
// Four measurements belong to this binary alone: each pass of the tokenizer, the two passes together, and the speed of
// reading the input once. Reading the input once is the bound that every pass over the source is measured against.

#include "pr/assert.h"
#include "pr/css.h"
#include "pr/string_ops.h"

#include "css/src/css_syntax.h"
#include "tools/harness/harness.h"

#include <stdio.h>

using namespace harness;

using namespace pr_impl;

namespace {

// The inputs of a measured sheet: the text, the sheet that it parsed to, and the tokens that the tokenizer writes.
struct Work {
    PrStringView text;
    PrCssStyleSheet* sheet;
    CssTokenArray tokens;
    CssTokenScratch scratch;
    volatile uint8_t folded;   // value that harnessReadBound folded the text into
};

// The numbers of rules and declarations that a sheet parsed to, counted over the whole tree.
struct Counts {
    uint32_t rules;
    uint32_t declarations;
};

void contents(PrCssRuleList* rules, Counts* out) {
    uint32_t count = prCssRuleListLength(rules);
    for (uint32_t i = 0; i < count; ++i) {
        PrCssRule* rule = prCssRuleListRuleAt(rules, i);
        ++out->rules;
        PrCssStyleRule* style = prCssStyleRuleQuery(rule);
        if (style != nullptr)
            out->declarations += prCssStyleDeclarationLength(
                prCssStylePropertiesAsCssStyleDeclaration(prCssStyleRuleStyle(style)));
        PrCssGroupingRule* group = prCssGroupingRuleQuery(rule);
        if (group != nullptr)
            contents(prCssGroupingRuleCssRules(group), out);
    }
}

// Serializes a sheet and returns the number of bytes.
uint32_t serializeAll(PrCssRuleList* rules) {
    uint32_t size = 0;
    uint32_t count = prCssRuleListLength(rules);
    for (uint32_t i = 0; i < count; ++i) {
        PrCssRule* rule = prCssRuleListRuleAt(rules, i);
        PrRawString text = prCssRuleCssText(rule);
        size += uint32_t(prRawStringView(&text).size);
        prRawStringReset(&text);

        PrCssGroupingRule* group = prCssGroupingRuleQuery(rule);
        if (group != nullptr)
            size += serializeAll(prCssGroupingRuleCssRules(group));
    }
    return size;
}

void parseOnly(Work* work) {
    PrCssStyleSheet* sheet = nullptr;
    prCssStyleSheetCreate(nullptr, nullptr, &sheet);
    prCssStyleSheetReplaceSync(sheet, work->text);
    prCssStyleSheetRelease(sheet);
}

void parseAndWalk(Work* work) {
    PrCssStyleSheet* sheet = nullptr;
    prCssStyleSheetCreate(nullptr, nullptr, &sheet);
    prCssStyleSheetReplaceSync(sheet, work->text);

    Counts counts = {0, 0};
    contents(prCssStyleSheetCssRules(sheet), &counts);
    prCssStyleSheetRelease(sheet);
}

void writeOnly(Work* work) {
    serializeAll(prCssStyleSheetCssRules(work->sheet));
}

void tokenizeOnly(Work* work) {
    cssTokenize(&work->tokens, &work->scratch, work->text);
}

void readOnly(Work* work) {
    work->folded = harnessReadBound(work->text);
}

// Returns the name of a row, which is the file's name without its extension.
PrStringView rowName(PrStringView file) {
    for (size_t size = file.size; size != 0; --size) {
        if (file.data[size - 1] == '.')
            return prStringSlice(file, 0, size - 1);
    }
    return file;
}

void report(PrStringView name, Work* work, uint32_t rules, uint32_t declarations, uint32_t out, bool first) {
    HarnessCounters parse = harnessMeasure([&] { parseOnly(work); });
    HarnessCounters ready = harnessMeasure([&] { parseAndWalk(work); });
    HarnessCounters write = harnessMeasure([&] { writeOnly(work); });
    HarnessCounters tokenize = harnessMeasure([&] { tokenizeOnly(work); });
    HarnessCounters bound = harnessMeasure([&] { readOnly(work); });
    HarnessCounts counts{};
    harnessCountsAdd(&counts, "parse", parse);
    harnessCountsAdd(&counts, "ready", ready);
    harnessCountsAdd(&counts, "write", write);
    harnessCountsAdd(&counts, "tokenize", tokenize);
    harnessCountsAdd(&counts, "bound", bound);
    PrTextBuffer row{};
    harnessAppendFormat(&row, "%s\n  {\"name\": \"%.*s\", \"bytes\": %u, \"out\": %u, \"rules\": %u,"
        " \"declarations\": %u, \"parse\": %.6f, \"ready\": %.6f, \"write\": %.6f, \"tokenize\": %.6f, \"bound\": %.6f",
        first ? "" : ",", int(name.size), name.data, uint32_t(work->text.size), out, rules,
        declarations, parse.millis, ready.millis, write.millis, tokenize.millis, bound.millis);
    harnessCountsWrite(&counts, &row);
    prTextBufferAppend(&row, "}");
    fwrite(row.data, 1, row.size, stdout);
    prTextBufferReset(&row);
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    PrTextBuffer file = {};
    printf("[");
    for (int i = 1; i < argc; ++i) {
        if (!harnessReadFile(&file, argv[i])) {
            fprintf(stderr, "cannot read %s\n", argv[i]);
            return 2;
        }
        PrStringView text = prTextBufferView(&file);

        PrCssStyleSheet* sheet = nullptr;

        prCssStyleSheetCreate(nullptr, nullptr, &sheet);
        prCssStyleSheetReplaceSync(sheet, text);

        Work work = {text, sheet, {}, {}, 0};
        Counts counts = {0, 0};
        contents(prCssStyleSheetCssRules(sheet), &counts);

        report(rowName(harnessFileName(argv[i])), &work, counts.rules, counts.declarations,
            serializeAll(prCssStyleSheetCssRules(sheet)), i == 1);

        cssTokenArrayReset(&work.tokens);
        cssTokenScratchReset(&work.scratch);
        prCssStyleSheetRelease(sheet);
    }
    prTextBufferReset(&file);
    printf("\n]\n");
    return 0;
}
