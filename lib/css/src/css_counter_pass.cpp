// The counter pass: the values of CSS counters over the boxes that a presentation generates, and the text that they
// give the content of pseudo-elements.

#include "css/src/css_counter_pass.h"

#include "pr/locale.h"

#include "css/src/css_counter.h"
#include "css/src/css_keyword.h"
#include "dom/src/dom_flat.h"
#include "dom/src/dom_node.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

namespace {

typedef CssCounting Counter;
typedef CssCountingStep Step;
typedef CssCountingBoundary Boundary;

// The state of one run of the pass, which works in the presentation's scratch arrays (CssCountingScratch).
struct Pass {
    PrCssPresentation* presentation;
    RawArray<Step>& steps;
    RawArray<Counter>& counters;
    CssCountingTexts& texts;
    PrTextBuffer& text;
    const CssStyleValues* initial;  // the initial Generated group, whose counter properties change no counter
    RawArray<Boundary>& boundaries; // innermost last
    uint32_t found;                 // the listed changes (counterChanges) among the elements collected
    uint32_t quoteDepth;            // the quote depth (css-content-3/#quote-depth)
};

// Returns the sum of `a` and `b`, clamped to the range of a counter's value.
int32_t addClamped(int32_t a, int64_t b) {
    int64_t sum = int64_t(a) + b;
    return sum < INT32_MIN ? INT32_MIN : sum > INT32_MAX ? INT32_MAX : int32_t(sum);
}

// Returns `value` incremented by `by`, or `value` itself if the sum leaves the range of a counter's value. The engine
// drops such an increment, as css/css-lists/counter-reset-increment-overflow-underflow expects, where
// css-lists-3/#auto-numbering clamps the sum.
int32_t increment(int32_t value, int64_t by) {
    int64_t sum = int64_t(value) + by;
    return sum < INT32_MIN || sum > INT32_MAX ? value : int32_t(sum);
}

// Returns true if the node of `step` is a list item, an element or a ::before or ::after, whose counter-increment does
// not name list-item, so that the item increments list-item implicitly (css-lists-3/#list-item-counter).
bool incrementsListItem(const Step& step) {
    if (!step.listItem)
        return false;
    const CssComputedStyle* style = step.style;
    bool named = false;
    cssEachCounterItem(cssComputedValue(style, PrCssProperty_CounterIncrement),
        [&](PrStringView name, bool, bool, int32_t) { named = named || name == "list-item"; });
    return !named;
}

// Returns true if a counter-reset of `name` instantiates a counter whose scope is the instantiating node and its
// descendants, without its following siblings. The list-item counter has that narrow scope, so that a list nested
// directly in a list leaves the outer list's numbering to the items after it, which csswg-drafts#9076 resolved to
// support (the wide scope of css-lists-3/#nested-counters would number them in the nested list).
bool narrowScope(PrStringView name) {
    return name == "list-item";
}

// Returns the index of the innermost counter named `name`, or kArrayMissing.
uint32_t innermost(const Pass& p, PrStringView name) {
    for (uint32_t i = p.counters.count; i > 0; --i) {
        if (p.counters.data[i - 1].name == name)
            return i - 1;
    }
    return kArrayMissing;
}

// Returns the initial value of a reversed counter that the node of step `at` instantiates without a value
// (css-lists-3/#instantiating-counters). The value is the negated sum of the increments in the counter's scope, up to
// the first node that sets the counter, whose value is added, plus the last increment that is not zero, negated.
//
// The scope ends where a later sibling instantiates a counter of the same name. A descendant that does so starts a
// nested scope, whose nodes change the nested counter, and so does an element with style containment.
int32_t reversedStart(const Pass& p, uint32_t at, PrStringView name) {
    uint32_t depth = p.steps.data[at].depth;
    // A narrow scope ends where the instantiating node does, and a pseudo-element has no subtree.
    bool narrow = narrowScope(name);
    bool alone = narrow && p.steps.data[at].node->type == DomTargetType_PseudoElement;
    uint32_t end = narrow ? depth + 1 : depth;
    int64_t sum = 0;
    int64_t last = 0;
    uint32_t nested = UINT32_MAX;    // the least depth of the steps of a nested scope, or UINT32_MAX outside one
    uint32_t contained = UINT32_MAX; // the depth of the element with style containment whose subtree the walk is in
    for (uint32_t i = at; i < p.steps.count && !(alone && i != at); ++i) {
        const Step& step = p.steps.data[i];
        if (step.node == nullptr) {
            if (step.depth < end)
                break;
            if (step.depth <= contained)
                contained = UINT32_MAX;
            if (step.depth < nested || (narrow && step.depth == nested))
                nested = UINT32_MAX;
            continue;
        }
        if (step.depth > contained || step.depth >= nested)
            continue;
        const CssComputedStyle* style = step.style;
        bool resets = false;
        cssEachCounterItem(cssComputedValue(style, PrCssProperty_CounterReset),
            [&](PrStringView item, bool, bool, int32_t) { resets = resets || item == name; });
        if (resets && i != at) {
            if (step.depth == depth)
                break;
            nested = step.depth;
            continue;
        }
        int64_t increment = 0;
        cssEachCounterItem(cssComputedValue(style, PrCssProperty_CounterIncrement),
            [&](PrStringView item, bool, bool given, int32_t value) {
                if (item == name)
                    increment += given ? value : 1;
            });
        if (name == "list-item" && incrementsListItem(step))
            increment -= 1;
        if (increment != 0)
            last = -increment;
        // The increments inside a subtree with style containment reach no counter from outside it.
        if (step.node->type != DomTargetType_PseudoElement && cssContainsStyle(style))
            contained = step.depth;
        bool sets = false;
        int32_t set = 0;
        cssEachCounterItem(cssComputedValue(style, PrCssProperty_CounterSet), [&](PrStringView item, bool, bool, int32_t value) {
            if (item == name) {
                sets = true;
                set = value;
            }
        });
        if (sets) {
            sum += set;
            break;
        }
        sum -= increment;
    }
    return addClamped(0, sum + last);
}

// Instantiates a counter on the node of step `at` (css-lists-3/#instantiate-counter). It replaces a counter of the same
// name that the node or a previous sibling instantiated.
void instantiate(Pass& p, uint32_t at, PrStringView name, bool reversed, bool given, int32_t value, bool narrow) {
    uint32_t depth = p.steps.data[at].depth;
    uint32_t inner = innermost(p, name);
    if (inner != kArrayMissing && p.counters.data[inner].depth == depth)
        p.counters.erase(inner);
    if (reversed && !given)
        value = reversedStart(p, at, name);
    p.counters.push(Counter{.name = name, .reversed = reversed, .value = value, .depth = depth, .narrow = narrow});
}

// Returns the index of the innermost counter named `name`, instantiating one with the value 0 on the node of step `at`
// if there is none.
uint32_t counterFor(Pass& p, uint32_t at, PrStringView name) {
    uint32_t inner = innermost(p, name);
    if (inner != kArrayMissing)
        return inner;
    instantiate(p, at, name, false, true, 0, false);
    return p.counters.count - 1;
}

// Returns the index of the counter named `name` that the node of step `at` increments or sets: the innermost, unless a
// subtree with style containment holds the node and not the counter, in which case the node instantiates a new one.
uint32_t changedCounter(Pass& p, uint32_t at, PrStringView name) {
    uint32_t inner = innermost(p, name);
    if (inner != kArrayMissing && (p.boundaries.count == 0 || inner >= p.boundaries.last().counters))
        return inner;
    instantiate(p, at, name, false, true, 0, false);
    return p.counters.count - 1;
}

// Applies the counter properties of the node of step `at`: it instantiates, then increments, then sets
// (css-lists-3/#auto-numbering).
void applyCounters(Pass& p, uint32_t at) {
    const Step& step = p.steps.data[at];
    const CssComputedStyle* style = step.style;
    // Most participants are list items, whose counter properties are the initial ones.
    if (style->groups[CssStyleGroup_Generated] == p.initial) {
        if (incrementsListItem(step)) {
            uint32_t counter = changedCounter(p, at, "list-item");
            int32_t by = p.counters.data[counter].reversed ? -1 : 1;
            p.counters.data[counter].value = increment(p.counters.data[counter].value, by);
        }
        return;
    }
    CssValueRun reset = cssComputedValue(style, PrCssProperty_CounterReset);
    // Of several items that name the same counter, only the last instantiates it.
    uint32_t index = 0;
    cssEachCounterItem(reset, [&](PrStringView name, bool reversed, bool given, int32_t value) {
        uint32_t later = 0;
        bool last = true;
        cssEachCounterItem(reset, [&](PrStringView other, bool, bool, int32_t) {
            last = last && (later++ <= index || other != name);
        });
        ++index;
        if (last)
            instantiate(p, at, name, reversed, given, value, narrowScope(name));
    });
    cssEachCounterItem(cssComputedValue(style, PrCssProperty_CounterIncrement),
        [&](PrStringView name, bool, bool given, int32_t value) {
            uint32_t counter = changedCounter(p, at, name);
            p.counters.data[counter].value = increment(p.counters.data[counter].value, given ? value : 1);
        });
    if (incrementsListItem(step)) {
        uint32_t counter = changedCounter(p, at, "list-item");
        int32_t by = p.counters.data[counter].reversed ? -1 : 1;
        p.counters.data[counter].value = increment(p.counters.data[counter].value, by);
    }
    cssEachCounterItem(cssComputedValue(style, PrCssProperty_CounterSet), [&](PrStringView name, bool, bool, int32_t value) {
        uint32_t counter = changedCounter(p, at, name);
        p.counters.data[counter].value = value;
    });
}

// Returns the counter style that argument `at` of a counter() or counters() names, or an empty run, which is decimal,
// if the function has no such argument.
CssValueRun styleArgument(CssValueRun run, const uint32_t* starts, uint32_t count, uint32_t at) {
    return at < count ? cssValueSubtree(run, starts[at]) : CssValueRun{};
}

// Returns the physical sides of the inline end and the block end of a node styled by `style`, by its writing mode and
// its direction (css-writing-modes-4/#block-flow). sideways-lr runs its lines from the bottom up.
CssCounterFlow flowOf(const CssComputedStyle* style) {
    bool rtl = cssValueIsKeyword(cssComputedValue(style, PrCssProperty_Direction).values, PrCssKeyword_Rtl);
    CssCounterSide down = rtl ? CssCounterSide_Up : CssCounterSide_Down;
    CssCounterSide up = rtl ? CssCounterSide_Down : CssCounterSide_Up;
    CssCounterSide right = rtl ? CssCounterSide_Left : CssCounterSide_Right;
    switch (cssValueKeyword(cssComputedValue(style, PrCssProperty_WritingMode).values)) {
        case PrCssKeyword_VerticalRl:
        case PrCssKeyword_SidewaysRl: return CssCounterFlow{down, CssCounterSide_Left};
        case PrCssKeyword_VerticalLr: return CssCounterFlow{down, CssCounterSide_Right};
        case PrCssKeyword_SidewaysLr: return CssCounterFlow{up, CssCounterSide_Right};
        default: return CssCounterFlow{right, CssCounterSide_Down};
    }
}

// The quote keyword of a content item, or PrCssKeyword_None if it is not one.
PrCssKeyword quoteOf(const CssValueNode* item) {
    if (item->kind != PrCssValueKind_Keyword)
        return PrCssKeyword_None;
    PrCssKeyword keyword = cssValueKeyword(item);
    switch (keyword) {
        case PrCssKeyword_OpenQuote:
        case PrCssKeyword_CloseQuote:
        case PrCssKeyword_NoOpenQuote:
        case PrCssKeyword_NoCloseQuote:
            return keyword;
        default:
            return PrCssKeyword_None;
    }
}

// Returns the node whose style a node inherits from in the presentation, and writes that style to `style`: the
// originating element or ::before or ::after of a pseudo-element, or an element's parent element in the flat tree.
// Returns null for the root.
const PrDomNode* PR_NULLABLE parentOf(const PrCssPresentation* r, const PrDomNode* node,
        const CssComputedStyle* PR_NULLABLE* style) {
    for (const PrDomNode* at = domFlatParent(node); at != nullptr; at = domFlatParent(at)) {
        uint32_t entry = cssPresentationNodeEntry(r, at);
        if (entry != kHashMissing && r->entries.data[entry].style != nullptr) {
            *style = r->entries.data[entry].style;
            return at;
        }
    }
    *style = nullptr;
    return nullptr;
}

// Returns the opening or closing quotation mark at `depth` of the quotation mark system of the pseudo-element `node`,
// styled by `style` (css-content-3/#quotes-property).
//
// A list of pairs gives the pair at the depth, or its last pair past them, and none gives no mark. match-parent takes
// the value of the nearest ancestor that does not inherit it, and auto there means the marks of that ancestor's
// language. Otherwise auto gives the marks of the language of the originating element's parent, so that a q in another
// language than its parent's takes the marks of the text around it, as css/css-content/quotes-030 expects. CLDR gives
// the marks (prLocaleQuote).
PrStringView quoteMark(const Pass& p, const PrDomNode* node, const CssComputedStyle* style, uint32_t depth, bool open) {
    CssValueRun quotes = cssComputedValue(style, PrCssProperty_Quotes);
    const PrDomNode* holder = node;
    const CssComputedStyle* source = style;     // the style whose value is not match-parent, if one was found
    bool matched = false;
    while (cssValueIsKeyword(quotes.values, PrCssKeyword_MatchParent)) {
        const CssComputedStyle* parent = nullptr;
        holder = parentOf(p.presentation, holder, &parent);
        if (holder == nullptr)
            break;
        matched = true;
        source = parent;
        quotes = cssComputedValue(parent, PrCssProperty_Quotes);
    }
    if (cssValueIsKeyword(quotes.values, PrCssKeyword_None))
        return "";
    if (quotes.count != 0 && quotes.values[0].kind == PrCssValueKind_String) {
        uint32_t pairs = quotes.count / 2;
        uint32_t pair = depth < pairs ? depth : pairs - 1;
        return cssValueText(quotes, &quotes.values[2 * pair + (open ? 0 : 1)]);
    }
    PrLocale locale = source->locale;
    if (!matched) {
        const PrDomNode* element = node;
        const CssComputedStyle* elementStyle = style;
        while (element->type == DomTargetType_PseudoElement && element->parent != nullptr) {
            const CssComputedStyle* parent = nullptr;
            const PrDomNode* above = parentOf(p.presentation, element, &parent);
            if (above == nullptr)
                break;
            element = above;
            elementStyle = parent;
        }
        const CssComputedStyle* parent = nullptr;
        locale = parentOf(p.presentation, element, &parent) != nullptr ? parent->locale : elementStyle->locale;
    }
    return prLocaleQuote(locale, depth, open);
}

// Sets the texts of the pseudo-element of step `at` from the counters in its scope and the quote depth: its marker
// string if `marker` is set, or the text of each counter(), counters(), open-quote and close-quote of its content. A
// counter that no node instantiated is instantiated on the pseudo-element. Each quote keyword steps the quote depth,
// which a closing quote at depth 0 leaves at 0, inserting no mark.
void readCounters(Pass& p, uint32_t at, bool marker) {
    const Step& step = p.steps.data[at];
    const CssComputedStyle* style = step.style;
    CssCounterFlow flow = flowOf(style);
    const CssCounterStyles* styles = &p.presentation->rules.counterStyles;
    p.texts.text.clear();
    p.texts.ends.clear();
    auto close = [&]() {
        p.texts.text.append(p.text.data, p.text.size);
        p.texts.ends.push(p.texts.text.count);
        prTextBufferClear(&p.text);
    };
    if (marker) {
        uint32_t counter = counterFor(p, at, "list-item");
        int32_t value = p.counters.data[counter].value;
        cssCounterAppendMarker(&p.text, styles, cssComputedValue(style, PrCssProperty_ListStyleType), value, flow);
        close();
        return;
    }
    CssValueRun content = cssComputedValue(style, PrCssProperty_Content);
    for (uint32_t i = 0; i < content.count; i += content.values[i].next) {
        const CssValueNode* item = &content.values[i];
        if (cssValueIsLiteral(item, '/'))
            break;
        PrCssKeyword quote = quoteOf(item);
        if (quote != PrCssKeyword_None) {
            bool open = quote == PrCssKeyword_OpenQuote || quote == PrCssKeyword_NoOpenQuote;
            bool shown = quote == PrCssKeyword_OpenQuote || quote == PrCssKeyword_CloseQuote;
            bool stepped = open || p.quoteDepth != 0;
            if (!open && stepped)
                --p.quoteDepth;
            if (shown && stepped)
                prTextBufferAppend(&p.text, quoteMark(p, step.node, style, p.quoteDepth, open));
            if (open && p.quoteDepth != UINT32_MAX)
                ++p.quoteDepth;
            if (shown)
                close();
            continue;
        }
        bool one = cssValueIsFunction(item, PrCssKeyword_Counter);
        if (!one && !cssValueIsFunction(item, PrCssKeyword_Counters))
            continue;
        uint32_t starts[3];
        uint32_t count = cssValueArguments(content, i, starts, 3);
        PrStringView name = cssValueText(content, &content.values[starts[0]]);
        if (one) {
            uint32_t counter = counterFor(p, at, name);
            int32_t value = p.counters.data[counter].value;
            cssCounterAppend(&p.text, styles, styleArgument(content, starts, count, 1), value, flow);
        } else {
            counterFor(p, at, name);
            PrStringView separator = count > 1 ? cssValueText(content, &content.values[starts[1]]) : PrStringView{};
            bool first = true;
            for (uint32_t c = 0; c < p.counters.count; ++c) {
                if (p.counters.data[c].name != name)
                    continue;
                if (!first)
                    prTextBufferAppend(&p.text, separator);
                first = false;
                cssCounterAppend(&p.text, styles, styleArgument(content, starts, count, 2), p.counters.data[c].value, flow);
            }
        }
        close();
    }
}

// Returns true if the content of a pseudo-element reads a counter or steps the quote depth.
bool readsCounters(const CssComputedStyle* style) {
    CssValueRun content = cssComputedValue(style, PrCssProperty_Content);
    for (uint32_t i = 0; i < content.count; i += content.values[i].next) {
        if (cssValueIsFunction(&content.values[i], PrCssKeyword_Counter) ||
                cssValueIsFunction(&content.values[i], PrCssKeyword_Counters) ||
                quoteOf(&content.values[i]) != PrCssKeyword_None)
            return true;
    }
    return false;
}

// Appends the step of the pseudo-element of `entry` at `which`, at `depth`, if it generates a box, and the step of its
// own marker after it. Sets `counts` if either is a participant.
void generated(Pass& p, const CssPresentationEntry& entry, uint32_t which, uint32_t depth, bool* counts) {
    CssPresentationPseudo* pseudo = entry.pseudos[which];
    uint32_t at = pseudo != nullptr ? cssPresentationNodeEntry(p.presentation, pseudo) : kHashMissing;
    if (at == kHashMissing || p.presentation->entries.data[at].style == nullptr)
        return;
    const CssPresentationEntry& box = p.presentation->entries.data[at];
    *counts = *counts || box.counts;
    p.steps.push(Step{pseudo, box.style, depth, box.listItem});
    if (box.listItem)
        generated(p, box, kMarkerPseudo, depth, counts);
}

// Appends the steps of `element` and its subtree, if it has a box or passes its children's boxes to its parent. Returns
// true if the subtree holds a participant.
//
// Scopes follow the boxes. An element of display contents has no box, so it takes no step of its own, and its children
// and pseudo-elements are steps among its siblings, at its depth.
bool collect(Pass& p, const PrDomElement* element, uint32_t depth, CssCountingWalk walk) {
    PrCssPresentation* r = p.presentation;
    uint32_t at = cssPresentationNodeEntry(r, element);
    if (at != kHashMissing && r->entries.data[at].counterChanged)
        ++p.found;
    if (at == kHashMissing || (!r->entries.data[at].boxed && !r->entries.data[at].contents))
        return false;
    if (walk == CssCountingWalk_Participants && !r->entries.data[at].counts && !r->entries.data[at].countsBelow)
        return false;
    bool contents = !r->entries.data[at].boxed;
    if (!contents)
        p.steps.push(Step{element, r->entries.data[at].style, depth, r->entries.data[at].listItem});
    uint32_t inner = contents ? depth : depth + 1;
    bool below = false;
    // The pseudo-elements are children in tree order: ::marker, ::before, the element's children, then ::after.
    const uint32_t first[] = {kMarkerPseudo, kBeforePseudo};
    for (uint32_t which : first)
        generated(p, r->entries.data[at], which, inner, &below);
    domFlatEach(element, [&](PrDomNode* child) PR_FORCE_INLINE_LAMBDA {
        if (const PrDomElement* e = domElementFromNode(child))
            below = collect(p, e, inner, walk) || below;
    });
    generated(p, r->entries.data[at], kAfterPseudo, inner, &below);
    if (!contents)
        p.steps.push(Step{nullptr, nullptr, depth, false});
    if (walk == CssCountingWalk_Participants)
        r->entries.data[at].countsBelow = below;
    return r->entries.data[at].counts || below;
}

// --- Starting past the root ---

// Returns the depth of the deepest element of the chain that holds `node`, or UINT32_MAX if the chain holds `node`
// itself or none of its ancestors, so that `node` may come before the chain's points.
uint32_t chainDepthOf(const CssCountingResume& resume, const PrDomNode* node) {
    const RawArray<const PrDomElement*>& chain = resume.chain;
    for (uint32_t k = 0; k < chain.count; ++k) {
        if (chain.data[k] == node)
            return UINT32_MAX;
    }
    for (const PrDomNode* at = domFlatParent(node); at != nullptr; at = domFlatParent(at)) {
        for (uint32_t k = chain.count; k-- > 0;) {
            if (chain.data[k] == at)
                return k;
        }
    }
    return UINT32_MAX;
}

// Appends the steps that follow the point of the chain's element at depth `from` (CssCountingResume): for that element
// and each of its ancestors in turn, its children after the chain's element below it, or every child of the last
// element, which had none walked, then its ::after and its end.
void collectFrom(Pass& p, const CssCountingResume& resume, uint32_t from) {
    PrCssPresentation* r = p.presentation;
    uint32_t last = resume.chain.count - 1;
    for (uint32_t depth = from + 1; depth-- > 0;) {
        const PrDomElement* element = resume.chain.data[depth];
        DomFlatChildren children;
        if (depth == last)
            children = domFlatChildren(element);
        else
            children = domFlatChildrenAfter(element, resume.chain.data[depth + 1]);
        while (PrDomNode* child = domFlatNext(children)) {
            if (const PrDomElement* e = domElementFromNode(child))
                collect(p, e, depth + 1, CssCountingWalk_Participants);
        }
        bool below = false;
        generated(p, r->entries.data[cssPresentationNodeEntry(r, element)], kAfterPseudo, depth + 1, &below);
        p.steps.push(Step{nullptr, nullptr, depth, false});
    }
}

// Sets the counters, boundaries and quote depth of the pass to those of `point`, with the counters' names copied into
// the scratch names, which hold them for the run.
void restorePoint(Pass& p, const CssCountingPoint& point) {
    RawArray<char>& names = p.presentation->counterScratch.names;
    names.clear();
    names.reserve(point.names.count);
    p.counters.clear();
    for (uint32_t i = 0; i < point.counters.count; ++i) {
        Counter counter = point.counters.data[i];
        const char* name = names.data + names.count;
        names.append(counter.name.data, uint32_t(counter.name.size));
        counter.name = PrStringView{name, counter.name.size};
        p.counters.push(counter);
    }
    p.boundaries.clear();
    p.boundaries.append(point.boundaries.span());
    p.quoteDepth = point.quoteDepth;
}

// Keeps the counters, boundaries and quote depth of the pass in `point`, with copies of the counters' names.
void savePoint(const Pass& p, CssCountingPoint& point) {
    uint32_t size = 0;
    for (uint32_t i = 0; i < p.counters.count; ++i)
        size += uint32_t(p.counters.data[i].name.size);
    point.counters.clear();
    point.names.clear();
    point.names.reserve(size);
    for (uint32_t i = 0; i < p.counters.count; ++i) {
        Counter counter = p.counters.data[i];
        const char* name = point.names.data + point.names.count;
        point.names.append(counter.name.data, uint32_t(counter.name.size));
        counter.name = PrStringView{name, counter.name.size};
        point.counters.push(counter);
    }
    point.boundaries.clear();
    point.boundaries.append(p.boundaries.span());
    point.quoteDepth = p.quoteDepth;
}

// Returns true if a reversed counter is in scope at `point`, whose initial value counts the increments that follow.
bool holdsReversed(const CssCountingPoint& point) {
    for (uint32_t i = 0; i < point.counters.count; ++i) {
        if (point.counters.data[i].reversed)
            return true;
    }
    return false;
}

// Returns true if the step at `at` is a pseudo-element step of an ::after, or of its marker.
bool isAfter(const Step& step) {
    if (step.node == nullptr || step.node->type != DomTargetType_PseudoElement)
        return false;
    CssPseudo pseudo = static_cast<const CssPresentationPseudo*>(step.node)->pseudo;
    return pseudo == CssPseudo_After || pseudo == CssPseudo_AfterMarker;
}

// Finds, in the steps that the run collected, the point of each element of the chain that they end with: the step
// before which the element closes, which is its ::after, or its end. Writes the step of each depth's point to `at` by
// depth, from the root's, and returns the number of points. Sets `*last` to the element whose point is the deepest, if
// the steps hold its own step, and to null if they start at that point, so that the chain below it is the last run's.
uint32_t closingPoints(const Pass& p, RawArray<uint32_t>& at, const PrDomElement* PR_NULLABLE* last) {
    at.clear();
    *last = nullptr;
    const RawArray<Step>& steps = p.steps;
    if (steps.count == 0 || steps.last().node != nullptr || steps.last().depth != 0)
        return 0;
    uint32_t i = steps.count - 1;
    for (uint32_t depth = 0;; ++depth) {
        uint32_t point = i;
        while (point > 0 && steps.data[point - 1].depth == depth + 1 && isAfter(steps.data[point - 1]))
            --point;
        at.push(point);
        if (point == 0)
            return at.count;
        i = point - 1;
        if (steps.data[i].node != nullptr || steps.data[i].depth != depth + 1)
            break;
    }
    // The chain ends with the element whose end it last found, which had no element walked inside it.
    uint32_t depth = at.count - 1;
    for (uint32_t k = i + 1; k-- > 0;) {
        const Step& step = steps.data[k];
        if (step.node != nullptr && step.node->type != DomTargetType_PseudoElement && step.depth == depth) {
            *last = static_cast<const PrDomElement*>(step.node);
            break;
        }
    }
    return at.count;
}

// Sets the chain from `last`, the deepest element of a new chain at depth `depth`, and its ancestors, or clears it if
// one of them has no box of its own, in which case the next run starts at the root.
void chainFrom(PrCssPresentation* r, CssCountingResume& resume, const PrDomElement* last, uint32_t depth) {
    resume.chain.resize(depth + 1);
    const PrDomNode* node = last;
    for (uint32_t k = depth + 1; k-- > 0;) {
        uint32_t at = node != nullptr ? cssPresentationNodeEntry(r, node) : kHashMissing;
        if (at == kHashMissing || !r->entries.data[at].boxed || domElementFromNode(const_cast<PrDomNode*>(node)) == nullptr) {
            resume.chain.clear();
            return;
        }
        resume.chain.data[k] = static_cast<const PrDomElement*>(node);
        node = domFlatParent(node);
    }
    if (resume.chain.data[0] != domDocumentElement(r->document))
        resume.chain.clear();
}

} // namespace

bool cssIsListItem(const CssComputedStyle* style) {
    CssValueRun display = cssComputedValue(style, PrCssProperty_Display);
    for (uint32_t i = 0; i < display.count; ++i) {
        if (cssValueIsKeyword(&display.values[i], PrCssKeyword_ListItem))
            return true;
    }
    return false;
}

bool cssContainsStyle(const CssComputedStyle* style) {
    CssValueRun contain = cssComputedValue(style, PrCssProperty_Contain);
    for (uint32_t i = 0; i < contain.count; ++i) {
        if (cssValueIsKeyword(&contain.values[i], PrCssKeyword_Style) ||
                cssValueIsKeyword(&contain.values[i], PrCssKeyword_Strict) ||
                cssValueIsKeyword(&contain.values[i], PrCssKeyword_Content))
            return true;
    }
    if (!cssValueIsKeyword(cssComputedValue(style, PrCssProperty_ContentVisibility).values, PrCssKeyword_Visible))
        return true;
    CssValueRun container = cssComputedValue(style, PrCssProperty_ContainerType);
    for (uint32_t i = 0; i < container.count; ++i) {
        if (cssValueIsKeyword(&container.values[i], PrCssKeyword_Size) ||
                cssValueIsKeyword(&container.values[i], PrCssKeyword_InlineSize))
            return true;
    }
    return false;
}

bool cssCountsElement(const PrCssPresentation* presentation, const CssComputedStyle* style, bool listItem) {
    // A style whose Generated group is the initial one changes no counter.
    if (listItem || style->groups[CssStyleGroup_Generated] == presentation->styles.initial[CssStyleGroup_Generated])
        return listItem;
    return cssChangesCounters(cssComputedValue(style, PrCssProperty_CounterReset)) ||
        cssChangesCounters(cssComputedValue(style, PrCssProperty_CounterIncrement)) ||
        cssChangesCounters(cssComputedValue(style, PrCssProperty_CounterSet));
}

bool cssCountsPseudo(const PrCssPresentation* presentation, const CssComputedStyle* style, CssPseudo pseudo) {
    if (cssPseudoIsMarker(pseudo) || cssIsListItem(style))
        return true;
    if (style->groups[CssStyleGroup_Generated] == presentation->styles.initial[CssStyleGroup_Generated])
        return false;
    return readsCounters(style) || cssChangesCounters(cssComputedValue(style, PrCssProperty_CounterReset)) ||
        cssChangesCounters(cssComputedValue(style, PrCssProperty_CounterIncrement)) ||
        cssChangesCounters(cssComputedValue(style, PrCssProperty_CounterSet));
}

bool cssReadsQuotes(const CssComputedStyle* style) {
    CssValueRun content = cssComputedValue(style, PrCssProperty_Content);
    for (uint32_t i = 0; i < content.count; i += content.values[i].next) {
        if (quoteOf(&content.values[i]) != PrCssKeyword_None)
            return true;
    }
    return false;
}

void cssCountsBelow(PrCssPresentation* presentation, const PrDomNode* node) {
    for (const PrDomNode* at = domFlatParent(node); at != nullptr; at = domFlatParent(at)) {
        uint32_t entry = cssPresentationNodeEntry(presentation, at);
        if (entry == kHashMissing || presentation->entries.data[entry].countsBelow)
            return;
        presentation->entries.data[entry].countsBelow = true;
    }
}

void cssCountersChanged(PrCssPresentation* presentation, const PrDomNode* PR_NULLABLE element) {
    presentation->countersStale = true;
    uint32_t at = element != nullptr ? cssPresentationNodeEntry(presentation, element) : kHashMissing;
    if (at == kHashMissing) {
        presentation->countersFromRoot = true;
        return;
    }
    if (!presentation->entries.data[at].counterChanged) {
        presentation->entries.data[at].counterChanged = true;
        presentation->counterChanges.push(element);
    }
}

void cssCounterResumeReset(CssCountingResume& resume) {
    resume.chain.reset();
    for (uint32_t i = 0; i < resume.points.count; ++i) {
        resume.points.data[i].counters.reset();
        resume.points.data[i].boundaries.reset();
        resume.points.data[i].names.reset();
    }
    resume.points.reset();
}

void cssCountersRun(PrCssPresentation* presentation, CssCountingWalk walk, CssCountingTextsFn fn, void* PR_NULLABLE context) {
    PrDomElement* root = domDocumentElement(presentation->document);
    CssCountingScratch& scratch = presentation->counterScratch;
    CssCountingResume& resume = presentation->counterResume;
    bool participants = walk == CssCountingWalk_Participants;
    Pass p{
        .presentation = presentation,
        .steps = scratch.steps,
        .counters = scratch.counters,
        .texts = scratch.texts,
        .text = scratch.text,
        .initial = presentation->styles.initial[CssStyleGroup_Generated],
        .boundaries = scratch.boundaries,
        .found = 0,
        .quoteDepth = 0,
    };
    p.steps.clear();
    p.counters.clear();
    p.boundaries.clear();
    // A run whose changes all follow the points of the last run's chain starts at the deepest point that they follow.
    // The point is where it would be in a run from the root, since nothing before it changed, unless a reversed counter
    // in scope there counts what follows.
    bool resumed = false;
    if (participants && root != nullptr && !presentation->countersFromRoot && presentation->counterChanges.count != 0 &&
            resume.chain.count != 0) {
        uint32_t from = 0;
        for (uint32_t i = 0; i < presentation->counterChanges.count && from != UINT32_MAX; ++i) {
            uint32_t depth = chainDepthOf(resume, presentation->counterChanges.data[i]);
            from = depth == UINT32_MAX || depth > from ? depth : from;
        }
        if (from != UINT32_MAX && !holdsReversed(resume.points.data[from])) {
            collectFrom(p, resume, from);
            resumed = p.found == presentation->counterChanges.count;
            if (resumed)
                restorePoint(p, resume.points.data[from]);
            else
                p.steps.clear();
        }
    }
    if (!resumed && root != nullptr)
        collect(p, root, 0, walk);
    // The points of the chain that the steps end with, which the run keeps for the next. A run that starts past the
    // root keeps the points below where it starts, and the chain there, unless it walks a new chain's elements.
    RawArray<uint32_t>& points = scratch.points;
    const PrDomElement* last = nullptr;
    uint32_t pointCount = participants ? closingPoints(p, points, &last) : 0;
    if (participants && last != nullptr)
        chainFrom(presentation, resume, last, pointCount - 1);
    else if (participants && (!resumed || pointCount == 0))
        resume.chain.clear();
    while (resume.points.count < resume.chain.count)
        resume.points.emplace() = CssCountingPoint{};
    uint32_t next = pointCount;     // the depth of the next point to keep, by its step, from the deepest
    for (uint32_t at = 0; at < p.steps.count; ++at) {
        while (next != 0 && points.data[next - 1] == at && resume.chain.count != 0) {
            --next;
            savePoint(p, resume.points.data[next]);
        }
        const Step& step = p.steps.data[at];
        if (step.node == nullptr) {
            while (p.counters.count != 0 && (p.counters.last().depth > step.depth ||
                    (p.counters.last().narrow && p.counters.last().depth == step.depth)))
                p.counters.pop();
            if (p.boundaries.count != 0 && p.boundaries.last().depth == step.depth)
                p.boundaries.pop();
            continue;
        }
        applyCounters(p, at);
        // The element itself is outside the subtree that its style containment scopes.
        if (step.node->type != DomTargetType_PseudoElement && cssContainsStyle(step.style))
            p.boundaries.push(Boundary{step.depth, p.counters.count});
        if (step.node->type != DomTargetType_PseudoElement)
            continue;
        CssPresentationPseudo* pseudo = static_cast<CssPresentationPseudo*>(const_cast<PrDomNode*>(step.node));
        // A style whose Generated group is the initial one has a content of normal.
        bool initial = step.style->groups[CssStyleGroup_Generated] == p.initial;
        bool normal = initial || cssValueIsKeyword(cssComputedValue(step.style, PrCssProperty_Content).values,
            PrCssKeyword_Normal);
        bool marker = cssPseudoIsMarker(pseudo->pseudo) && normal;
        bool kept = false;
        if (marker && participants) {
            uint32_t counter = counterFor(p, at, "list-item");
            int32_t value = p.counters.data[counter].value;
            kept = pseudo->markerKept && pseudo->markerValue == value;
            pseudo->markerKept = true;
            pseudo->markerValue = value;
        }
        if (!kept && (marker || (!initial && readsCounters(step.style)))) {
            readCounters(p, at, marker);
            fn(context, pseudo, &p.texts);
        }
        // A pseudo-element has no subtree, so a narrow scope that it starts ends with it.
        while (p.counters.count != 0 && p.counters.last().narrow && p.counters.last().depth == step.depth)
            p.counters.pop();
    }
    if (!participants)
        return;
    for (uint32_t i = 0; i < presentation->counterChanges.count; ++i) {
        uint32_t at = cssPresentationNodeEntry(presentation, presentation->counterChanges.data[i]);
        if (at != kHashMissing)
            presentation->entries.data[at].counterChanged = false;
    }
    presentation->counterChanges.clear();
    presentation->countersFromRoot = false;
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
