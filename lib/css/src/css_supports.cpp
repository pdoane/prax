#include "css/src/css_supports.h"

#include "pr/assert.h"
#include "pr/text_buffer.h"

#include "css/src/css_at_rule.h"
#include "css/src/css_keyword.h"
#include "css/src/css_parser.h"

namespace pr_impl {
namespace {

// Returns the index of the first value of `kind` in the subtree at `index`, or the subtree's end if there is none.
uint32_t findIn(CssValueRun run, uint32_t index, PrCssValueKind kind) {
    uint32_t end = index + run.values[index].next;
    uint32_t at = index + 1;
    while (at < end && run.values[at].kind != kind)
        ++at;
    return at;
}

// Returns true if the UA supports the <supports-feature> or <general-enclosed> at `index`.
bool featureSupported(CssValueRun run, uint32_t index) {
    const CssValueNode* values = run.values;
    switch (cssValueProduction(&values[index])) {
        case CssValueDef_SupportsDecl: {
            // The '(' is followed by the name of a declaration, or by a <supports-condition-name>. No named supports
            // condition exists for the name to refer to (css-conditional-5/#dfn-supports-condition-name).
            const CssValueNode* name = &values[index + 2];
            return name->kind == PrCssValueKind_Ident && (name->aux & CssValueAuxFlag_Supported) != 0;
        }
        case CssValueDef_SupportsSelectorFn:
            // The function matches only a supported selector, so a match is supported
            // (css-conditional-4/#dfn-support-selector).
            return true;
        case CssValueDef_SupportsFontTechFn:
            return cssFontTechSupported(cssValueKeyword(&values[findIn(run, index, PrCssValueKind_Keyword)]));
        case CssValueDef_SupportsFontFormatFn: {
            // css-conditional-5/#dfn-support-font-format: a format written as a <string> is not supported.
            uint32_t format = findIn(run, index, PrCssValueKind_Keyword);
            return format != index + values[index].next && cssFontFormatSupported(cssValueKeyword(&values[format]));
        }
        case CssValueDef_SupportsAtRuleFn: {
            // css-conditional-5/#dfn-support-at-rule: an at-rule is supported if it is known.
            const CssValueNode* name = &values[findIn(run, index, PrCssValueKind_Ident)];
            return cssAtRuleFromText(cssValueText(run, name)) != CssAtRule_None;
        }
        default:
            // No named feature (css-conditional-5/#dfn-support-named-feature) or environment variable
            // (css-conditional-5/#dfn-support-env) is supported, and <general-enclosed> is false
            // (css-conditional-3/#at-supports).
            return false;
    }
}

// Replaces the results of the condition's <supports-in-parens> with the condition's own result. Those results are the
// last entries of `results`, starting at the result of the condition's first child.
void combine(CssValueRun run, uint32_t index, RawArray<bool>* results) {
    const CssValueNode* values = run.values;
    uint32_t end = index + values[index].next;
    uint32_t operands = 0;
    PrCssKeyword op = PrCssKeyword_And;
    for (uint32_t at = index + 1; at < end; at += values[at].next) {
        if (values[at].kind == PrCssValueKind_Keyword)
            op = cssValueKeyword(&values[at]);
        else
            ++operands;
    }

    uint32_t first = results->count - operands;
    bool any = false;
    bool all = true;
    for (uint32_t i = first; i < results->count; ++i) {
        any = any || results->data[i];
        all = all && results->data[i];
    }
    results->count = first;
    results->push(op == PrCssKeyword_Not ? !all : op == PrCssKeyword_Or ? any : all);
}

// Returns true if `text` is a supported <supports-condition>.
bool evaluateText(CssParser* parser, PrStringView text) {
    CssTokenStream all = cssParserTokenize(parser, text);
    CssTokenStream ts = cssSubStream(&all, cssStreamTrimmed(&all));
    CssParseContext context = {nullptr};
    parser->pool.values.count = 0;
    parser->pool.text.count = 0;
    CssValueSpan matched;
    return cssParseProduction(CssValueDef_SupportsCondition, &ts, &context, &parser->pool, &parser->match, &matched) &&
        cssSupportsEvaluate(cssValuePoolRun(&parser->pool, matched));
}

} // namespace

bool cssFontFormatSupported(PrCssKeyword format) {
    return format != PrCssKeyword_EmbeddedOpentype && format != PrCssKeyword_Svg;
}

bool cssFontTechSupported(PrCssKeyword tech) {
    switch (tech) {
        case PrCssKeyword_FeaturesGraphite:
        case PrCssKeyword_ColorSvg:
        case PrCssKeyword_Incremental:
            return false;
        default:
            return true;
    }
}

bool cssSupportsEvaluate(CssValueRun condition) {
    // Reading from the end evaluates every <supports-in-parens> before the condition that holds it. Each one leaves its
    // result on the stack.
    RawArray<bool> results = {};
    for (uint32_t i = condition.count; i-- > 0;) {
        const CssValueNode* value = &condition.values[i];
        if (value->kind != PrCssValueKind_Production)
            continue;
        switch (cssValueProduction(value)) {
            case CssValueDef_SupportsCondition:
                combine(condition, i, &results);
                break;
            case CssValueDef_SupportsDecl:
            case CssValueDef_SupportsSelectorFn:
            case CssValueDef_SupportsFontTechFn:
            case CssValueDef_SupportsFontFormatFn:
            case CssValueDef_SupportsAtRuleFn:
            case CssValueDef_SupportsNamedFeatureFn:
            case CssValueDef_SupportsEnvFn:
            case CssValueDef_GeneralEnclosed:
                results.push(featureSupported(condition, i));
                break;
            default:
                break;
        }
    }
    PR_ASSERT(results.count == 1);
    bool supported = results.data[0];
    results.reset();
    return supported;
}

bool cssSupportsText(PrStringView text) {
    CssParser parser;
    cssParserInit(&parser);
    bool supported = evaluateText(&parser, text);
    if (!supported) {
        PrTextBuffer wrapped = {};
        prTextBufferAppendChar(&wrapped, '(');
        prTextBufferAppend(&wrapped, text);
        prTextBufferAppendChar(&wrapped, ')');
        supported = evaluateText(&parser, prTextBufferView(&wrapped));
        prTextBufferReset(&wrapped);
    }
    cssParserReset(&parser);
    return supported;
}

} // namespace pr_impl
