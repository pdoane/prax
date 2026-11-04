// Page-margin boxes: what a paged presentation gives the margins of its pages.

#include "css/src/css_presentation_page.h"

#include "pr/alloc.h"
#include "pr/hash_index.h"
#include "pr/layout_page.h"
#include "pr/layout_style.h"
#include "pr/mem.h"
#include "pr/search.h"
#include "pr/sort.h"
#include "pr/text_buffer.h"

#include "css/src/css_at_rule.h"
#include "css/src/css_counter.h"
#include "css/src/css_counter_pass.h"
#include "css/src/css_rule.h"
#include "css/src/css_style.h"
#include "css/src/css_value.h"
#include "dom/src/dom_flat.h"
#include "dom/src/dom_node.h"

namespace pr_impl {

namespace {

// The margin rule of each margin box (css-page-3/#margin-at-rules), by PrLayoutMarginBox.
constexpr CssAtRule kMarginRules[PrLayoutMarginBox_Count] = {
    CssAtRule_TopLeftCorner, CssAtRule_TopLeft, CssAtRule_TopCenter, CssAtRule_TopRight, CssAtRule_TopRightCorner,
    CssAtRule_RightTop, CssAtRule_RightMiddle, CssAtRule_RightBottom,
    CssAtRule_BottomRightCorner, CssAtRule_BottomRight, CssAtRule_BottomCenter, CssAtRule_BottomLeft,
    CssAtRule_BottomLeftCorner, CssAtRule_LeftBottom, CssAtRule_LeftMiddle, CssAtRule_LeftTop,
};

// The declarations of the user agent's sheet for each margin box (css-page-3/#margin-text-alignment), by
// PrLayoutMarginBox.
constexpr const char* kMarginDefaults[PrLayoutMarginBox_Count] = {
    "text-align: right; vertical-align: middle",
    "text-align: left; vertical-align: middle",
    "text-align: center; vertical-align: middle",
    "text-align: right; vertical-align: middle",
    "text-align: left; vertical-align: middle",
    "text-align: center; vertical-align: top",
    "text-align: center; vertical-align: middle",
    "text-align: center; vertical-align: bottom",
    "text-align: left; vertical-align: middle",
    "text-align: right; vertical-align: middle",
    "text-align: center; vertical-align: middle",
    "text-align: left; vertical-align: middle",
    "text-align: right; vertical-align: middle",
    "text-align: center; vertical-align: bottom",
    "text-align: center; vertical-align: middle",
    "text-align: center; vertical-align: top",
};

// The styles of one kind of page (CssPageBoxes::boxes): its page context's, and those of the margin boxes that it
// generates, each with a reference, or null for a box that it does not generate, with their records, by
// PrLayoutMarginBox.
struct KindStyles {
    const CssComputedStyle* PR_NULLABLE context;
    const CssComputedStyle* PR_NULLABLE boxes[PrLayoutMarginBox_Count];
    PrLayoutStyleIndex records[PrLayoutMarginBox_Count];
};

// An assignment of a named string or a running element, in tree order
// (css-gcpm-3/#setting-named-strings-the-string-set-pro, css-gcpm-3/#running-elements): its name and value as ranges of
// the pass's text, or the running element, the page that it is on, and whether nothing on its page comes before it.
struct Assignment {
    uint32_t order;                 // its place in tree order among the assignments
    uint32_t nameStart;
    uint32_t nameEnd;
    uint32_t valueStart;
    uint32_t valueEnd;
    const PrDomElement* PR_NULLABLE element;
    uint32_t page;
    bool start;
};

// A counter instance of the document in the walk (css-lists-3/#creating-counters): its name as a range of the pass's
// text, its value, and the depth of the element whose children it is in scope among.
struct Instance {
    uint32_t nameStart;
    uint32_t nameEnd;
    int32_t value;
    uint32_t owner;
};

// The value of a counter that a page context or a margin context changes (css-page-3/#page-based-counters).
struct PageCounter {
    PrStringView name;
    int32_t value;
};

// What one pass over the pages reads and makes.
struct Pass {
    PrCssPresentation* r;
    PrLayoutTree* tree;
    uint32_t pages;
    RawArray<KindStyles> kinds;     // by kind of page
    RawArray<char> text;            // the names and values of the assignments and the counters

    // What the margin boxes read of the document, which the walk finds: named strings, running elements, and the
    // document's counters at the start of each page.
    bool readsStrings;
    bool readsElements;
    bool readsCounters;
    RawArray<Assignment> strings;
    RawArray<Assignment> elements;

    // The counters of the document in scope at the start of each page, outermost first: for each page, the start and
    // the end of its range of `snapshots`.
    RawArray<uint32_t> snapshotStarts;      // two by page
    RawArray<Instance> snapshots;

    // The walk's state: the counter instances in scope, innermost last, the node of each page's first content, the
    // pages whose first content the walk has reached, and those whose document counters it has taken.
    RawArray<Instance> scope;
    RawArray<const PrDomNode*> firsts;      // by page
    KeyIndex firstIndex;                    // pages by their first content's node
    RawArray<bool> reached;                 // by page
    RawArray<bool> started;                 // by page

    // The element at the start of each page, whose counters are the page's document counters
    // (css-page-3/#page-based-counters): the outermost ancestor of its first content that starts on the page and spans
    // no break after it.
    RawArray<const PrDomNode*> starts;      // by page
    KeyIndex startIndex;                    // pages by the node at their start

    // The counters that page contexts change, which persist across pages, and those that each margin box's context
    // changes, by PrLayoutMarginBox.
    RawArray<PageCounter> pageCounters;
    RawArray<PageCounter> boxCounters[PrLayoutMarginBox_Count];
    int32_t page;                           // the page counter
};

// Returns the view of the range of the pass's text.
PrStringView textOf(const Pass& p, uint32_t start, uint32_t end) {
    return PrStringView{p.text.data + start, end - start};
}

// Appends `value` to the pass's text and returns its start.
uint32_t addText(Pass& p, PrStringView value) {
    uint32_t start = p.text.count;
    p.text.append(value.data, uint32_t(value.size));
    return start;
}

// Calls `fn` with each item of a computed content list in order, up to its alternative text.
template <typename Fn>
void eachItem(CssValueRun content, Fn fn) {
    for (uint32_t i = 0; i < content.count; i += content.values[i].next) {
        const CssValueNode* item = &content.values[i];
        if (cssValueIsLiteral(item, '/'))
            break;
        fn(i, item);
        if (item->next == 0)
            break;
    }
}

// Returns the keyword of the second argument of the function at `at`, which names which assignment of a page a named
// string or a running element takes, or `otherwise` if it has none.
PrCssKeyword whichOf(CssValueRun run, uint32_t at, PrCssKeyword otherwise) {
    uint32_t starts[2];
    uint32_t count = cssValueArguments(run, at, starts, 2);
    if (count < 2 || run.values[starts[1]].kind != PrCssValueKind_Keyword)
        return otherwise;
    return cssValueKeyword(&run.values[starts[1]]);
}

// Returns the name that the first argument of the function at `at` gives.
PrStringView nameOf(CssValueRun run, uint32_t at) {
    uint32_t starts[1];
    if (cssValueArguments(run, at, starts, 1) == 0)
        return PrStringView{};
    return cssValueText(run, &run.values[starts[0]]);
}

// Notes what the content of a margin box reads of the document.
void noteReads(Pass& p, const CssComputedStyle* style) {
    CssValueRun content = cssComputedValue(style, PrCssProperty_Content);
    eachItem(content, [&](uint32_t i, const CssValueNode* item) {
        if (cssValueIsFunction(item, PrCssKeyword_String))
            p.readsStrings = true;
        else if (cssValueIsFunction(item, PrCssKeyword_Element))
            p.readsElements = true;
        else if (cssValueIsFunction(item, PrCssKeyword_Counter) || cssValueIsFunction(item, PrCssKeyword_Counters)) {
            PrStringView name = nameOf(content, i);
            if (name != "page" && name != "pages")
                p.readsCounters = true;
        }
    });
}

// Returns the user agent's declarations of the margin box `which`, parsed on first use.
const CssDeclarationBlock* defaultsOf(CssPageMargins& margins, PrLayoutMarginBox which) {
    if (margins.defaults[which] == nullptr) {
        CssDeclarationBlock* block = allocZeroed<CssDeclarationBlock>();
        cssDeclarationBlockParse(block, kMarginDefaults[which], nullptr, CssOrigin_UserAgent);
        margins.defaults[which] = block;
    }
    return margins.defaults[which];
}

// Returns the record of a margin box styled by `style`, whose display makes it a page-margin box. Its position and
// float do not apply (css-page-3/#margin-boxes).
PrLayoutStyleIndex marginRecordOf(Pass& p, const CssComputedStyle* style) {
    PrLayoutStyle record = prLayoutStyleRecord(p.tree, cssPresentationRecord(p.r, style));
    PrLayoutBoxStyle box = prLayoutBoxStyleRecord(p.tree, record.groups[PrLayoutGroup_Box]);
    box.display = PrLayoutDisplay_PageMargin;
    box.position = PrLayoutPosition_Static;
    box.floating = PrLayoutFloat_None;
    record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(p.tree, &box);
    return prLayoutStyleIntern(p.tree, &record);
}

// Finds the styles of each kind of page of `boxes`: its page context's, from the declarations of the rules that apply
// to it, under the root's, and its margin boxes', each from the user agent's declarations and those of the margin rules
// of the rules that apply, under its page context's. Returns true if a kind generates a margin box.
bool findStyles(Pass& p, const CssPageBoxes& boxes) {
    const CssComputedStyle* root = cssPresentationStyle(p.r, prDomDocumentElement(p.r->document));
    RawArray<const CssDeclarationBlock*> blocks;
    bool any = false;
    for (uint32_t k = 0; k < boxes.boxes.count; ++k) {
        const CssPageBox& kind = boxes.boxes.data[k];
        KindStyles& styles = p.kinds.emplace();
        styles = KindStyles{};
        bool margins = false;
        for (uint32_t i = 0; i < kind.rules.count && !margins; ++i)
            margins = kind.rules.data[i]->children.count != 0;
        if (!margins)
            continue;
        blocks.clear();
        for (uint32_t i = 0; i < kind.rules.count; ++i)
            blocks.push(&kind.rules.data[i]->style);
        styles.context = cssPresentationStyleOfBlocks(p.r, root, CssPseudo_None, blocks.data, blocks.count);
        for (PrLayoutMarginBox which = 0; which < PrLayoutMarginBox_Count; ++which) {
            blocks.clear();
            blocks.push(defaultsOf(p.r->pageMargins, which));
            for (uint32_t i = 0; i < kind.rules.count; ++i) {
                const CssRuleData* rule = kind.rules.data[i];
                for (uint32_t c = 0; c < rule->children.count; ++c) {
                    const CssRuleData* child = rule->children.data[c];
                    if (child->kind == CssRuleDataKind_At && kCssAtRuleAlias[child->atRule] == kMarginRules[which])
                        blocks.push(&child->style);
                }
            }
            if (blocks.count == 1)
                continue;
            // A margin box computes its content as ::before does: normal computes to none, and generates no box
            // (css-page-3/#populating-margin-boxes).
            const CssComputedStyle* style =
                cssPresentationStyleOfBlocks(p.r, styles.context, CssPseudo_Before, blocks.data, blocks.count);
            if (cssComputedFirstKeyword(style, PrCssProperty_Content) == PrCssKeyword_None) {
                cssComputedStyleRelease(style);
                continue;
            }
            styles.boxes[which] = style;
            styles.records[which] = marginRecordOf(p, style);
            noteReads(p, style);
            any = true;
        }
    }
    blocks.reset();
    return any;
}

// Returns the node of the document that the box `node` presents, or that the nearest box around it presents. A
// pseudo-element stands for the element that originates it.
const PrDomNode* PR_NULLABLE documentNodeOf(const Pass& p, PrLayoutNode node) {
    for (PrLayoutNode at = node; at != kPrLayoutNodeNone; at = prLayoutNodeParent(p.tree, at)) {
        const PrDomNode* found = cssPresentationNodeFromLayout(p.r, at);
        while (found != nullptr && found->type == DomTargetType_PseudoElement)
            found = found->parent;
        if (found != nullptr)
            return found;
    }
    return nullptr;
}

// Returns the innermost instance of the counter `name` in scope, or UINT32_MAX for none.
uint32_t instanceOf(const Pass& p, PrStringView name) {
    for (uint32_t i = p.scope.count; i-- > 0;) {
        if (textOf(p, p.scope.data[i].nameStart, p.scope.data[i].nameEnd) == name)
            return i;
    }
    return UINT32_MAX;
}

// Applies the counter properties of an element at `depth` styled by `style` to the counters in scope, in the order that
// css-lists-3/#auto-numbering gives: counter-reset, counter-set, then counter-increment. A list item increments
// list-item unless it names it.
void count(Pass& p, const CssComputedStyle* style, uint32_t depth) {
    auto instantiate = [&](PrStringView name, int32_t value) {
        uint32_t start = addText(p, name);
        p.scope.push(Instance{start, p.text.count, value, depth - 1});
        return p.scope.count - 1;
    };
    cssEachCounterItem(cssComputedValue(style, PrCssProperty_CounterReset),
        [&](PrStringView name, bool, bool, int32_t value) { instantiate(name, value); });
    cssEachCounterItem(cssComputedValue(style, PrCssProperty_CounterSet),
        [&](PrStringView name, bool, bool given, int32_t value) {
            uint32_t at = instanceOf(p, name);
            if (at == UINT32_MAX)
                at = instantiate(name, 0);
            p.scope.data[at].value = given ? value : 0;
        });
    bool listItem = cssIsListItem(style);
    bool named = false;
    cssEachCounterItem(cssComputedValue(style, PrCssProperty_CounterIncrement),
        [&](PrStringView name, bool, bool given, int32_t value) {
            named = named || name == "list-item";
            uint32_t at = instanceOf(p, name);
            if (at == UINT32_MAX)
                at = instantiate(name, 0);
            p.scope.data[at].value += given ? value : 1;
        });
    if (listItem && !named) {
        uint32_t at = instanceOf(p, "list-item");
        if (at == UINT32_MAX)
            at = instantiate("list-item", 0);
        p.scope.data[at].value += 1;
    }
}

// Takes the counters in scope as the document counters of the page `page`, unless it has them already: the counters of
// an element at the start of the page, inside the deepest element that spans the break before the page
// (css-page-3/#page-based-counters).
void start(Pass& p, uint32_t page) {
    if (p.started.data[page])
        return;
    p.started.data[page] = true;
    // A later page's counters may come before an earlier one's in tree order, as a float's may, so each page records
    // where its own counters are.
    if (p.readsCounters) {
        p.snapshotStarts.data[2 * page] = p.snapshots.count;
        p.snapshots.append(p.scope.data, p.scope.count);
        p.snapshotStarts.data[2 * page + 1] = p.snapshots.count;
    }
}

// Notes that the walk reached `node`. If it is the first content of a page, the assignments after it on its page do not
// start the page, and the page's document counters are those in scope, unless an element that starts on the page came
// before.
void reach(Pass& p, const PrDomNode* node) {
    uint32_t at = p.firstIndex.find(uint64_t(uintptr_t(node)), [&](uint32_t i) {
        return uint64_t(uintptr_t(p.firsts.data[i]));
    });
    if (at == kHashMissing || p.reached.data[at])
        return;
    start(p, at);
    p.reached.data[at] = true;
}

// Returns the page that holds the box `box`, or UINT32_MAX for none.
uint32_t pageOf(const Pass& p, PrLayoutNode box) {
    if (box == kPrLayoutNodeNone)
        return UINT32_MAX;
    uint32_t page = prLayoutNodePage(p.tree, box);
    return page < p.pages ? page : UINT32_MAX;
}

// Appends the text of `element` to the pass's text, as if white-space were normal: each run of white space as one
// space, and none at either end (css-gcpm-3/#content-function-header). With `letter`, appends only its first character.
void addElementText(Pass& p, const PrDomElement* element, bool letter) {
    PrRawString text = domNodeTextContent(static_cast<const PrDomNode*>(element));
    PrStringView view = prRawStringView(&text);
    bool space = false;
    bool any = false;
    for (size_t i = 0; i < view.size; ++i) {
        char c = view.data[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
            space = any;
            continue;
        }
        if (space)
            p.text.push(' ');
        space = false;
        any = true;
        p.text.push(c);
        // A character is its lead byte and the continuation bytes after it.
        if (letter && (i + 1 >= view.size || (uint8_t(view.data[i + 1]) & 0xc0) != 0x80))
            break;
    }
    prRawStringReset(&text);
}

// Records the named strings that the element styled by `style` sets
// (css-gcpm-3/#setting-named-strings-the-string-set-pro): for each name, its content list's strings, counters and
// content() as the element's text.
void setStrings(Pass& p, const PrDomElement* element, const CssComputedStyle* style, uint32_t page) {
    CssValueRun set = cssComputedValue(style, PrCssProperty_StringSet);
    if (set.count == 0 || cssValueIsKeyword(set.values, PrCssKeyword_None))
        return;
    bool fresh = true;
    Assignment* current = nullptr;
    for (uint32_t i = 0; i < set.count; i += set.values[i].next) {
        const CssValueNode* item = &set.values[i];
        if (cssValueIsLiteral(item, ',')) {
            fresh = true;
        } else if (fresh) {
            fresh = false;
            uint32_t start = addText(p, cssValueText(set, item));
            p.strings.push(Assignment{
                .nameStart = start,
                .nameEnd = p.text.count,
                .valueStart = p.text.count,
                .valueEnd = p.text.count,
                .element = element,
                .page = page,
                .start = !p.reached.data[page],
            });
            current = &p.strings.last();
        } else if (current != nullptr) {
            if (item->kind == PrCssValueKind_String) {
                addText(p, cssValueText(set, item));
            } else if (cssValueIsFunction(item, PrCssKeyword_Content)) {
                PrCssKeyword part = PrCssKeyword_Text;
                uint32_t starts[1];
                if (cssValueArguments(set, i, starts, 1) != 0 && set.values[starts[0]].kind == PrCssValueKind_Keyword)
                    part = cssValueKeyword(&set.values[starts[0]]);
                if (part == PrCssKeyword_Text || part == PrCssKeyword_FirstLetter)
                    addElementText(p, element, part == PrCssKeyword_FirstLetter);
            } else if (cssValueIsFunction(item, PrCssKeyword_Counter)) {
                uint32_t at = instanceOf(p, nameOf(set, i));
                PrTextBuffer number{};
                uint32_t starts[2];
                uint32_t args = cssValueArguments(set, i, starts, 2);
                CssValueRun counterStyle = args > 1 ? cssValueSubtree(set, starts[1]) : CssValueRun{};
                cssCounterAppend(&number, &p.r->rules.counterStyles, counterStyle,
                    at != UINT32_MAX ? p.scope.data[at].value : 0,
                    CssCounterFlow{CssCounterSide_Right, CssCounterSide_Down});
                addText(p, prTextBufferView(&number));
                prTextBufferReset(&number);
            }
            current->valueEnd = p.text.count;
        }
        if (item->next == 0)
            break;
    }
}

// Walks the subtree of `node` at `depth` in tree order, over the boxes that the presentation generates, and records
// what the margin boxes read of it.
void walk(Pass& p, const PrDomNode* node, uint32_t depth) {
    reach(p, node);
    const PrDomElement* element = domElementFromNode(node);
    if (element == nullptr)
        return;
    const CssComputedStyle* style = cssPresentationStyle(p.r, element);
    if (style == nullptr)
        return;
    PrLayoutNode box = cssPresentationBox(p.r, node);
    // The outermost element that starts on a page and ends on it is at the page's start, before its own counters,
    // inside the elements that span the break after the page.
    if (p.startIndex.index.count != 0) {
        uint32_t at = p.startIndex.find(uint64_t(uintptr_t(node)), [&](uint32_t i) {
            return uint64_t(uintptr_t(p.starts.data[i]));
        });
        if (at != kHashMissing)
            start(p, at);
    }
    if (box != kPrLayoutNodeNone)
        count(p, style, depth);
    if (p.readsStrings) {
        CssValueRun set = cssComputedValue(style, PrCssProperty_StringSet);
        uint32_t page =
            set.count != 0 && !cssValueIsKeyword(set.values, PrCssKeyword_None) ? pageOf(p, box) : UINT32_MAX;
        if (page != UINT32_MAX)
            setStrings(p, element, style, page);
    }
    CssValueRun position = cssComputedValue(style, PrCssProperty_Position);
    if (position.count != 0 && cssValueIsFunction(position.values, PrCssKeyword_Running)) {
        // A running element's content is out of flow, and is laid out only in the margin boxes that place it.
        uint32_t page = pageOf(p, box);
        if (page != UINT32_MAX && p.readsElements) {
            uint32_t start = addText(p, nameOf(position, 0));
            p.elements.push(Assignment{
                .nameStart = start,
                .nameEnd = p.text.count,
                .element = element,
                .page = page,
                .start = !p.reached.data[page],
            });
        }
        return;
    }
    domFlatEach(node, [&](PrDomNode* child) { walk(p, child, depth + 1); });
    while (p.scope.count != 0 && p.scope.last().owner == depth)
        p.scope.pop();
}

// Returns true if the name of `a` comes before `name` in the order of their bytes.
bool nameBefore(const Pass& p, const Assignment& a, PrStringView name) {
    PrStringView own = textOf(p, a.nameStart, a.nameEnd);
    size_t common = own.size < name.size ? own.size : name.size;
    int order = prMemCompare(own.data, name.data, common);
    return order < 0 || (order == 0 && own.size < name.size);
}

// Sorts `list` by name, then by page, then in tree order, so that a page's assignments of one name are found by
// searching. An assignment's page is the page of its box, and boxes may come out of tree order across pages, as floats
// do.
void sortAssignments(const Pass& p, RawArray<Assignment>& list) {
    for (uint32_t i = 0; i < list.count; ++i)
        list.data[i].order = i;
    sortItems(list.span(), [&](const Assignment& a, const Assignment& b) {
        PrStringView name = textOf(p, b.nameStart, b.nameEnd);
        if (nameBefore(p, a, name))
            return true;
        if (textOf(p, a.nameStart, a.nameEnd) != name)
            return false;
        return a.page < b.page || (a.page == b.page && a.order < b.order);
    });
}

// Walks the document in tree order for what the margin boxes read of it, once the pages' first content is known.
void walkDocument(Pass& p) {
    p.reached.resize(p.pages);
    p.started.resize(p.pages);
    for (uint32_t i = 0; i < p.pages; ++i) {
        p.reached.data[i] = false;
        p.started.data[i] = false;
    }
    p.firsts.resize(p.pages);
    for (uint32_t i = 0; i < p.pages; ++i) {
        PrLayoutNode first = prLayoutTreePage(p.tree, i).first;
        p.firsts.data[i] = first != kPrLayoutNodeNone ? documentNodeOf(p, first) : nullptr;
        if (p.firsts.data[i] != nullptr)
            p.firstIndex.insert(uint64_t(uintptr_t(p.firsts.data[i])), i);
    }
    // An element that holds a later page's first content spans the breaks before that page.
    if (p.readsCounters) {
        KeyIndex spans;
        RawArray<const PrDomNode*> spanning;
        RawArray<uint32_t> spannedTo;
        for (uint32_t i = p.pages; i-- > 1;) {
            for (const PrDomNode* at = p.firsts.data[i] != nullptr ? p.firsts.data[i]->parent : nullptr;
                    at != nullptr; at = at->parent) {
                uint64_t key = uint64_t(uintptr_t(at));
                if (spans.find(key, [&](uint32_t k) { return uint64_t(uintptr_t(spanning.data[k])); }) != kHashMissing)
                    break;
                spans.insert(key, spanning.count);
                spanning.push(at);
                spannedTo.push(i);
            }
        }
        auto spansAfter = [&](const PrDomNode* node, uint32_t page) {
            uint32_t at = spans.find(uint64_t(uintptr_t(node)),
                [&](uint32_t k) { return uint64_t(uintptr_t(spanning.data[k])); });
            return at != kHashMissing && spannedTo.data[at] > page;
        };
        p.starts.resize(p.pages);
        for (uint32_t i = 0; i < p.pages; ++i) {
            const PrDomNode* at = p.firsts.data[i];
            if (at != nullptr && domElementFromNode(at) == nullptr)
                at = at->parent;
            const PrDomNode* outermost = nullptr;
            for (; at != nullptr && domElementFromNode(at) != nullptr; at = at->parent) {
                if (spansAfter(at, i) || pageOf(p, cssPresentationBox(p.r, at)) != i)
                    break;
                outermost = at;
            }
            p.starts.data[i] = outermost;
            if (outermost != nullptr)
                p.startIndex.insert(uint64_t(uintptr_t(outermost)), i);
        }
        spans.reset();
        spanning.reset();
        spannedTo.reset();
    }
    p.snapshotStarts.resize(2 * p.pages);
    for (uint32_t i = 0; i < 2 * p.pages; ++i)
        p.snapshotStarts.data[i] = 0;
    PrDomElement* root = prDomDocumentElement(p.r->document);
    walk(p, static_cast<const PrDomNode*>(root), 1);
    p.scope.clear();
    sortAssignments(p, p.strings);
    sortAssignments(p, p.elements);
}

// Returns the assignment of `name` among `list` that the keyword `which` takes on `page`
// (css-gcpm-3/#using-named-strings): the first on the page, the first if it starts the page, the last on the page, or
// the first unless the page has one. Without one on the page, it is the last before the page, the entry value. Returns
// null for none, and for an empty value.
const Assignment* PR_NULLABLE assignmentOf(const Pass& p, const RawArray<Assignment>& list, PrStringView name,
        uint32_t page, PrCssKeyword which) {
    const Assignment* items = list.data;
    uint32_t begin = partitionPoint(0, list.count, [&](uint32_t i) { return nameBefore(p, items[i], name); });
    uint32_t end = partitionPoint(begin, list.count,
        [&](uint32_t i) { return textOf(p, items[i].nameStart, items[i].nameEnd) == name; });
    uint32_t low = partitionPoint(begin, end, [&](uint32_t i) { return items[i].page < page; });
    uint32_t high = partitionPoint(low, end, [&](uint32_t i) { return items[i].page <= page; });
    const Assignment* entry = low > begin ? &items[low - 1] : nullptr;
    const Assignment* first = low < high ? &items[low] : nullptr;
    const Assignment* last = low < high ? &items[high - 1] : nullptr;
    switch (which) {
        case PrCssKeyword_Start: return first != nullptr && first->start ? first : entry;
        case PrCssKeyword_Last: return last != nullptr ? last : entry;
        case PrCssKeyword_FirstExcept: return first != nullptr ? nullptr : entry;
        default: return first != nullptr ? first : entry;
    }
}

// Returns the value of the counter `name` on the page `page` for the margin box `which`, and appends each of its
// instances, outermost first, to `out`: the page and pages counters, a counter of the margin box's context or of the
// page context, or the document's at the start of the page.
void counterValues(const Pass& p, PrStringView name, uint32_t page, PrLayoutMarginBox which, RawArray<int32_t>& out) {
    for (const PageCounter& c : p.boxCounters[which].span()) {
        if (c.name == name) {
            out.push(c.value);
            return;
        }
    }
    for (const PageCounter& c : p.pageCounters.span()) {
        if (c.name == name) {
            out.push(c.value);
            return;
        }
    }
    if (name == "page") {
        out.push(p.page);
        return;
    }
    if (name == "pages") {
        out.push(int32_t(p.pages));
        return;
    }
    if (p.readsCounters && page < p.pages) {
        uint32_t start = p.snapshotStarts.data[2 * page];
        uint32_t end = p.snapshotStarts.data[2 * page + 1];
        for (uint32_t i = start; i < end; ++i) {
            if (textOf(p, p.snapshots.data[i].nameStart, p.snapshots.data[i].nameEnd) == name)
                out.push(p.snapshots.data[i].value);
        }
    }
    if (out.count == 0)
        out.push(0);
}

// Applies the counter properties of a page context or a margin context, styled by `style`, to `counters` for a page
// (css-page-3/#page-based-counters). Returns true if its counter-increment names the page counter, which then is not
// incremented by one.
bool countPage(Pass& p, RawArray<PageCounter>& counters, const CssComputedStyle* style) {
    auto counterOf = [&](PrStringView name) -> int32_t& {
        if (name == "page")
            return p.page;
        for (PageCounter& c : counters.span()) {
            if (c.name == name)
                return c.value;
        }
        counters.push(PageCounter{name, 0});
        return counters.last().value;
    };
    bool page = false;
    cssEachCounterItem(cssComputedValue(style, PrCssProperty_CounterReset),
        [&](PrStringView name, bool, bool, int32_t value) { counterOf(name) = value; });
    cssEachCounterItem(cssComputedValue(style, PrCssProperty_CounterSet),
        [&](PrStringView name, bool, bool given, int32_t value) { counterOf(name) = given ? value : 0; });
    cssEachCounterItem(cssComputedValue(style, PrCssProperty_CounterIncrement),
        [&](PrStringView name, bool, bool given, int32_t value) {
            page = page || name == "page";
            counterOf(name) += given ? value : 1;
        });
    return page;
}

// What a margin box holds on one page, in order: runs of text, images and copies of running elements.
enum {
    PieceKind_Text,
    PieceKind_Image,
    PieceKind_Element,
};
typedef uint8_t PieceKind;

struct Piece {
    PieceKind kind;
    uint32_t start;                 // Text: the range of the text
    uint32_t end;
    uint32_t item;                           // Image: the content item
    const PrDomElement* PR_NULLABLE element; // Element: the running element
};

// Appends to `pieces` what the content of the margin box `which`, styled by `style`, gives on `page`, with its text in
// `text`, and to `made` what it is made from.
void piecesOf(Pass& p, uint32_t page, PrLayoutMarginBox which, const CssComputedStyle* style, RawArray<char>& text,
        RawArray<Piece>& pieces, RawArray<char>& made) {
    CssValueRun content = cssComputedValue(style, PrCssProperty_Content);
    uint32_t run = text.count;
    auto endRun = [&]() {
        if (text.count != run)
            pieces.push(Piece{.kind = PieceKind_Text, .start = run, .end = text.count});
        run = text.count;
    };
    auto add = [&](PrStringView value) { text.append(value.data, uint32_t(value.size)); };
    RawArray<int32_t> values;
    PrTextBuffer number{};
    CssCounterFlow flow{CssCounterSide_Right, CssCounterSide_Down};
    eachItem(content, [&](uint32_t i, const CssValueNode* item) {
        if (item->kind == PrCssValueKind_String) {
            add(cssValueText(content, item));
        } else if (cssValueIsFunction(item, PrCssKeyword_Counter) || cssValueIsFunction(item, PrCssKeyword_Counters)) {
            bool several = cssValueIsFunction(item, PrCssKeyword_Counters);
            uint32_t starts[3];
            uint32_t args = cssValueArguments(content, i, starts, 3);
            PrStringView name = nameOf(content, i);
            PrStringView separator{};
            if (several && args > 1)
                separator = cssValueText(content, &content.values[starts[1]]);
            uint32_t styleAt = several ? 2 : 1;
            CssValueRun counterStyle = args > styleAt ? cssValueSubtree(content, starts[styleAt]) : CssValueRun{};
            values.clear();
            counterValues(p, name, page, which, values);
            if (!several)
                values.data[0] = values.last();
            uint32_t shown = several ? values.count : 1;
            for (uint32_t v = 0; v < shown; ++v) {
                if (v != 0)
                    add(separator);
                prTextBufferClear(&number);
                cssCounterAppend(&number, &p.r->rules.counterStyles, counterStyle, values.data[v], flow);
                add(prTextBufferView(&number));
            }
        } else if (cssValueIsFunction(item, PrCssKeyword_String)) {
            const Assignment* a =
                assignmentOf(p, p.strings, nameOf(content, i), page, whichOf(content, i, PrCssKeyword_First));
            if (a != nullptr)
                add(textOf(p, a->valueStart, a->valueEnd));
        } else if (cssValueIsFunction(item, PrCssKeyword_Element)) {
            const Assignment* a =
                assignmentOf(p, p.elements, nameOf(content, i), page, whichOf(content, i, PrCssKeyword_First));
            if (a != nullptr) {
                endRun();
                pieces.push(Piece{.kind = PieceKind_Element, .element = a->element});
            }
        } else if (item->kind == PrCssValueKind_Url || cssValueIsFunction(item, PrCssKeyword_Url)) {
            endRun();
            pieces.push(Piece{.kind = PieceKind_Image, .item = i});
        }
    });
    endRun();
    values.reset();
    prTextBufferReset(&number);
    // What the pieces are made from: their text, the images and whether each has loaded, which a pass after one loads
    // makes again, and the running elements, which a pass after any change to the presentation copies again.
    for (const Piece& piece : pieces.span()) {
        char kind = char('t' + piece.kind);
        made.push(kind);
        if (piece.kind == PieceKind_Text) {
            made.append(text.data + piece.start, piece.end - piece.start);
        } else {
            CssValueRun image = cssValueSubtree(content, piece.item);
            uint64_t parts[2] = {
                piece.kind == PieceKind_Element ? uint64_t(uintptr_t(piece.element)) : piece.item,
                piece.kind == PieceKind_Element ? p.r->version : uint64_t(cssPresentationImageReady(p.r, image)),
            };
            made.append(reinterpret_cast<const char*>(parts), sizeof(parts));
        }
        made.push('\0');
    }
}

// Returns the nearest hyperlink around `node`, an a element with an href, up to the running element `top`, or null.
const PrDomElement* PR_NULLABLE linkAround(const PrDomNode* node, const PrDomElement* top) {
    for (const PrDomNode* at = node; at != nullptr; at = at->parent) {
        const PrDomElement* element = domElementFromNode(at);
        if (element != nullptr && prDomElementNamespace(element) == PrDomNamespace_Html &&
                prDomElementTag(element) == PrDomTag_A && domAttributeValue(element, "href").data != nullptr)
            return element;
        if (at == static_cast<const PrDomNode*>(top))
            break;
    }
    return nullptr;
}

// Gives the boxes of the copy `copy` of the box `original` of a running element `top`, and of their descendants, that
// show a link's content keys of their own, so that the document's structure makes links of their marks
// (CssPageMarginLink). Every other box of a copy has no key, and its content is an artifact.
void keyLinks(Pass& p, PrLayoutNode original, PrLayoutNode copy, const PrDomElement* top, uint32_t number) {
    CssPageMargins& margins = p.r->pageMargins;
    PrLayoutNodeKind kind = prLayoutNodeKind(p.tree, original);
    if (kind == PrLayoutNodeKind_Text || kind == PrLayoutNodeKind_Replaced) {
        const PrDomNode* node = cssPresentationNodeFromLayout(p.r, original);
        const PrDomElement* link = node != nullptr ? linkAround(node, top) : nullptr;
        if (link != nullptr) {
            uint64_t key = (++margins.keys << 1) | 1;
            prLayoutNodeSetKey(p.tree, copy, key);
            margins.links.push(CssPageMarginLink{link, number, key});
        }
    }
    PrLayoutNode child = prLayoutNodeFirstChild(p.tree, original);
    PrLayoutNode copied = prLayoutNodeFirstChild(p.tree, copy);
    for (; child != kPrLayoutNodeNone && copied != kPrLayoutNodeNone; child = prLayoutNodeNextSibling(p.tree, child),
            copied = prLayoutNodeNextSibling(p.tree, copied))
        keyLinks(p, child, copied, top, number);
}

// Makes the margin box `which` of a page under the root's box, styled by `style` and `record`, holding `pieces`.
PrLayoutNode makeBox(Pass& p, const CssComputedStyle* style, PrLayoutStyleIndex record, const RawArray<char>& text,
        const RawArray<Piece>& pieces) {
    PrCssPresentation* r = p.r;
    PrLayoutNode box = prLayoutNodeCreateElement(p.tree, r->root, record);
    CssValueRun content = cssComputedValue(style, PrCssProperty_Content);
    for (const Piece& piece : pieces.span()) {
        switch (piece.kind) {
            case PieceKind_Text:
                prLayoutNodeCreateText(p.tree, box, PrStringView{text.data + piece.start, piece.end - piece.start});
                break;
            case PieceKind_Image: cssPresentationCreateImage(r, box, style, cssValueSubtree(content, piece.item)); break;
            case PieceKind_Element: {
                // A copy of the running element, which inherits from where the element is in the document
                // (css-gcpm-3/#running-elements), laid out in flow in the margin box.
                PrLayoutNode original = cssPresentationBox(r, static_cast<const PrDomNode*>(piece.element));
                if (original == kPrLayoutNodeNone)
                    break;
                PrLayoutNode copy = prLayoutTreeCopy(p.tree, original, box);
                PrLayoutStyle placed = prLayoutStyleRecord(p.tree, prLayoutNodeStyle(p.tree, original));
                PrLayoutBoxStyle placedBox = prLayoutBoxStyleRecord(p.tree, placed.groups[PrLayoutGroup_Box]);
                placedBox.position = PrLayoutPosition_Static;
                placed.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(p.tree, &placedBox);
                prLayoutNodeSetStyle(p.tree, copy, prLayoutStyleIntern(p.tree, &placed));
                keyLinks(p, original, copy, piece.element, ++r->pageMargins.copies);
                break;
            }
        }
    }
    return box;
}

// Erases the box of `slot`, and empties it.
void eraseSlot(PrCssPresentation* r, CssPageMarginSlot& slot) {
    if (slot.node != kPrLayoutNodeNone && prLayoutNodeIsValid(r->tree, slot.node))
        prLayoutTreeErase(r->tree, slot.node);
    slot.node = kPrLayoutNodeNone;
    slot.record = kPrLayoutStyleNone;
    prRawStringReset(&slot.made);
}

// Releases the styles of the pass.
void endPass(Pass& p) {
    for (KindStyles& kind : p.kinds.span()) {
        cssComputedStyleRelease(kind.context);
        for (const CssComputedStyle* style : kind.boxes)
            cssComputedStyleRelease(style);
    }
    p.kinds.reset();
    p.text.reset();
    p.strings.reset();
    p.elements.reset();
    p.snapshotStarts.reset();
    p.snapshots.reset();
    p.scope.reset();
    p.firsts.reset();
    p.firstIndex.reset();
    p.starts.reset();
    p.startIndex.reset();
    p.reached.reset();
    p.started.reset();
    p.pageCounters.reset();
    for (RawArray<PageCounter>& counters : p.boxCounters)
        counters.reset();
}

// Gives the tree the margin boxes of the presentation's slots for `pages` pages, in order, and has the next update lay
// them out.
void commit(PrCssPresentation* r, uint32_t pages) {
    CssPageMargins& margins = r->pageMargins;
    RawArray<PrLayoutNode> boxes;
    boxes.resize(pages * PrLayoutMarginBox_Count);
    margins.nodes.clear();
    for (uint32_t i = 0; i < boxes.count; ++i) {
        boxes.data[i] = i < margins.slots.count ? margins.slots.data[i].node : PrLayoutNode(kPrLayoutNodeNone);
        if (boxes.data[i] != kPrLayoutNodeNone)
            margins.nodes.push(boxes.data[i]);
    }
    cssPresentationReconcileRoot(r);
    prLayoutTreeSetPageMargins(r->tree, boxes.data, pages);
    boxes.reset();
    r->layoutStale = true;
}

} // namespace

void cssPresentationSetPageMargins(PrCssPresentation* presentation, const CssPageBoxes& boxes, bool boxesChanged) {
    PrCssPresentation* r = presentation;
    if (r->root == kPrLayoutNodeNone || prDomDocumentElement(r->document) == nullptr) {
        cssPresentationClearPageMargins(r);
        return;
    }
    uint32_t pages = prLayoutTreePageCount(r->tree);
    CssPageMargins& last = r->pageMargins;
    if (!boxesChanged && last.made && last.version == r->version && last.pages == pages)
        return;
    last.made = true;
    last.version = r->version;
    last.pages = pages;
    Pass p{.r = r, .tree = r->tree, .pages = pages};
    if (!findStyles(p, boxes)) {
        endPass(p);
        cssPresentationClearPageMargins(r);
        return;
    }
    CssPageMargins& margins = r->pageMargins;
    margins.links.clear();
    if (p.readsStrings || p.readsElements || p.readsCounters)
        walkDocument(p);
    // The slots of the pages past the last are erased, from the last.
    for (uint32_t i = margins.slots.count; i-- > p.pages * PrLayoutMarginBox_Count;)
        eraseSlot(r, margins.slots.data[i]);
    uint32_t total = p.pages * PrLayoutMarginBox_Count;
    if (margins.slots.count > total) {
        margins.slots.count = total;
    } else {
        while (margins.slots.count < total)
            margins.slots.push(CssPageMarginSlot{});
    }
    RawArray<char> text;
    RawArray<Piece> pieces;
    RawArray<char> made;
    p.page = 0;
    for (uint32_t page = 0; page < p.pages; ++page) {
        uint32_t kind = cssPageBoxesKind(boxes, prLayoutTreePage(p.tree, page).context);
        const KindStyles* styles = kind < p.kinds.count ? &p.kinds.data[kind] : nullptr;
        // The page counter is incremented by one on every page, unless the page context's counter-increment names it.
        bool named = styles != nullptr && styles->context != nullptr && countPage(p, p.pageCounters, styles->context);
        if (!named)
            p.page += 1;
        for (PrLayoutMarginBox which = 0; which < PrLayoutMarginBox_Count; ++which) {
            CssPageMarginSlot& slot = margins.slots.data[page * PrLayoutMarginBox_Count + which];
            const CssComputedStyle* style = styles != nullptr ? styles->boxes[which] : nullptr;
            if (style == nullptr) {
                eraseSlot(r, slot);
                continue;
            }
            int32_t pageBefore = p.page;
            countPage(p, p.boxCounters[which], style);
            p.page = pageBefore;
            text.clear();
            pieces.clear();
            made.clear();
            piecesOf(p, page, which, style, text, pieces, made);
            PrStringView kept = prRawStringView(&slot.made);
            bool copies = false;
            for (const Piece& piece : pieces.span())
                copies = copies || piece.kind == PieceKind_Element;
            if (slot.node != kPrLayoutNodeNone && prLayoutNodeIsValid(r->tree, slot.node) && !copies &&
                    slot.record == styles->records[which] && kept == PrStringView{made.data, made.count})
                continue;
            eraseSlot(r, slot);
            slot.node = makeBox(p, style, styles->records[which], text, pieces);
            slot.record = styles->records[which];
            slot.made = prRawStringCopy(PrStringView{made.data, made.count});
        }
    }
    text.reset();
    pieces.reset();
    made.reset();
    commit(r, p.pages);
    endPass(p);
}

void cssPresentationClearPageMargins(PrCssPresentation* presentation) {
    PrCssPresentation* r = presentation;
    CssPageMargins& margins = r->pageMargins;
    if (margins.slots.count == 0)
        return;
    // The boxes are the last of the root's children, and the last of its stacking context's list, so erasing them from
    // the last moves nothing that stays.
    for (uint32_t i = margins.slots.count; i-- > 0;)
        eraseSlot(r, margins.slots.data[i]);
    margins.slots.clear();
    margins.links.clear();
    margins.made = false;
    commit(r, 0);
}

void cssPageMarginsReset(CssPageMargins& margins) {
    for (CssPageMarginSlot& slot : margins.slots.span())
        prRawStringReset(&slot.made);
    margins.slots.reset();
    margins.nodes.reset();
    margins.links.reset();
    for (CssDeclarationBlock*& block : margins.defaults) {
        if (block == nullptr)
            continue;
        cssDeclarationBlockReset(block);
        prMemoryFree(block);
        block = nullptr;
    }
}

CssPageMarginLinks cssPresentationPageMarginLinks(const PrCssPresentation* presentation) {
    const RawArray<CssPageMarginLink>& links = presentation->pageMargins.links;
    return CssPageMarginLinks{links.data, links.count};
}

} // namespace pr_impl
