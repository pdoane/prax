// The mutation benchmark's cases through the C API, without a shell.
//
// ./dev.sh bench mutation runs this and prints its rows beside the rows that pr_shell reports. Each case is the one
// that tools/bench/scripts/mutation.js names. It does through C what the page does through the DOM and the CSSOM,
// batched and measured the same way. The difference between the two rows is therefore the cost of the binding and
// QuickJS. The document belongs to a browsing context, as the page's does, with no scripting.
//
// Usage: mutation_bench <small.css> <large.css> [case]
//
// If a case is named, only that case runs, repeatedly, for five minutes (harnessRepeat). This is for attaching a
// profiler.

#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/css.h"
#include "pr/html.h"
#include "pr/html_page.h"
#include "pr/print.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"

#include "tools/harness/harness.h"

using namespace harness;

using namespace pr_impl;

namespace {

// The data that the cases work on, as bench.js's `cases` holds it.
struct Work {
    PrDomDocument* document;
    PrDomNode* host;
    PrDomNode* other;
    PrStringView html;
    PrStringView small;
    PrStringView large;
    PrTextBuffer unique;          // the text that `unique` made last
    uint32_t uniqueCount;
    PrDomNode* PR_NULLABLE subtree;
    PrDomNode* PR_NULLABLE detached[2];
    PrCssStyleSheet* PR_NULLABLE sheet;
    PrCssRule* PR_NULLABLE rule;
    PrDomNode* PR_NULLABLE style;
    uint32_t index;
    bool flip;
};

typedef void (*BodyFn)(Work* work);

PrDomElement* element(Work* work, PrStringView name) {
    PrDomElement* made = nullptr;
    prDomElementCreate(work->document, name, nullptr, &made);
    return made;
}

// Appends a node that the caller created, and releases the caller's reference.
void adopt(PrDomNode* parent, PrDomNode* child) {
    prDomNodeAppendChild(parent, child);
    prDomNodeRelease(child);
}

// Builds a tree of `depth` levels below `root`, with `fanout` elements per parent, and text in each leaf.
PrDomNode* build(Work* work, PrDomNode* root, uint32_t depth, uint32_t fanout) {
    for (uint32_t i = 0; i < fanout; ++i) {
        PrDomElement* child = element(work, depth > 1 ? "div" : "span");
        prDomElementSetClassName(child, "item");
        if (depth > 1)
            build(work, prDomElementAsNode(child), depth - 1, fanout);
        else
            adopt(prDomElementAsNode(child), prDomTextAsNode(prDomTextCreate(work->document, "text")));
        adopt(root, prDomElementAsNode(child));
    }
    return root;
}

PrDomNode* tree(Work* work) {
    return build(work, prDomElementAsNode(element(work, "div")), 3, 10);
}

// The count that a walk produced, stored where the optimizer cannot remove the walk.
volatile uint32_t walked;

uint32_t walk(PrDomNode* node) {
    uint32_t count = 1;
    for (PrDomNode* child = prDomNodeFirstChild(node); child != nullptr; child = prDomNodeNextSibling(child))
        count += walk(child);
    return count;
}

// Writes the markup of a tree that `build` makes.
void markup(PrTextBuffer* out, uint32_t depth, uint32_t fanout) {
    PrStringView tag = depth > 1 ? "div" : "span";
    for (uint32_t i = 0; i < fanout; ++i) {
        prTextBufferAppend(out, "<");
        prTextBufferAppend(out, tag);
        prTextBufferAppend(out, " class=\"item\">");
        if (depth > 1)
            markup(out, depth - 1, fanout);
        else
            prTextBufferAppend(out, "text");
        prTextBufferAppend(out, "</");
        prTextBufferAppend(out, tag);
        prTextBufferAppend(out, ">");
    }
}

// Releases what the last case kept.
void clear(Work* work) {
    prDomNodeRelease(work->subtree);
    prDomNodeRelease(work->detached[0]);
    prDomNodeRelease(work->detached[1]);
    prCssStyleSheetRelease(work->sheet);
    prDomNodeRelease(work->style);
    work->style = nullptr;
    work->subtree = nullptr;
    work->detached[0] = nullptr;
    work->detached[1] = nullptr;
    work->sheet = nullptr;
    work->rule = nullptr;
}

// Returns `text` with a comment after it that differs at every call, so that no cache holds a parse of it. bench.js's
// `unique` makes the same text.
PrStringView unique(Work* work, PrStringView text) {
    prTextBufferClear(&work->unique);
    prTextBufferAppend(&work->unique, text);
    harnessAppendFormat(&work->unique, "\n/* %u */", ++work->uniqueCount);
    return prTextBufferView(&work->unique);
}

PrCssStyleSheet* sheetOf(PrStringView text) {
    PrCssStyleSheet* sheet = prCssStyleSheetCreate(nullptr);
    prCssStyleSheetReplaceSync(sheet, text);
    return sheet;
}

PrCssStyleDeclaration* ruleStyle(PrCssRule* rule) {
    return prCssStylePropertiesAsCssStyleDeclaration(prCssStyleRuleStyle(prCssStyleRuleQuery(rule)));
}

// Creates a style element that holds the large stylesheet, whose data the document's cache shares.
PrDomNode* cachedStyle(Work* work) {
    PrDomNode* style = prDomElementAsNode(element(work, "style"));
    prDomNodeAppendChild(prDomElementAsNode(prDomDocumentHead(work->document)), style);
    prDomNodeSetTextContent(style, work->large);
    return style;
}

PrCssRuleList* cachedRules(Work* work) {
    PrHtmlStyleElement* style = prHtmlStyleElementQuery(prDomNodeAsEventTarget(work->style));
    return prCssStyleSheetCssRules(prHtmlStyleElementSheet(style));
}

// Gives a color that it does not have to the large stylesheet's rule `index`, which is a style rule, or to the first
// rule of the @media rule `index`.
void editCached(Work* work, bool nested) {
    prDomNodeSetTextContent(work->style, work->large);
    PrCssRule* rule = prCssRuleListRuleAt(cachedRules(work), work->index);
    if (nested)
        rule = prCssRuleListRuleAt(prCssGroupingRuleCssRules(prCssGroupingRuleQuery(rule)), 0);
    PrCssDeclarationName color = prCssDeclarationNameFromProperty(PrCssProperty_Color);
    prCssStyleDeclarationSetProperty(ruleStyle(rule), &color, "rgb(1, 2, 3)", false);
}

struct Case {
    const char* name;
    void (*setup)(Work* work);
    BodyFn run;
};

constexpr Case kCases[] = {
    {"node build detached", [](Work*) {}, [](Work* w) { prDomNodeRelease(tree(w)); }},
    {
        "node build insert remove",
        [](Work*) {},
        [](Work* w) {
            PrDomNode* made = tree(w);
            prDomNodeAppendChild(w->host, made);
            prDomNodeRemove(made);
            prDomNodeRelease(made);
        },
    },
    {
        "node move detached",
        [](Work* w) {
            w->subtree = tree(w);
            w->detached[0] = prDomElementAsNode(element(w, "div"));
            w->detached[1] = prDomElementAsNode(element(w, "div"));
        },
        [](Work* w) {
            w->flip = !w->flip;
            prDomNodeAppendChild(w->detached[w->flip ? 1 : 0], w->subtree);
        },
    },
    {
        "node move connected",
        [](Work* w) {
            w->subtree = tree(w);
            prDomNodeAppendChild(w->host, w->subtree);
        },
        [](Work* w) {
            w->flip = !w->flip;
            prDomNodeAppendChild(w->flip ? w->other : w->host, w->subtree);
        },
    },
    {
        "node walk",
        [](Work* w) {
            w->subtree = tree(w);
            prDomNodeAppendChild(w->host, w->subtree);
        },
        [](Work* w) { walked = walk(w->subtree); },
    },
    {
        "node clone",
        [](Work* w) { w->subtree = tree(w); },
        [](Work* w) {
            PrDomNode* copy = nullptr;
            prDomNodeCloneNode(w->subtree, true, &copy);
            prDomNodeRelease(copy);
        },
    },
    {"node innerHTML", [](Work*) {}, [](Work* w) { prHtmlSetInnerHtml(w->host, w->html); }},
    {
        "node attributes",
        [](Work* w) {
            w->subtree = prDomElementAsNode(element(w, "div"));
            prDomNodeAppendChild(w->host, w->subtree);
        },
        [](Work* w) {
            PrDomElement* e = prDomElementQuery(prDomNodeAsEventTarget(w->subtree));
            prDomElementSetAttribute(e, "data-x", "1");
            prDomElementSetAttribute(e, "title", "t");
            prDomElementRemoveAttribute(e, "data-x");
            prDomElementRemoveAttribute(e, "title");
        },
    },
    {
        "node range extract",
        [](Work* w) {
            w->subtree = tree(w);
            prDomNodeAppendChild(w->host, w->subtree);
        },
        [](Work* w) {
            PrDomRange* range = prDomRangeCreate(w->document);
            prDomRangeSelectNodeContents(range, w->subtree);
            PrDomDocumentFragment* fragment = nullptr;
            prDomRangeExtractContents(range, &fragment);
            adopt(w->subtree, prDomDocumentFragmentAsNode(fragment));
            prDomRangeRelease(range);
        },
    },
    {
        "node shadow build",
        [](Work*) {},
        [](Work* w) {
            PrDomElement* hostElement = element(w, "div");
            PrDomShadowRootInit init = prDomShadowRootInitDefault(PrDomShadowRootMode_Open);
            PrDomShadowRoot* shadow = nullptr;
            prDomElementAttachShadow(hostElement, &init, &shadow);
            build(w, prDomShadowRootAsNode(shadow), 2, 10);
            prDomNodeRelease(prDomElementAsNode(hostElement));
        },
    },
    {
        "node textContent",
        [](Work* w) { prHtmlSetInnerHtml(w->host, w->html); },
        [](Work* w) {
            prDomNodeSetTextContent(w->host, "");
            prHtmlSetInnerHtml(w->host, w->html);
        },
    },
    {
        "sheet replaceSync",
        [](Work* w) { w->sheet = sheetOf(w->small); },
        [](Work* w) { prCssStyleSheetReplaceSync(w->sheet, unique(w, w->small)); },
    },
    {
        "sheet replaceSync read",
        [](Work* w) { w->sheet = sheetOf(w->small); },
        [](Work* w) {
            prCssStyleSheetReplaceSync(w->sheet, unique(w, w->small));
            PrCssRuleList* rules = prCssStyleSheetCssRules(w->sheet);
            for (uint32_t i = 0; i < prCssRuleListLength(rules); ++i) {
                PrRawString text = prCssRuleCssText(prCssRuleListRuleAt(rules, i));
                prRawStringReset(&text);
            }
        },
    },
    {
        "sheet insert delete",
        [](Work* w) { w->sheet = sheetOf(w->large); },
        [](Work* w) {
            prCssStyleSheetInsertRule(w->sheet, ".item > span:hover { color: red; margin: 1px 2px }", 0, nullptr);
            prCssStyleSheetDeleteRule(w->sheet, 0);
        },
    },
    {
        "sheet insert delete read",
        [](Work* w) { w->sheet = sheetOf(w->large); },
        [](Work* w) {
            prCssStyleSheetInsertRule(w->sheet, ".item > span:hover { color: red; margin: 1px 2px }", 0, nullptr);
            PrCssRule* rule = prCssRuleListRuleAt(prCssStyleSheetCssRules(w->sheet), 0);
            PrRawString text = prCssStyleRuleSelectorText(prCssStyleRuleQuery(rule));
            prRawStringReset(&text);
            prCssStyleSheetDeleteRule(w->sheet, 0);
        },
    },
    {
        "sheet style text",
        [](Work* w) {
            w->style = prDomElementAsNode(element(w, "style"));
            prDomNodeAppendChild(prDomElementAsNode(prDomDocumentHead(w->document)), w->style);
            prDomNodeSetTextContent(w->style, w->small);
        },
        [](Work* w) { prDomNodeSetTextContent(w->style, unique(w, w->small)); },
    },
    {
        "sheet style insert remove",
        [](Work* w) {
            w->style = prDomElementAsNode(element(w, "style"));
            prDomNodeSetTextContent(w->style, w->small);
            prDomNodeAppendChild(prDomElementAsNode(prDomDocumentHead(w->document)), w->style);
            prDomNodeRemove(w->style);
        },
        [](Work* w) {
            prDomNodeAppendChild(prDomElementAsNode(prDomDocumentHead(w->document)), w->style);
            prDomNodeRemove(w->style);
        },
    },
    {
        "sheet cached text",
        [](Work* w) { w->style = cachedStyle(w); },
        [](Work* w) { prDomNodeSetTextContent(w->style, w->large); },
    },
    {
        "sheet cached edit",
        [](Work* w) {
            w->style = cachedStyle(w);
            PrCssRuleList* rules = cachedRules(w);
            w->index = prCssRuleListLength(rules) >> 1;
            while (prCssStyleRuleQuery(prCssRuleListRuleAt(rules, w->index)) == nullptr)
                ++w->index;
        },
        [](Work* w) { editCached(w, false); },
    },
    {
        "sheet cached nested edit",
        [](Work* w) {
            w->style = cachedStyle(w);
            PrCssRuleList* rules = cachedRules(w);
            w->index = 0;
            for (;; ++w->index) {
                PrCssMediaRule* media = prCssMediaRuleQuery(prCssRuleListRuleAt(rules, w->index));
                if (media == nullptr)
                    continue;
                PrCssRuleList* inner =
                    prCssGroupingRuleCssRules(prCssGroupingRuleQuery(prCssRuleListRuleAt(rules, w->index)));
                if (prCssRuleListLength(inner) != 0 && prCssStyleRuleQuery(prCssRuleListRuleAt(inner, 0)) != nullptr)
                    break;
            }
        },
        [](Work* w) { editCached(w, true); },
    },
    {
        "sheet nested insert delete",
        [](Work* w) {
            w->sheet = sheetOf("@media print { a { color: red } }");
            w->rule = prCssRuleListRuleAt(prCssStyleSheetCssRules(w->sheet), 0);
        },
        [](Work* w) {
            PrCssGroupingRule* group = prCssGroupingRuleQuery(w->rule);
            prCssGroupingRuleInsertRule(group, ".item b { margin: 1px }", 0, nullptr);
            PrCssDeclarationName margin = prCssDeclarationNameFromProperty(PrCssProperty_Margin);
            PrRawString value = prCssStyleDeclarationGetPropertyValue(
                ruleStyle(prCssRuleListRuleAt(prCssGroupingRuleCssRules(group), 0)), &margin);
            prRawStringReset(&value);
            prCssGroupingRuleDeleteRule(group, 0);
        },
    },
    {
        "rule selectorText",
        [](Work* w) {
            w->sheet = sheetOf(".item { color: red }");
            w->rule = prCssRuleListRuleAt(prCssStyleSheetCssRules(w->sheet), 0);
        },
        [](Work* w) {
            w->flip = !w->flip;
            prCssStyleRuleSetSelectorText(prCssStyleRuleQuery(w->rule), w->flip ? ".item > span:hover" : ".item");
        },
    },
    {
        "rule cssText",
        [](Work* w) {
            w->sheet = sheetOf(".item { color: red }");
            w->rule = prCssRuleListRuleAt(prCssStyleSheetCssRules(w->sheet), 0);
        },
        [](Work* w) {
            w->flip = !w->flip;
            prCssStyleDeclarationSetCssText(ruleStyle(w->rule), w->flip ? "margin: 1px 2px; color: red" :
                "padding: 3px; display: block");
        },
    },
    {
        "rule setProperty",
        [](Work* w) {
            w->sheet = sheetOf(".item { color: red; margin: 1px 2px; display: block }");
            w->rule = prCssRuleListRuleAt(prCssStyleSheetCssRules(w->sheet), 0);
        },
        [](Work* w) {
            w->flip = !w->flip;
            PrCssDeclarationName margin = prCssDeclarationNameFromProperty(PrCssProperty_Margin);
            prCssStyleDeclarationSetProperty(ruleStyle(w->rule), &margin, w->flip ? "3px 4px" : "1px 2px", false);
        },
    },
    {
        "rule setProperty large",
        [](Work* w) {
            w->sheet = sheetOf(".item { --payload: [a b c d e f g h i j k l m n o p q r s t u v w x y z] "
                "[a b c d e f g h i j k l m n o p q r s t u v w x y z] [a b c d e f g h i j k l m n o p q r s t u v w x y z] "
                "[a b c d e f g h i j k l m n o p q r s t u v w x y z]; background: linear-gradient(red, blue), "
                "linear-gradient(45deg, rgb(1, 2, 3) 10%, transparent 90%), "
                "radial-gradient(circle at 20% 30%, green, yellow 40%, blue), url(tile.png) repeat-x; color: red; opacity: 1 }");
            w->rule = prCssRuleListRuleAt(prCssStyleSheetCssRules(w->sheet), 0);
        },
        [](Work* w) {
            w->flip = !w->flip;
            PrCssDeclarationName opacity = prCssDeclarationNameFromProperty(PrCssProperty_Opacity);
            prCssStyleDeclarationSetProperty(ruleStyle(w->rule), &opacity, w->flip ? "0.5" : "1", false);
        },
    },
    {
        "inline setProperty",
        [](Work* w) {
            w->subtree = prDomElementAsNode(element(w, "div"));
            prDomNodeAppendChild(w->host, w->subtree);
        },
        [](Work* w) {
            w->flip = !w->flip;
            PrDomElement* e = prDomElementQuery(prDomNodeAsEventTarget(w->subtree));
            PrCssDeclarationName margin = prCssDeclarationNameFromProperty(PrCssProperty_Margin);
            prCssStyleDeclarationSetProperty(prCssStylePropertiesAsCssStyleDeclaration(prCssElementStyle(e)), &margin,
                w->flip ? "3px 4px" : "1px 2px", false);
        },
    },
};

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    if (argc != 3 && argc != 4) {
        printError("usage: mutation_bench <small.css> <large.css> [case]\n");
        return 2;
    }
    PrTextBuffer small = {};
    PrTextBuffer large = {};
    if (!harnessReadFile(&small, argv[1]) || !harnessReadFile(&large, argv[2])) {
        printError("mutation_bench: cannot read the stylesheets\n");
        return 1;
    }
    PrTextBuffer html = {};
    markup(&html, 3, 10);

    PrHtmlPage* page = prHtmlPageCreate(nullptr);
    prHtmlPageLoad(page, "<!doctype html><body>", "about:blank", nullptr);
    Work work = {};
    work.document = prHtmlWindowDocument(prHtmlPageWindow(page));
    PrDomNode* body = prDomElementAsNode(prDomDocumentBody(work.document));
    work.host = prDomElementAsNode(element(&work, "div"));
    work.other = prDomElementAsNode(element(&work, "div"));
    adopt(body, work.host);
    adopt(body, work.other);
    work.html = prTextBufferView(&html);
    work.small = prTextBufferView(&small);
    work.large = prTextBufferView(&large);

    PrStringView only = argc == 4 ? argv[3] : PrStringView{};
    // The array's opening reaches the output before the harness writes to it (harnessWriteCase, harnessWriteLine),
    // which buffers what it writes, so it stays first.
    print("[");
    for (uint32_t i = 0; i < countOf(kCases); ++i) {
        const Case& c = kCases[i];
        if (only.data != nullptr) {
            if (only != c.name)
                continue;
            c.setup(&work);
            harnessRepeat([&] { c.run(&work); });
        }
        c.setup(&work);
        harnessWriteCase(i == 0, c.name, harnessMeasure([&] { c.run(&work); }));
        clear(&work);
    }
    harnessWriteLine("\n]");

    prHtmlPageDestroy(page);
    prTextBufferReset(&html);
    prTextBufferReset(&work.unique);
    prTextBufferReset(&small);
    prTextBufferReset(&large);
    return 0;
}
