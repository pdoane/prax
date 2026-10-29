// The cascade of a document that has no presentation.

#include "svg/src/svg_cascade.h"

#include "pr/text_buffer.h"

#include "css/src/css_root.h"
#include "css/src/css_rule.h"
#include "css/src/css_serialize.h"
#include "dom/src/dom_flat.h"
#include "dom/src/dom_node.h"
#include "dom/src/dom_resource.h"
#include "dom/src/dom_use.h"
#include "svg/src/svg_style.h"

namespace pr_impl {

namespace {

// Returns the rule that gives the root what it inherits from the host: its color, and the custom properties that the
// host defines. A rule of the user agent's origin gives them as the initial values of inherited properties are given,
// and the document's own rules override them.
void hostRule(TextBuffer* out, const PrSvgDrawOptions& options) {
    textAppend(out, ":root{");
    if (options.hasColor) {
        textAppend(out, "color:color(srgb ");
        cssSerializeNumber(out, double(options.color.r));
        textAppendChar(out, ' ');
        cssSerializeNumber(out, double(options.color.g));
        textAppendChar(out, ' ');
        cssSerializeNumber(out, double(options.color.b));
        textAppend(out, " / ");
        cssSerializeNumber(out, double(options.color.a));
        textAppend(out, ");");
    }
    for (uint32_t i = 0; i < options.propertyCount; ++i) {
        textAppend(out, options.properties[i].name);
        textAppendChar(out, ':');
        textAppend(out, options.properties[i].value);
        textAppendChar(out, ';');
    }
    textAppendChar(out, '}');
}

uint64_t keyOf(const PrDomElement* element) {
    return uint64_t(reinterpret_cast<uintptr_t>(element));
}

} // namespace

void svgCascadeInit(SvgCascade* cascade, PrDomDocument* document, const PrSvgDrawOptions& options, PrVec2 viewport) {
    *cascade = SvgCascade{};
    cascade->document = document;
    // A document without a browsing context takes the documents that its references name from the host's loader. The
    // use elements' shadow trees, which the document keeps, are brought up to date before they are styled.
    domResourcesSetLoader(document, options.load, options.loadContext);
    domUsesSync(document);
    cascade->viewport = CssViewport{viewport.x, viewport.y};
    cascade->rootFont = kCssInitialFontBases;
    cssStyleTableInit(&cascade->table);
    cascade->builder.table = &cascade->table;
    CssSheetData* svg = cssSheetDataCreate();
    cssSheetDataParse(svg, kSvgStyle, nullptr, CssOrigin_UserAgent);
    cascade->userAgent.push(svg);
    if (options.hasColor || options.propertyCount != 0) {
        TextBuffer rule{};
        hostRule(&rule, options);
        CssSheetData* host = cssSheetDataCreate();
        cssSheetDataParse(host, textView(&rule), nullptr, CssOrigin_UserAgent);
        cascade->userAgent.push(host);
        textReset(&rule);
    }
    Array<CssParsedUserAgentSheet> userAgent;
    for (uint32_t i = 0; i < cascade->userAgent.count; ++i)
        userAgent.push(CssParsedUserAgentSheet{cascade->userAgent.data[i], 0, CssAppearance_None});
    cssRuleSetBuildUserAgent(&cascade->userAgentRules, document, userAgent.data, userAgent.count, 0,
                             document->media);
    userAgent.reset();
    PrCssStyleSheetList* sheets = cssDocumentStyleSheets(document);
    CssTreeSheets tree{document, 0, sheets->sheets.data, sheets->sheets.count};
    cssRuleSetBuild(&cascade->rules, &cascade->userAgentRules, &tree, 1, document->media);
}

void svgCascadeReset(SvgCascade* cascade) {
    for (uint32_t i = 0; i < cascade->styles.count; ++i) {
        const SvgCascadeEntry& entry = cascade->styles.data[i];
        cssComputedStyleRelease(entry.style);
    }
    cascade->styles.reset();
    cascade->index.index.reset();
    cssCascadeScratchReset(&cascade->scratch);
    cssStyleBuilderReset(&cascade->builder);
    cssRuleSetReset(&cascade->rules);
    cssRuleSetReset(&cascade->userAgentRules);
    for (uint32_t i = 0; i < cascade->userAgent.count; ++i) {
        CssSheetData* sheet = cascade->userAgent.data[i];
        cssSheetDataRelease(sheet);
    }
    cascade->userAgent.reset();
    cssStyleTableReset(&cascade->table);
}

const CssComputedStyle* svgCascadeStyle(SvgCascade* cascade, const PrDomElement* element) {
    uint32_t at =
        cascade->index.find(keyOf(element), [&](uint32_t i) { return keyOf(cascade->styles.data[i].element); });
    if (at != kHashMissing)
        return cascade->styles.data[at].style;
    // The flat parent, so that a use element's instance root inherits from the use
    // (SVG2/struct.html#UseStyleInheritance).
    const PrDomNode* flat = domFlatParent(element);
    const PrDomElement* parent = flat != nullptr ? domElementFromNode(flat) : nullptr;
    const CssComputedStyle* parentStyle = parent != nullptr ? svgCascadeStyle(cascade, parent) : nullptr;
    const CssComputedStyle* style = svgCascadeStyleUnder(cascade, element, parentStyle);
    if (parent == nullptr)
        cascade->rootFont = cssStyleFontBases(style);
    cascade->index.insert(keyOf(element), cascade->styles.count);
    cascade->styles.push(SvgCascadeEntry{element, style});
    return style;
}

const CssComputedStyle* svgCascadeStyleUnder(SvgCascade* cascade, const PrDomElement* element,
                                             const CssComputedStyle* PR_NULLABLE parent) {
    cssStylePrepare(const_cast<PrDomElement*>(element));
    CssCascadeInput input{.element = element,
                          .parent = parent,
                          .rootFont = cascade->rootFont,
                          .viewport = cascade->viewport,
                          .fresh = false,
                          .matched = nullptr,
                          .pseudo = CssPseudo_None};
    return cssCascadeStyle(&cascade->rules, &cascade->builder, &cascade->scratch, input);
}

const CssComputedStyle* PR_NULLABLE svgCascadePseudoStyleUnder(SvgCascade* cascade, const PrDomElement* element,
                                                               CssPseudo pseudo,
                                                               const CssComputedStyle* PR_NULLABLE parent) {
    cssStylePrepare(const_cast<PrDomElement*>(element));
    if ((cssCascadeOriginating(&cascade->rules, &cascade->scratch, element) & (1u << pseudo)) == 0)
        return nullptr;
    CssCascadeInput input{.element = element,
                          .parent = parent,
                          .rootFont = cascade->rootFont,
                          .viewport = cascade->viewport,
                          .fresh = false,
                          .matched = nullptr,
                          .pseudo = pseudo};
    return cssCascadeStyle(&cascade->rules, &cascade->builder, &cascade->scratch, input);
}

} // namespace pr_impl
