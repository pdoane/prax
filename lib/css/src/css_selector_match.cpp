// Selector matching
//
// selectors-4 - https://drafts.csswg.org/selectors-4/
// selectors-5 - https://drafts.csswg.org/selectors-5/
// html - https://html.spec.whatwg.org/

#include "css/src/css_selector_match.h"

#include "pr/hash.h"
#include "pr/memory.h"
#include "pr/name_table.h"
#include "pr/string_ops.h"

#include "css/src/css_parser.h"
#include "dom/src/dom_attribute.h"
#include "dom/src/dom_custom.h"
#include "dom/src/dom_element_state.h"
#include "dom/src/dom_select.h"
#include "dom/src/dom_slot.h"

namespace pr_impl {
namespace {

// The result of matching a complex selector from one of its compounds
// (selectors-4/#match-a-complex-selector-against-an-element).
enum {
    Match_Yes,
    Match_Fails,            // fails at this element; another element that the combinator reaches may match
    Match_FailsSiblings,    // fails at this element and at every sibling before it
    Match_FailsAll,         // fails at every element that a combinator further right would try next
};
typedef uint8_t Match;

// The length of the longest subtag of a well-formed language tag (selectors-4/#the-lang-pseudo).
constexpr size_t kLanguageSubtagMaxLength = 8;

Match matchFrom(CssSelectorMatcher* m, const CssComplexSelector* complex, uint32_t index,
    const PrDomElement* element, const PrDomElement* PR_NULLABLE anchor);
Match matchMaybeHost(CssSelectorMatcher* m, const CssComplexSelector* complex, uint32_t index,
    const PrDomElement* element, const PrDomElement* PR_NULLABLE anchor);

// ---------------------------------------------------------------------------------------------------------------------
// The tree

// Returns the parent of `element` in the tree that the matcher matches in: its parent element, or the host for a top
// element of the matcher's shadow tree. Returns null for the host, which is the root of that tree.
PR_FORCE_INLINE const PrDomElement* PR_NULLABLE parentInTree(const CssSelectorMatcher* m, const PrDomElement* element) {
    const PrDomElement* host = m->host;
    if (host == nullptr)
        return domNodeParentElement(element);
    if (element == host)
        return nullptr;
    const PrDomShadowRoot* shadow = domNodeShadowRoot(element->parent);
    return shadow != nullptr && shadow->host == host ? host : domNodeParentElement(element);
}

// Returns true if `compound` matches a featureless host: each of its simple selectors is :host, :host(),
// :host-context(), a universal selector or a pseudo-element, and one is of the :host kind
// (css-shadow-1/#host-element-in-tree).
bool matchesFeatureless(const PrCssSelectors* selectors, const CssCompoundSelector* compound) {
    bool host = false;
    for (uint32_t k = 0; k < compound->simpleCount; ++k) {
        const CssSimpleSelector* simple = &selectors->simple[compound->simple + k];
        switch (simple->kind) {
            case CssSimpleKind_PseudoClass:
                if (simple->pseudoClass != CssPseudoClass_Host && simple->pseudoClass != CssPseudoClass_HostFn &&
                        simple->pseudoClass != CssPseudoClass_HostContextFn)
                    return false;
                host = true;
                break;
            case CssSimpleKind_Universal:
            case CssSimpleKind_PseudoElement:
                break;
            default:
                return false;
        }
    }
    return host;
}

// Returns the element siblings. A root's sibling links join the detached roots, which are not siblings of each other.
const PrDomElement* PR_NULLABLE previousElement(const PrDomElement* element) {
    if (element->parent == nullptr)
        return nullptr;
    for (const PrDomNode* node = element->previous; node != nullptr; node = node->previous) {
        if (node->type == PrDomNodeType_Element)
            return static_cast<const PrDomElement*>(node);
    }
    return nullptr;
}

const PrDomElement* PR_NULLABLE nextElement(const PrDomElement* element) {
    if (element->parent == nullptr)
        return nullptr;
    for (const PrDomNode* node = element->next; node != nullptr; node = node->next) {
        if (node->type == PrDomNodeType_Element)
            return static_cast<const PrDomElement*>(node);
    }
    return nullptr;
}

// Returns true if two elements have the same namespace and local name.
bool typeEquals(const PrDomElement* a, const PrDomElement* b) {
    if (a->space != b->space)
        return false;
    if (a->document == b->document || !domTagIsAtom(a->tag))
        return a->tag == b->tag;
    return domElementName(a) == domElementName(b);
}

// Returns true if `word` is one of the words of `list`, which are separated by ASCII whitespace. No word is empty or
// contains whitespace, so an empty `word`, or one that contains whitespace, never matches.
bool holdsWord(PrStringView list, PrStringView word, bool fold) {
    size_t at = 0;
    while (at < list.size) {
        if (prAsciiIsSpace(list.data[at])) {
            ++at;
            continue;
        }
        size_t start = at;
        while (at < list.size && !prAsciiIsSpace(list.data[at]))
            ++at;
        PrStringView token = prStringSlice(list, start, at - start);
        if (fold ? prAsciiEqualsFold(token, word) : token == word)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Names

// Returns true if `space` is the namespace that the selector's prefix names (selectors-4/#type-nmsp,
// selectors-4/#attrnmsp). Without a prefix, an element's selector accepts any namespace if the sheet declares no
// default namespace. An attribute's selector without a prefix accepts only an attribute in no namespace.
bool inNamespace(CssSelectorMatcher* m, const CssSimpleSelector* simple, const PrDomNode* node, bool element) {
    DomNamespaceId space = node->space;
    switch (simple->prefix) {
        case CssNamespacePrefix_Any:
            return true;
        case CssNamespacePrefix_Empty:
            return space == PrDomNamespace_None;
        default: {
            if (simple->space == kCssSelectorNone)
                return element || space == PrDomNamespace_None;
            const CssSelectorNamespace* wanted = &m->selectors->namespaces[simple->space];
            if (wanted->named != PrDomNamespace_Other)
                return space == wanted->named;
            return domNamespaceUrl(node->document, space) == cssSelectorName(m->selectors, wanted->url);
        }
    }
}

// Returns the tag of the lowered local name at `name`, among the atoms of the document being matched. Returns
// PrDomTag_Unknown, which is no element's tag, if no element of the document has that name.
DomTagId localNameTag(CssSelectorMatcher* m, uint32_t name) {
    for (uint32_t i = 0; i < m->localNames.count; ++i) {
        if (m->localNames.data[i].name == name)
            return m->localNames.data[i].tag;
    }
    const AtomTable* names = m->document->localNames;
    Atom atom = names != nullptr ? atomTableFind(names, cssSelectorNameLowered(m->selectors, name)) : kAtomNone;
    DomTagId tag = atom != kAtomNone ? PrDomTag_Count + atom : DomTagId(PrDomTag_Unknown);
    m->localNames.push(CssLocalName{name, tag});
    return tag;
}

// html/#case-sensitivity-of-selectors: an HTML element's name is compared with the selector's name in ASCII lowercase.
// Every other element's name is compared with the selector's name as written.
bool matchesType(CssSelectorMatcher* m, const CssSimpleSelector* simple, const PrDomElement* element) {
    if (!inNamespace(m, simple, element, true))
        return false;
    if (!domIsHtmlInHtmlDocument(element))
        return domElementName(element) == cssSelectorName(m->selectors, simple->name);
    if (!domTagIsAtom(element->tag))
        return element->tag == simple->tag;
    if (element->document != m->document)
        return domElementName(element) == cssSelectorNameLowered(m->selectors, simple->name);
    return element->tag == localNameTag(m, simple->name);
}

// Returns the first attribute from `from` on whose namespace and name match the attribute selector `simple`. For an
// attribute in no namespace of an HTML element in an HTML document, the name is compared with the selector's name in
// ASCII lowercase. Every other name is compared with the selector's name as written
// (html/#case-sensitivity-of-selectors).
const PrDomAttr* PR_NULLABLE nextNamed(CssSelectorMatcher* m, const CssSimpleSelector* simple,
        const PrDomElement* element, const PrDomAttr* PR_NULLABLE from) {
    const CssSelectorName* name = &m->selectors->names[simple->name];
    for (const PrDomAttr* attribute = from; attribute != nullptr;
            attribute = static_cast<const PrDomAttr*>(attribute->next)) {
        if (attribute->hash != name->hash || !inNamespace(m, simple, attribute, false))
            continue;
        bool lowered = domIsHtmlInHtmlDocument(element) && attribute->space == PrDomNamespace_None;
        PrStringView wanted;
        if (lowered)
            wanted = cssSelectorNameLowered(m->selectors, simple->name);
        else
            wanted = cssSelectorName(m->selectors, simple->name);
        if (prRawStringView(&attribute->name) == wanted)
            return attribute;
    }
    return nullptr;
}

// Returns true if `value` satisfies the attribute selector's operator, which is not Exists
// (selectors-4/#attribute-representation, selectors-4/#attribute-substrings). `plain` means the attribute is in no
// namespace, on an HTML element in an HTML document.
bool matchesValue(CssSelectorMatcher* m, const CssSimpleSelector* simple, PrStringView value, bool plain) {
    PrStringView wanted = cssSelectorName(m->selectors, simple->value);
    bool fold = simple->letterCase == CssAttributeCase_Insensitive || (simple->letterCase == CssAttributeCase_Html && plain);
    switch (simple->match) {
        case CssAttributeMatch_Equals:
            return fold ? prAsciiEqualsFold(value, wanted) : value == wanted;
        case CssAttributeMatch_Includes:
            return holdsWord(value, wanted, fold);
        case CssAttributeMatch_DashMatch:
            return (fold ? prAsciiStartsWithFold(value, wanted) : prStringStartsWith(value, wanted)) &&
                (value.size == wanted.size || value.data[wanted.size] == '-');
        case CssAttributeMatch_Prefix:
            return wanted.size != 0 && (fold ? prAsciiStartsWithFold(value, wanted) : prStringStartsWith(value, wanted));
        case CssAttributeMatch_Suffix:
            return wanted.size != 0 && (fold ? prAsciiEndsWithFold(value, wanted) : prStringEndsWith(value, wanted));
        case CssAttributeMatch_Substring:
            return wanted.size != 0 && (fold ? prAsciiContainsFold(value, wanted) : prStringContains(value, wanted));
        default:
            return false;
    }
}

bool matchesAttribute(CssSelectorMatcher* m, const CssSimpleSelector* simple, const PrDomElement* element) {
    const PrDomAttr* attribute = nextNamed(m, simple, element, element->firstAttribute);
    if (simple->match == CssAttributeMatch_Exists)
        return attribute != nullptr;
    for (; attribute != nullptr;
            attribute = nextNamed(m, simple, element, static_cast<const PrDomAttr*>(attribute->next))) {
        bool plain = domIsHtmlInHtmlDocument(element) && attribute->space == PrDomNamespace_None;
        if (matchesValue(m, simple, prRawStringView(&attribute->value), plain))
            return true;
    }
    return false;
}

// selectors-4/#id-selectors, selectors-4/#class-html: compared exactly, and ASCII case-insensitively in quirks mode.
bool matchesId(CssSelectorMatcher* m, PrStringView id, PrStringView name) {
    return m->quirks ? prAsciiEqualsFold(id, name) : id == name;
}

// Returns the atom of the class name at `name`, in the document being matched.
Atom classAtom(CssSelectorMatcher* m, uint32_t name) {
    for (uint32_t i = 0; i < m->classes.count; ++i) {
        if (m->classes.data[i].name == name)
            return m->classes.data[i].atom;
    }
    Atom atom = domClassesFind(m->document, cssSelectorName(m->selectors, name));
    m->classes.push(CssClassAtom{name, atom});
    return atom;
}

bool matchesClass(CssSelectorMatcher* m, const CssSimpleSelector* simple, const PrDomElement* element) {
    if (element->classes == kDomClassesNone)
        return false;
    if (m->quirks || element->document != m->document) {
        PrStringView name = cssSelectorName(m->selectors, simple->name);
        DomClassList classes = domClassesList(element->document, element->classes);
        for (uint32_t i = 0; i < classes.count; ++i) {
            PrStringView text = domClassesName(element->document, classes.atoms[i]);
            if (m->quirks ? prAsciiEqualsFold(text, name) : text == name)
                return true;
        }
        return false;
    }
    Atom atom = classAtom(m, simple->name);
    return atom != kAtomNone && domClassesHas(m->document, element->classes, atom);
}

// ---------------------------------------------------------------------------------------------------------------------
// Tree-structural pseudo-classes (selectors-4/#structural-pseudos)

bool isRoot(const PrDomElement* element) {
    return element->parent != nullptr && element->parent->type == PrDomNodeType_Document;
}

// selectors-4/#the-scope-pseudo
bool matchesScope(CssSelectorMatcher* m, const PrDomElement* element) {
    return m->scope != nullptr ? element == m->scope : isRoot(element);
}

// selectors-4/#the-empty-pseudo: the element has no element children, and no text other than white space.
bool isEmpty(const PrDomElement* element) {
    for (const PrDomNode* child = element->first; child != nullptr; child = child->next) {
        if (child->type == PrDomNodeType_Element)
            return false;
        if (!domIsText(child))
            continue;
        PrStringView data = prRawStringView(&static_cast<const PrDomText*>(child)->data);
        for (size_t i = 0; i < data.size; ++i) {
            if (!prAsciiIsSpace(data.data[i]))
                return false;
        }
    }
    return true;
}

bool matchesList(CssSelectorMatcher* m, CssSelectorList list, const PrDomElement* element);

// Returns true if a position counted from one matches An+B for some n of zero or more (css-syntax-3/#anb-microsyntax).
bool matchesNth(int32_t a, int32_t b, uint32_t position) {
    int64_t offset = int64_t(position) - b;
    if (a == 0)
        return offset == 0;
    return offset % a == 0 && (offset == 0 || (offset > 0) == (a > 0));
}

// The number of slots that a matcher's memos start with.
constexpr uint32_t kCssMemoInitial = 64;

// The parts of a memo's key. A key identifies a count of siblings, or a search from the element. A count runs from the
// first or the last sibling, and counts every element, the elements of the element's type, or the elements that an `of`
// list matches. A search is what a :has() argument, or one of its selectors that is a single compound, finds from the
// element. The low bits say which kind of memo it is, and the rest identify the argument or the selector.
constexpr uint32_t kMemoLast = 1;
constexpr uint32_t kMemoOfType = 2;
constexpr uint32_t kMemoHas = 4;       // the argument matches from the element
constexpr uint32_t kMemoBelow = 5;     // a descendant of the element matches the selector
constexpr uint32_t kMemoLater = 6;     // a later sibling of the element matches the selector

// The result of matching the compounds from one compound leftward, from the element or one of its ancestors, for a
// descendant combinator.
constexpr uint32_t kMemoAncestors = 7;

constexpr uint32_t kMemoShift = 3;

// Returns the slot that a memo's probe starts at.
uint32_t memoSlot(const CssMatchMemos* memos, const PrDomElement* element, uint32_t key) {
    return prHashPointer(element, uint64_t(key) << 40) & memos->mask;
}

// Returns the memo of `key` for `element`, or null if there is none. Adding a memo may move the others.
const CssMatchMemo* PR_NULLABLE findMemo(const CssSelectorMatcher* m, const PrDomElement* element, uint32_t key) {
    const CssMatchMemos* memos = &m->memos;
    if (memos->slots == nullptr)
        return nullptr;
    for (uint32_t slot = memoSlot(memos, element, key);; slot = (slot + 1) & memos->mask) {
        const CssMatchMemo* at = &memos->slots[slot];
        if (at->element == nullptr)
            return nullptr;
        if (at->element == element && at->key == key)
            return at;
    }
}

void placeMemo(CssMatchMemos* memos, CssMatchMemo memo) {
    uint32_t slot = memoSlot(memos, memo.element, memo.key);
    while (memos->slots[slot].element != nullptr)
        slot = (slot + 1) & memos->mask;
    memos->slots[slot] = memo;
}

// Adds a memo. No memo of `key` for `element` may exist yet. The slots stay at most half full.
void addMemo(CssSelectorMatcher* m, const PrDomElement* element, uint32_t key, uint32_t value) {
    CssMatchMemos* memos = &m->memos;
    if (memos->slots == nullptr || (memos->count + 1) * 2 > memos->mask + 1) {
        CssMatchMemo* old = memos->slots;
        uint32_t size = old == nullptr ? 0 : memos->mask + 1;
        uint32_t grown = size == 0 ? kCssMemoInitial : size * 2;
        memos->slots = static_cast<CssMatchMemo*>(prMemoryAllocZeroed(sizeof(CssMatchMemo) * grown));
        memos->mask = grown - 1;
        for (uint32_t i = 0; i < size; ++i) {
            if (old[i].element != nullptr)
                placeMemo(memos, old[i]);
        }
        prMemoryFree(old);
    }
    placeMemo(memos, CssMatchMemo{element, key, value});
    ++memos->count;
}

// Returns true if a count of the siblings of `element` includes `sibling`. The count includes the siblings of the
// element's type, or the siblings that the list `of` matches.
bool counts(CssSelectorMatcher* m, const PrDomElement* element, const PrDomElement* sibling,
        bool ofType, CssSelectorList of) {
    return ofType ? typeEquals(sibling, element) : of.count == 0 || matchesList(m, of, sibling);
}

// Computes positionOf for elements queried out of order. The first query of a count of a parent's children counts all
// of the children, and keeps each position as a memo.
uint32_t positionMemo(CssSelectorMatcher* m, const PrDomElement* element, bool last, bool ofType, uint32_t argument) {
    const PrDomNode* parent = element->parent;
    CssSelectorList of = m->selectors->arguments[argument].selectors;
    uint32_t key = (last ? kMemoLast : 0) | (ofType ? kMemoOfType : 0) | (of.count != 0 ? (argument + 1) << kMemoShift : 0);
    const CssMatchMemo* memo = findMemo(m, element, key);
    if (memo != nullptr)
        return memo->value;

    uint32_t position = 0;
    for (const PrDomNode* node = last ? parent->last : parent->first; node != nullptr; node =
            last ? node->previous : node->next) {
        if (node->type != PrDomNodeType_Element)
            continue;
        const PrDomElement* sibling = static_cast<const PrDomElement*>(node);
        if (!counts(m, element, sibling, ofType, of))
            continue;
        addMemo(m, sibling, key, ++position);
    }
    memo = findMemo(m, element, key);
    PR_ASSERT(memo != nullptr);
    return memo->value;
}

// Returns the position of `element` among its element siblings, counted from one at the first or, with `last`, at the
// last. The count includes only siblings of its type, or only siblings that the `of` list of the argument at `argument`
// matches, which must include `element`. A count queried in element order counts back to the element that it counted
// last.
uint32_t positionOf(CssSelectorMatcher* m, const PrDomElement* element, bool last, bool ofType, uint32_t argument) {
    const PrDomNode* parent = element->parent;
    if (parent == nullptr)
        return 1;
    CssSelectorList of = m->selectors->arguments[argument].selectors;
    uint32_t key = (ofType ? kMemoOfType : 0) | (of.count != 0 ? (argument + 1) << kMemoShift : 0);
    uint32_t run = kArrayMissing;
    for (uint32_t i = 0; i < m->runs.count && run == kArrayMissing; ++i) {
        const CssNthRun* at = &m->runs.data[i];
        if (at->key == key && (!ofType || typeEquals(at->element, element)))
            run = i;
    }
    const PrDomElement* counted = run != kArrayMissing ? m->runs.data[run].element : nullptr;

    uint32_t position = 1;
    if (counted != element) {
        uint32_t steps = 0;
        const PrDomElement* at = previousElement(element);
        for (; at != nullptr && at != counted; at = previousElement(at)) {
            if (++steps > kCssNthWalk)
                return positionMemo(m, element, last, ofType, argument);
            position += counts(m, element, at, ofType, of) ? 1 : 0;
        }
        if (at != nullptr)
            position += m->runs.data[run].position;
        bool moved = counted == nullptr || counted->parent != parent;
        if (run == kArrayMissing) {
            run = m->runs.count;
            m->runs.push(CssNthRun{key, element, position, 0});
        }
        m->runs.data[run].element = element;
        m->runs.data[run].position = position;
        if (moved)
            m->runs.data[run].count = 0;
    } else {
        position = m->runs.data[run].position;
    }
    if (!last)
        return position;

    CssNthRun* entry = &m->runs.data[run];
    if (entry->count == 0) {
        uint32_t count = 0;
        for (const PrDomNode* node = parent->first; node != nullptr; node = node->next) {
            const PrDomElement* sibling = domElementFromNode(node);
            count += sibling != nullptr && counts(m, element, sibling, ofType, of) ? 1 : 0;
        }
        entry = &m->runs.data[run];
        entry->count = count;
    }
    return entry->count - position + 1;
}

bool matchesNthPseudo(CssSelectorMatcher* m, uint32_t argument, const PrDomElement* element, bool last, bool ofType) {
    const CssSelectorArgument* nth = &m->selectors->arguments[argument];
    if (nth->selectors.count != 0 && !matchesList(m, nth->selectors, element))
        return false;
    return matchesNth(nth->a, nth->b, positionOf(m, element, last, ofType, argument));
}

// Returns true if no element sibling before the element, or after it, has the element's type.
bool firstOfType(const PrDomElement* element) {
    for (const PrDomElement* sibling = previousElement(element); sibling != nullptr; sibling = previousElement(sibling)) {
        if (typeEquals(sibling, element))
            return false;
    }
    return true;
}

bool lastOfType(const PrDomElement* element) {
    for (const PrDomElement* sibling = nextElement(element); sibling != nullptr; sibling = nextElement(sibling)) {
        if (typeEquals(sibling, element))
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Logical combinations (selectors-4/#logical-combination)

bool matchesCompound(CssSelectorMatcher* m, const CssCompoundSelector* compound, const PrDomElement* element);

// Returns true if a descendant of `root` matches the relative selector `complex` from `anchor`. The descendant is
// exactly `depth` below `root`, or at any depth for kCssReachAny.
bool walkHas(CssSelectorMatcher* m, const CssComplexSelector* complex, const PrDomElement* root,
        uint32_t depth, const PrDomElement* anchor) {
    const PrDomNode* node = root->first;
    uint32_t level = 1;
    while (node != nullptr) {
        const PrDomElement* element = domElementFromNode(node);
        if (element != nullptr && (depth == kCssReachAny || level == depth) &&
                matchFrom(m, complex, 0, element, anchor) == Match_Yes)
            return true;
        if (node->first != nullptr && level < depth) {
            node = node->first;
            ++level;
            continue;
        }
        while (node->next == nullptr) {
            node = node->parent;
            --level;
            if (node == root)
                return false;
        }
        node = node->next;
    }
    return false;
}

// Returns true if a descendant of `root` matches `compound`, the single compound of the selector at `index`. The walk
// keeps a memo for each element that it finishes or finds a match below, so an anchor inside an element that was
// already searched finds its result there.
bool belowMatches(CssSelectorMatcher* m, uint32_t index, const CssCompoundSelector* compound, const PrDomElement* root) {
    uint32_t key = (index << kMemoShift) | kMemoBelow;
    const CssMatchMemo* memo = findMemo(m, root, key);
    if (memo != nullptr)
        return memo->value != 0;

    const PrDomNode* node = root->first;
    while (node != nullptr) {
        const PrDomElement* element = domElementFromNode(node);
        bool known = false;
        if (element != nullptr) {
            memo = findMemo(m, element, key);
            known = memo != nullptr;
            if (matchesCompound(m, compound, element) || (known && memo->value != 0)) {
                for (const PrDomElement* at = domNodeParentElement(element); at != root; at = domNodeParentElement(at))
                    addMemo(m, at, key, 1);
                addMemo(m, root, key, 1);
                return true;
            }
        }
        if (!known && node->first != nullptr) {
            node = node->first;
            continue;
        }
        while (node->next == nullptr) {
            node = node->parent;
            if (node == root)
                break;
            addMemo(m, static_cast<const PrDomElement*>(node), key, 0);
        }
        if (node == root)
            break;
        node = node->next;
    }
    addMemo(m, root, key, 0);
    return false;
}

// Returns true if a later sibling of `anchor` matches the selector at `index`. The selector starts with a
// subsequent-sibling combinator, and its other combinators are `k` next-sibling combinators. A sibling is the
// selector's subject if the leftmost compound matches the sibling `k` elements before it. So the selector matches from
// an element if it matches from the next element, or if its leftmost compound matches the next element. Every element
// that the walk passes has the same result, which is kept as a memo.
bool laterMatches(CssSelectorMatcher* m, uint32_t index, uint32_t k, const PrDomElement* anchor) {
    const CssComplexSelector* complex = &m->selectors->complex[index];
    uint32_t key = (index << kMemoShift) | kMemoLater;
    bool found = false;
    bool known = false;
    const PrDomElement* at = anchor;
    for (; at != nullptr; at = nextElement(at)) {
        const CssMatchMemo* memo = findMemo(m, at, key);
        if (memo != nullptr) {
            found = memo->value != 0;
            known = true;
            break;
        }
        const PrDomElement* subject = nextElement(at);
        for (uint32_t i = 0; i < k && subject != nullptr; ++i)
            subject = nextElement(subject);
        if (subject == nullptr)
            break;
        if (matchFrom(m, complex, 0, subject, at) == Match_Yes) {
            found = true;
            break;
        }
    }
    const PrDomElement* end = at != nullptr && !known ? nextElement(at) : at;
    for (const PrDomElement* passed = anchor; passed != end; passed = nextElement(passed))
        addMemo(m, passed, key, found ? 1 : 0);
    return found;
}

// Returns the number of next-sibling combinators that follow the leading subsequent-sibling combinator of a selector
// that reaches only siblings. Returns kCssReachAny if the selector has any other combinators.
uint32_t rigidSiblings(const PrCssSelectors* selectors, const CssComplexSelector* complex) {
    const CssCompoundSelector* compounds = &selectors->compounds[complex->compound];
    if (complex->depth != 0 || compounds[complex->compoundCount - 1].combinator != CssCombinator_SubsequentSibling)
        return kCssReachAny;
    for (uint32_t i = 0; i + 1 < complex->compoundCount; ++i) {
        if (compounds[i].combinator != CssCombinator_NextSibling)
            return kCssReachAny;
    }
    return complex->compoundCount - 1;
}

// Returns true if some element of the document has had each class that the compounds of `complex` name. This is checked
// only if the document keeps classes as atoms. A selector that names a class that no element has matches nothing.
bool classesPresent(CssSelectorMatcher* m, const CssComplexSelector* complex, const PrDomElement* anchor) {
    if (m->quirks || anchor->document != m->document)
        return true;
    for (uint32_t i = 0; i < complex->compoundCount; ++i) {
        const CssCompoundSelector* compound = &m->selectors->compounds[complex->compound + i];
        for (uint32_t j = 0; j < compound->simpleCount; ++j) {
            const CssSimpleSelector* simple = &m->selectors->simple[compound->simple + j];
            if (simple->kind == CssSimpleKind_Class && classAtom(m, simple->name) == kAtomNone)
                return false;
        }
    }
    return true;
}

// Returns true if an element that the relative selector at `index` reaches from `anchor` matches the selector. Searches
// only the positions that the selector's reach allows for its subject.
bool reachesMatch(CssSelectorMatcher* m, uint32_t index, const PrDomElement* anchor) {
    const CssComplexSelector* complex = &m->selectors->complex[index];
    const CssCompoundSelector* subject = &m->selectors->compounds[complex->compound];
    bool alone = complex->compoundCount == 1;
    switch (complex->reach) {
        case CssReach_Descendant:
            if (alone && complex->depth == kCssReachAny)
                return belowMatches(m, index, subject, anchor);
            return walkHas(m, complex, anchor, complex->depth, anchor);
        case CssReach_Sibling: {
            if (complex->siblings == kCssReachAny) {
                uint32_t rigid = rigidSiblings(m->selectors, complex);
                if (rigid != kCssReachAny)
                    return laterMatches(m, index, rigid, anchor);
                for (const PrDomElement* sibling = nextElement(anchor); sibling != nullptr;
                        sibling = nextElement(sibling)) {
                    bool found;
                    if (complex->depth == 0)
                        found = matchFrom(m, complex, 0, sibling, anchor) == Match_Yes;
                    else
                        found = walkHas(m, complex, sibling, complex->depth, anchor);
                    if (found)
                        return true;
                }
                return false;
            }
            const PrDomElement* sibling = anchor;
            for (uint32_t i = 0; i < complex->siblings && sibling != nullptr; ++i)
                sibling = nextElement(sibling);
            if (sibling == nullptr)
                return false;
            if (complex->depth == 0)
                return matchFrom(m, complex, 0, sibling, anchor) == Match_Yes;
            return walkHas(m, complex, sibling, complex->depth, anchor);
        }
        default: {
            const PrDomNode* root = domNodeRoot(anchor);
            for (const PrDomNode* node = root; node != nullptr; node = domNodeNext(root, node)) {
                const PrDomElement* element = domElementFromNode(node);
                if (element != nullptr && matchFrom(m, complex, 0, element, anchor) == Match_Yes)
                    return true;
            }
            return false;
        }
    }
}

// Returns true if an element that a relative selector of `list` reaches from `anchor` matches that selector
// (selectors-4/#relational).
bool searchHas(CssSelectorMatcher* m, CssSelectorList list, const PrDomElement* anchor) {
    for (uint32_t i = 0; i < list.count; ++i) {
        const CssComplexSelector* complex = &m->selectors->complex[list.first + i];
        if ((complex->flags & CssComplexFlag_Invalid) == 0 && classesPresent(m, complex, anchor) &&
                reachesMatch(m, list.first + i, anchor))
            return true;
    }
    return false;
}

// Returns true if searching from an anchor for the selector costs no more than a memo of the result. That holds if the
// search keeps its own memos, or tests a bounded number of elements that are no further from the anchor than the
// selector has compounds.
bool searchIsCheap(const PrCssSelectors* selectors, const CssComplexSelector* complex) {
    switch (complex->reach) {
        case CssReach_Descendant:
            return complex->compoundCount == 1 && complex->depth == kCssReachAny;
        case CssReach_Sibling:
            return complex->depth == 0 &&
                (complex->siblings != kCssReachAny || rigidSiblings(selectors, complex) != kCssReachAny);
        default:
            return false;
    }
}

// Returns the result of searchHas, which is computed once for each anchor.
bool matchesHas(CssSelectorMatcher* m, CssSelectorList list, const PrDomElement* anchor) {
    if (list.count == 1 && searchIsCheap(m->selectors, &m->selectors->complex[list.first]))
        return searchHas(m, list, anchor);
    uint32_t key = (list.first << kMemoShift) | kMemoHas;
    const CssMatchMemo* memo = findMemo(m, anchor, key);
    if (memo != nullptr)
        return memo->value != 0;
    bool matches = searchHas(m, list, anchor);
    addMemo(m, anchor, key, matches ? 1 : 0);
    return matches;
}

// ---------------------------------------------------------------------------------------------------------------------
// The language pseudo-class (selectors-4/#the-lang-pseudo)

// Returns the subtag of a language tag or range at `*at`. Moves `*at` past the subtag and its hyphen.
PrStringView nextSubtag(PrStringView text, size_t* at) {
    size_t start = *at;
    size_t end = start;
    while (end < text.size && text.data[end] != '-')
        ++end;
    *at = end + 1;
    return prStringSlice(text, start, end - start);
}

// Returns true if each subtag is one to eight ASCII letters and digits, or, in a range, a wildcard.
bool wellFormedLanguage(PrStringView text, bool range) {
    for (size_t at = 0; at <= text.size;) {
        PrStringView subtag = nextSubtag(text, &at);
        if (range && subtag == "*")
            continue;
        if (subtag.size == 0 || subtag.size > kLanguageSubtagMaxLength)
            return false;
        for (size_t i = 0; i < subtag.size; ++i) {
            if (!prAsciiIsLetter(subtag.data[i]) && !prAsciiIsDigit(subtag.data[i]))
                return false;
        }
    }
    return true;
}

// Returns true if the extended language range `range` matches the language tag `tag`, ASCII case-insensitively (RFC
// 4647 extended filtering). The empty range matches only the empty tag.
bool matchesLanguage(PrStringView tag, PrStringView range) {
    if (range.size == 0)
        return tag.size == 0;
    if (!wellFormedLanguage(tag, false) || !wellFormedLanguage(range, true))
        return false;

    size_t tagAt = 0;
    size_t rangeAt = 0;
    PrStringView primary = nextSubtag(range, &rangeAt);
    if (!prAsciiEqualsFold(nextSubtag(tag, &tagAt), primary) && primary != "*")
        return false;
    while (rangeAt <= range.size) {
        PrStringView wanted = nextSubtag(range, &rangeAt);
        if (wanted == "*")
            continue;
        for (;;) {
            if (tagAt > tag.size)
                return false;
            PrStringView subtag = nextSubtag(tag, &tagAt);
            if (prAsciiEqualsFold(subtag, wanted))
                break;
            if (subtag.size == 1)
                return false;
        }
    }
    return true;
}

bool matchesLang(CssSelectorMatcher* m, const CssSelectorArgument* argument, const PrDomElement* element) {
    PrStringView language = domElementLanguage(element);
    for (uint32_t i = 0; i < argument->itemCount; ++i) {
        if (matchesLanguage(language, cssSelectorName(m->selectors, argument->items + i)))
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// The directionality pseudo-class (html/#the-directionality)

bool matchesDir(CssSelectorMatcher* m, const CssSelectorArgument* argument, const PrDomElement* element) {
    PrStringView direction = cssSelectorNameLowered(m->selectors, argument->items);
    if (direction == "rtl")
        return domElementRtl(element);
    return direction == "ltr" && !domElementRtl(element);
}

bool matchesHeading(CssSelectorMatcher* m, const CssSelectorArgument* argument, const PrDomElement* element) {
    uint32_t level = domElementHeadingLevel(element);
    for (uint32_t i = 0; i < argument->itemCount && level != 0; ++i) {
        if (m->selectors->numbers[argument->items + i] == int32_t(level))
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Simple and compound selectors

// Records that a match read `flags` of the children of `node` on behalf of `element`, if the matcher records reads.
void noteStructure(CssSelectorMatcher* m, const PrDomNode* PR_NULLABLE node, DomStructureFlags flags,
        const PrDomElement* element) {
    if (m->structure == nullptr || node == nullptr)
        return;
    if (element != m->target)
        flags |= DomStructureFlag_Descendants;
    if (m->structure->count != 0 && m->structure->last().node == node)
        m->structure->last().flags |= flags;
    else
        m->structure->push(CssStructureRead{node, flags});
}

// Returns what a structural pseudo-class reads of an element's siblings.
DomStructureFlags structureOf(CssPseudoClass pseudoClass) {
    switch (pseudoClass) {
        case CssPseudoClass_FirstChild:
            return DomStructureFlag_Next;
        case CssPseudoClass_LastChild:
            return DomStructureFlag_Previous;
        case CssPseudoClass_OnlyChild:
            return DomStructureFlag_Next | DomStructureFlag_Previous;
        case CssPseudoClass_LastOfType:
        case CssPseudoClass_NthLastChildFn:
        case CssPseudoClass_NthLastOfTypeFn:
            return DomStructureFlag_Backward;
        case CssPseudoClass_OnlyOfType:
            return DomStructureFlag_Forward | DomStructureFlag_Backward;
        default:
            return DomStructureFlag_Forward;
    }
}

// Returns true if `element` matches a structural pseudo-class (selectors-4/#structural-pseudos).
bool matchesStructural(CssSelectorMatcher* m, const CssSimpleSelector* simple, const PrDomElement* element) {
    switch (simple->pseudoClass) {
        case CssPseudoClass_FirstChild:
            return previousElement(element) == nullptr;
        case CssPseudoClass_LastChild:
            return nextElement(element) == nullptr;
        case CssPseudoClass_OnlyChild:
            return previousElement(element) == nullptr && nextElement(element) == nullptr;
        case CssPseudoClass_FirstOfType:
            return firstOfType(element);
        case CssPseudoClass_LastOfType:
            return lastOfType(element);
        case CssPseudoClass_OnlyOfType:
            return firstOfType(element) && lastOfType(element);
        case CssPseudoClass_NthChildFn:
            return matchesNthPseudo(m, simple->argument, element, false, false);
        case CssPseudoClass_NthLastChildFn:
            return matchesNthPseudo(m, simple->argument, element, true, false);
        case CssPseudoClass_NthOfTypeFn:
            return matchesNthPseudo(m, simple->argument, element, false, true);
        case CssPseudoClass_NthLastOfTypeFn:
            return matchesNthPseudo(m, simple->argument, element, true, true);
        default:
            return false;
    }
}

bool matchesPseudoClass(CssSelectorMatcher* m, const CssSimpleSelector* simple, const PrDomElement* element) {
    const CssSelectorArgument* argument =
        simple->argument != kCssSelectorNone ? &m->selectors->arguments[simple->argument] : nullptr;
    switch (simple->pseudoClass) {
        case CssPseudoClass_IsFn:
        case CssPseudoClass_WhereFn:
            return matchesList(m, argument->selectors, element);
        case CssPseudoClass_NotFn:
            return !matchesList(m, argument->selectors, element);
        case CssPseudoClass_HasFn:
            return matchesHas(m, argument->selectors, element);
        case CssPseudoClass_Scope:
            return matchesScope(m, element);

        case CssPseudoClass_Root:
            return isRoot(element);
        case CssPseudoClass_Empty:
            noteStructure(m, element, DomStructureFlag_Empty, element);
            return isEmpty(element);
        case CssPseudoClass_FirstChild:
        case CssPseudoClass_LastChild:
        case CssPseudoClass_OnlyChild:
        case CssPseudoClass_FirstOfType:
        case CssPseudoClass_LastOfType:
        case CssPseudoClass_OnlyOfType:
        case CssPseudoClass_NthChildFn:
        case CssPseudoClass_NthLastChildFn:
        case CssPseudoClass_NthOfTypeFn:
        case CssPseudoClass_NthLastOfTypeFn:
            noteStructure(m, element->parent, structureOf(simple->pseudoClass), element);
            return matchesStructural(m, simple, element);

        case CssPseudoClass_LangFn:
            return matchesLang(m, argument, element);
        case CssPseudoClass_DirFn:
            return matchesDir(m, argument, element);
        case CssPseudoClass_StateFn:
            // selectors-5/#state-pseudo: a custom element whose state set holds the identifier.
            return argument != nullptr && domCustomHasState(element, cssSelectorName(m->selectors, argument->items));
        case CssPseudoClass_Defined:
            return domElementDefined(element);
        case CssPseudoClass_Playing:
            return domElementMediaFlag(element, DomMediaFlag_Playing);
        case CssPseudoClass_Paused:
            return domElementMediaFlag(element, DomMediaFlag_Paused);
        case CssPseudoClass_Seeking:
            return domElementMediaFlag(element, DomMediaFlag_Seeking);
        case CssPseudoClass_Buffering:
            return domElementMediaFlag(element, DomMediaFlag_Buffering);
        case CssPseudoClass_Stalled:
            return domElementMediaFlag(element, DomMediaFlag_Stalled);
        case CssPseudoClass_Muted:
            return domElementMediaFlag(element, DomMediaFlag_Muted);
        case CssPseudoClass_VolumeLocked:
            return domElementMediaFlag(element, DomMediaFlag_VolumeLocked);
        case CssPseudoClass_AnyLink:
            return domElementLink(element);
        // A script sees every link as unvisited (selectors-4/#link).
        case CssPseudoClass_Link:
            return domElementLink(element) && (m->purpose == CssMatchPurpose_Script || !domElementVisited(element));
        case CssPseudoClass_Visited:
            return m->purpose == CssMatchPurpose_Style && domElementVisited(element);
        case CssPseudoClass_Autofill:
            return domElementAutofilled(element);
        case CssPseudoClass_Enabled:
            return domElementEnablement(element) == DomEnablement_Enabled;
        case CssPseudoClass_Disabled:
            return domElementEnablement(element) == DomEnablement_Disabled;
        case CssPseudoClass_Checked:
            return (domElementCheckedness(element) & DomCheckedFlag_Checked) != 0;
        case CssPseudoClass_Unchecked:
            return (domElementCheckedness(element) & DomCheckedFlag_Unchecked) != 0;
        case CssPseudoClass_Indeterminate:
            return (domElementCheckedness(element) & DomCheckedFlag_Indeterminate) != 0;
        case CssPseudoClass_Default:
            return domElementDefault(element);
        case CssPseudoClass_Required:
            return domElementRequirement(element) == DomRequirement_Required;
        case CssPseudoClass_Optional:
            return domElementRequirement(element) == DomRequirement_Optional;
        case CssPseudoClass_ReadWrite:
            return element->space == PrDomNamespace_Html && domElementReadWrite(element);
        case CssPseudoClass_ReadOnly:
            return element->space == PrDomNamespace_Html && !domElementReadWrite(element);
        case CssPseudoClass_PlaceholderShown:
            return domElementPlaceholderShown(element);
        case CssPseudoClass_Open:
            return (domIsHtml(element, PrDomTag_Details) || domIsHtml(element, PrDomTag_Dialog)) &&
                domHasAttribute(element, "open");
        // html/#selector-modal matches a modal dialog. Nothing here puts an element in fullscreen.
        case CssPseudoClass_Modal:
            return domIsHtml(element, PrDomTag_Dialog) && element->state != nullptr && element->state->modal;
        case CssPseudoClass_HasSlotted:
            return domIsSlot(element) && domSlotHasSlotted(element);
        // css-shadow-1/#host-selector
        case CssPseudoClass_Host:
            return element == m->host && m->host != nullptr;
        case CssPseudoClass_HostFn:
        case CssPseudoClass_HostContextFn: {
            if (element != m->host || m->host == nullptr || argument == nullptr)
                return false;
            bool within = m->hostArgument;
            m->hostArgument = true;
            bool matches = matchesList(m, argument->selectors, element);
            // :host-context() also matches through the host's shadow-including ancestors.
            for (const PrDomNode* at = domShadowIncludingParent(element);
                    !matches && at != nullptr && simple->pseudoClass == CssPseudoClass_HostContextFn;
                    at = domShadowIncludingParent(at)) {
                if (const PrDomElement* ancestor = domElementFromNode(at))
                    matches = matchesList(m, argument->selectors, ancestor);
            }
            m->hostArgument = within;
            return matches;
        }
        case CssPseudoClass_Focus:
            return domElementFocused(element);
        case CssPseudoClass_FocusVisible:
            return domElementFocusVisible(element);
        case CssPseudoClass_FocusWithin:
            return domElementFocusWithin(element);
        case CssPseudoClass_Hover:
            return domElementHovered(element);
        case CssPseudoClass_Target:
            return element->document->target == element;
        case CssPseudoClass_Active:
            return domElementActive(element);
        case CssPseudoClass_Heading:
            return domElementHeadingLevel(element) != 0;
        case CssPseudoClass_HeadingFn:
            return matchesHeading(m, argument, element);
        // The engine's private pseudo-classes, which state the conditions of html/#rendering.
        case CssPseudoClass_PrListBox:
            return domIsHtml(element, PrDomTag_Select) && domSelectListBox(element);

        default:
            return false;
    }
}

bool matchesSimple(CssSelectorMatcher* m, const CssSimpleSelector* simple, const PrDomElement* element) {
    switch (simple->kind) {
        case CssSimpleKind_Type:
            return matchesType(m, simple, element);
        case CssSimpleKind_Universal:
            // The default namespace does not keep a featureless host from matching (selectors-4/#featureless).
            if (element == m->host && !m->hostArgument && simple->prefix == CssNamespacePrefix_None)
                return true;
            return inNamespace(m, simple, element, true);
        case CssSimpleKind_Id:
            return (element->flags & DomNodeFlag_Id) != 0 &&
                matchesId(m, domElementId(element), cssSelectorName(m->selectors, simple->name));
        case CssSimpleKind_Class:
            return matchesClass(m, simple, element);
        case CssSimpleKind_Attribute:
            return matchesAttribute(m, simple, element);
        case CssSimpleKind_PseudoClass:
            return matchesPseudoClass(m, simple, element);
        case CssSimpleKind_Nesting:
            return matchesScope(m, element);
        case CssSimpleKind_PseudoElement:
            return m->pseudoElements;
        default:
            return false;
    }
}

// Returns true if `element` matches the compound's simple selectors from `start` to `end`.
bool matchesSimples(CssSelectorMatcher* m, const CssCompoundSelector* compound, uint32_t start, uint32_t end,
        const PrDomElement* element) {
    for (uint32_t i = start; i < end; ++i) {
        if (!matchesSimple(m, &m->selectors->simple[compound->simple + i], element))
            return false;
    }
    return true;
}

bool matchesCompound(CssSelectorMatcher* m, const CssCompoundSelector* compound, const PrDomElement* element) {
    return matchesSimples(m, compound, 0, compound->simpleCount, element);
}

// ---------------------------------------------------------------------------------------------------------------------
// Complex selectors

// Returns true if an element of the tree matches the compounds from `index`, and the element's attribute that the
// combinator names holds the ID of `element` (selectors-5/#idref-combinators).
Match matchReference(CssSelectorMatcher* m, const CssComplexSelector* complex, uint32_t index,
        const CssCompoundSelector* compound, const PrDomElement* element, const PrDomElement* PR_NULLABLE anchor) {
    PrStringView id = domElementId(element);
    if (id.size == 0)
        return Match_Fails;
    const CssSimpleSelector* reference = &m->selectors->simple[compound->reference];
    const PrDomNode* root = domNodeRoot(element);
    for (const PrDomNode* node = root; node != nullptr; node = domNodeNext(root, node)) {
        const PrDomElement* referrer = domElementFromNode(node);
        if (referrer == nullptr)
            continue;
        for (const PrDomAttr* attribute = nextNamed(m, reference, referrer, referrer->firstAttribute);
                attribute != nullptr; attribute =
                nextNamed(m, reference, referrer, static_cast<const PrDomAttr*>(attribute->next))) {
            PrStringView value = prRawStringView(&attribute->value);
            if (matchesId(m, value, id) && matchFrom(m, complex, index, referrer, anchor) == Match_Yes)
                return Match_Yes;
        }
    }
    return Match_Fails;
}

// Matches a descendant combinator from `element`. Matches the compounds of `complex` from `index` leftward against the
// ancestors of `element`, nearest first. The walk's result from an ancestor is also its result from each ancestor that
// it passed on the way. A walk that follows another walk from the same parent returns the same result. A walk that
// passes more than kCssShallowWalk ancestors keeps its result for the ancestors beyond them. So the walks from the
// elements under one ancestor pass that ancestor once.
Match matchAncestors(CssSelectorMatcher* m, const CssComplexSelector* complex, uint32_t index, const PrDomElement* element) {
    uint32_t key = ((complex->compound + index) << kMemoShift) | kMemoAncestors;
    const PrDomElement* start = parentInTree(m, element);
    CssAncestorWalk* last = nullptr;
    for (uint32_t i = 0; i < m->walks.count && last == nullptr; ++i)
        last = m->walks.data[i].key == key ? &m->walks.data[i] : nullptr;
    if (last != nullptr && last->start == start)
        return Match(last->result);

    uint32_t passed = m->walk.count;
    uint32_t steps = 0;
    Match result = Match_FailsAll;
    // The walk in a document's tree reads no host, so that it costs what a walk of the parents does.
    auto walk = [&](auto parentOf, auto matchAt) PR_FORCE_INLINE_LAMBDA {
        for (const PrDomElement* at = start; at != nullptr; at = parentOf(at), ++steps) {
            if (steps >= kCssShallowWalk) {
                const CssMatchMemo* memo = findMemo(m, at, key);
                if (memo != nullptr) {
                    result = Match(memo->value);
                    break;
                }
                m->walk.push(at);
            }
            Match match = matchAt(at);
            if (match == Match_Yes || match == Match_FailsAll) {
                result = match;
                break;
            }
        }
    };
    if (m->host == nullptr) {
        walk([](const PrDomElement* at) { return domNodeParentElement(at); },
            [&](const PrDomElement* at) { return matchFrom(m, complex, index, at, nullptr); });
    } else {
        walk([&](const PrDomElement* at) { return parentInTree(m, at); },
            [&](const PrDomElement* at) { return matchMaybeHost(m, complex, index, at, nullptr); });
    }
    for (uint32_t i = passed; i < m->walk.count; ++i)
        addMemo(m, m->walk.data[i], key, result);
    m->walk.count = passed;

    // The walks nested in this one may have reallocated the array.
    last = nullptr;
    for (uint32_t i = 0; i < m->walks.count && last == nullptr; ++i)
        last = m->walks.data[i].key == key ? &m->walks.data[i] : nullptr;
    if (last == nullptr) {
        m->walks.push(CssAncestorWalk{key, start, result});
        return result;
    }
    last->start = start;
    last->result = result;
    return result;
}

// Records that a match read the siblings before `element` through the sibling combinator of the compound at `index`,
// and that the read gave `match`. Nothing is recorded if the compounds beyond the siblings fail. Those compounds relate
// to the siblings' parent and its ancestors, so no change among the siblings makes the selector match.
void noteSiblings(CssSelectorMatcher* m, const CssComplexSelector* complex, uint32_t index, const PrDomElement* element,
        const PrDomElement* PR_NULLABLE anchor, Match match) {
    if (m->structure == nullptr || element->parent == nullptr)
        return;
    // The compound whose combinator leads off the siblings.
    uint32_t last = index + 1;
    while (last + 1 < complex->compoundCount) {
        CssCombinator combinator = m->selectors->compounds[complex->compound + last].combinator;
        if (combinator != CssCombinator_NextSibling && combinator != CssCombinator_SubsequentSibling)
            break;
        ++last;
    }
    if (match != Match_Yes && anchor == nullptr) {
        switch (m->selectors->compounds[complex->compound + last].combinator) {
            case CssCombinator_Child: {
                const PrDomElement* parent = domNodeParentElement(element);
                if (parent == nullptr || matchFrom(m, complex, last + 1, parent, nullptr) != Match_Yes)
                    return;
                break;
            }
            case CssCombinator_Descendant:
                if (matchAncestors(m, complex, last + 1, element) != Match_Yes)
                    return;
                break;
            default:
                break;
        }
    }
    // A lone `+` reads only the element immediately before.
    const CssCompoundSelector* compounds = m->selectors->compounds + complex->compound;
    bool lone = last == index + 1 && compounds[index].combinator == CssCombinator_NextSibling &&
        (index == 0 || (compounds[index - 1].combinator != CssCombinator_NextSibling &&
            compounds[index - 1].combinator != CssCombinator_SubsequentSibling));
    noteStructure(m, element->parent, lone ? DomStructureFlag_Next : DomStructureFlag_Forward, element);
}

// Matches the compounds of `complex` after the compound at `index`, which `element` matches, through that compound's
// combinator.
Match matchCombinator(CssSelectorMatcher* m, const CssComplexSelector* complex, uint32_t index,
        const CssCompoundSelector* compound, const PrDomElement* element, const PrDomElement* PR_NULLABLE anchor) {
    CssCombinator combinator = compound->combinator;
    if (combinator == CssCombinator_None) {
        if (anchor == nullptr)
            return Match_Yes;
        combinator = CssCombinator_Descendant;
    }

    uint32_t next = index + 1;
    switch (combinator) {
        case CssCombinator_Descendant:
            if (anchor == nullptr)
                return matchAncestors(m, complex, next, element);
            for (const PrDomElement* at = parentInTree(m, element); at != nullptr; at = parentInTree(m, at)) {
                Match match = matchMaybeHost(m, complex, next, at, anchor);
                if (match == Match_Yes || match == Match_FailsAll)
                    return match;
            }
            return Match_FailsAll;
        case CssCombinator_Child: {
            if (m->host == nullptr) {
                const PrDomElement* parent = domNodeParentElement(element);
                return parent != nullptr ? matchFrom(m, complex, next, parent, anchor) : Match_FailsAll;
            }
            const PrDomElement* parent = parentInTree(m, element);
            return parent != nullptr ? matchMaybeHost(m, complex, next, parent, anchor) : Match_FailsAll;
        }
        case CssCombinator_NextSibling: {
            const PrDomElement* sibling = previousElement(element);
            Match match = sibling != nullptr ? matchFrom(m, complex, next, sibling, anchor) : Match_FailsSiblings;
            noteSiblings(m, complex, index, element, anchor, match);
            return match;
        }
        case CssCombinator_SubsequentSibling: {
            Match match = Match_FailsSiblings;
            for (const PrDomElement* at = previousElement(element); at != nullptr; at = previousElement(at)) {
                Match found = matchFrom(m, complex, next, at, anchor);
                if (found != Match_Fails) {
                    match = found;
                    break;
                }
            }
            noteSiblings(m, complex, index, element, anchor, match);
            return match;
        }
        case CssCombinator_Reference:
            return matchReference(m, complex, next, compound, element, anchor);
        default:
            return Match_FailsAll;
    }
}

// Matches the compounds of `complex` from `index` leftward, testing the compound at `index` against `element`. `anchor`
// is the element of the :has() whose argument is `complex`, and the leftmost compound relates to it. Beyond the
// leftmost compound of a relative selector without an anchor is the `&` or :scope that the selector is relative to.
Match matchFrom(CssSelectorMatcher* m, const CssComplexSelector* complex, uint32_t index,
        const PrDomElement* element, const PrDomElement* PR_NULLABLE anchor) {
    if (index == complex->compoundCount)
        return (anchor != nullptr ? element == anchor : matchesScope(m, element)) ? Match_Yes : Match_Fails;

    const CssCompoundSelector* compound = &m->selectors->compounds[complex->compound + index];
    uint32_t placedEnd = compound->placed + compound->placedCount;
    if (!matchesSimples(m, compound, 0, compound->placed, element) ||
            !matchesSimples(m, compound, placedEnd, compound->simpleCount, element))
        return Match_Fails;
    // The selectors that read structure are tested after the rest match, so a match that the rest decide reads no
    // structure.
    Match match = matchCombinator(m, complex, index, compound, element, anchor);
    if (match == Match_Yes && !matchesSimples(m, compound, compound->placed, placedEnd, element))
        return Match_Fails;
    return match;
}

// Matches as matchFrom does, for an element that may be the matcher's host, which is featureless: only a compound of
// :host, :host() or :host-context() matches it.
Match matchMaybeHost(CssSelectorMatcher* m, const CssComplexSelector* complex, uint32_t index,
        const PrDomElement* element, const PrDomElement* PR_NULLABLE anchor) {
    if (element == m->host && !m->hostArgument && index < complex->compoundCount &&
            !matchesFeatureless(m->selectors, &m->selectors->compounds[complex->compound + index]))
        return Match_Fails;
    return matchFrom(m, complex, index, element, anchor);
}

bool matchesList(CssSelectorMatcher* m, CssSelectorList list, const PrDomElement* element) {
    for (uint32_t i = 0; i < list.count; ++i) {
        const CssComplexSelector* complex = &m->selectors->complex[list.first + i];
        if ((complex->flags & CssComplexFlag_Invalid) == 0 && matchFrom(m, complex, 0, element, nullptr) == Match_Yes)
            return true;
    }
    return false;
}

// Returns the compound that is the subject of the list's single selector. Returns null if the list has more than one
// selector or its one selector is invalid.
const CssCompoundSelector* PR_NULLABLE subjectOf(const PrCssSelectors* selectors) {
    if (selectors->list.count != 1)
        return nullptr;
    const CssComplexSelector* complex = &selectors->complex[selectors->list.first];
    if ((complex->flags & CssComplexFlag_Invalid) != 0)
        return nullptr;
    return &selectors->compounds[complex->compound];
}

// Returns the first simple selector of `kind` in the compound, or null if it has none.
const CssSimpleSelector* PR_NULLABLE simpleOf(const PrCssSelectors* selectors,
        const CssCompoundSelector* PR_NULLABLE compound, CssSimpleKind kind) {
    for (uint32_t i = 0; compound != nullptr && i < compound->simpleCount; ++i) {
        const CssSimpleSelector* simple = &selectors->simple[compound->simple + i];
        if (simple->kind == kind)
            return simple;
    }
    return nullptr;
}

// Returns true if `node` is a descendant of `ancestor`.
bool isDescendant(const PrDomNode* node, const PrDomNode* ancestor) {
    for (const PrDomNode* at = node->parent; at != nullptr; at = at->parent) {
        if (at == ancestor)
            return true;
    }
    return false;
}

// Computes the filter of the selector at `index`. Returns false if the selector matches no element of the document,
// because it names a class that no element has.
bool subjectFilter(CssSelectorMatcher* m, uint32_t index, CssSubjectFilter* out) {
    const PrCssSelectors* selectors = m->selectors;
    const CssComplexSelector* complex = &selectors->complex[index];
    const CssCompoundSelector* subject = &selectors->compounds[complex->compound];
    const CssCompoundSelector* named = m->quirks ? nullptr : subject;
    const CssSimpleSelector* id = simpleOf(selectors, named, CssSimpleKind_Id);
    const CssSimpleSelector* name = simpleOf(selectors, named, CssSimpleKind_Class);
    const CssSimpleSelector* type = simpleOf(selectors, subject, CssSimpleKind_Type);
    if (type != nullptr && (type->space != kCssSelectorNone ||
            (type->prefix != CssNamespacePrefix_None && type->prefix != CssNamespacePrefix_Any)))
        type = nullptr;

    out->complex = index;
    out->id = id != nullptr ? cssSelectorName(selectors, id->name) : PrStringView{};
    out->atom = name != nullptr ? classAtom(m, name->name) : kAtomNone;
    if (name != nullptr && out->atom == kAtomNone)
        return false;
    if (type == nullptr)
        out->tag = kCssAnyTag;
    else if (type->tag != PrDomTag_Unknown)
        out->tag = DomTagId(type->tag);
    else
        out->tag = localNameTag(m, type->name);
    out->whole = complex->compoundCount == 1;
    for (uint32_t i = 0; out->whole && i < subject->simpleCount; ++i) {
        const CssSimpleSelector* simple = &selectors->simple[subject->simple + i];
        bool universal = simple->kind == CssSimpleKind_Universal && simple->space == kCssSelectorNone &&
            (simple->prefix == CssNamespacePrefix_None || simple->prefix == CssNamespacePrefix_Any);
        out->whole = simple == id || simple == name || simple == type || universal;
    }
    return true;
}

// Returns true if `element` matches the selector of `filter`. If it can, the filter answers without reading the
// element.
bool passes(CssSelectorMatcher* m, const CssSubjectFilter* filter, const PrDomElement* element) {
    if (filter->id.data != nullptr && ((element->flags & DomNodeFlag_Id) == 0 || domElementId(element) != filter->id))
        return false;
    if (filter->atom != kAtomNone && (element->classes == kDomClassesNone ||
            !domClassesHas(m->document, element->classes, filter->atom)))
        return false;
    bool html = domIsHtmlInHtmlDocument(element);
    if (filter->tag != kCssAnyTag) {
        if (html && element->tag != filter->tag)
            return false;
        if (filter->whole && html)
            return true;
    } else if (filter->whole) {
        return true;
    }
    const CssComplexSelector* complex = &m->selectors->complex[filter->complex];
    if (complex->compoundCount == 1)
        return matchesCompound(m, &m->selectors->compounds[complex->compound], element);
    return matchFrom(m, complex, 0, element, nullptr) == Match_Yes;
}

} // namespace

PrCssSelectors* PR_NULLABLE cssParseDomSelectors(PrStringView text) {
    CssParser parser;
    cssParserInit(&parser);
    CssSelectorContext context = {};
    PrCssSelectors* selectors = cssParseSelectorText(&parser, text, &context);
    cssParserReset(&parser);
    return selectors;
}

const PrCssSelectors* PR_NULLABLE cssDomSelectors(const PrDomDocument* document, PrStringView text) {
    constexpr uint64_t kSeed = 0x94d049bb133111ebull;
    CssSelectorCache*& cache = document->selectors;
    if (cache == nullptr)
        cache = allocZeroed<CssSelectorCache>();
    uint32_t hash = uint32_t(prHashBytes(text.data, text.size, kSeed));
    uint32_t at = cache->index.find(hash, [&](uint32_t i) {
        return prRawStringView(&cache->entries.data[i].text) == text;
    });
    if (at != kHashMissing) {
        cache->entries.data[at].used = ++cache->uses;
        return cache->entries.data[at].selectors;
    }

    PrCssSelectors* selectors = cssParseDomSelectors(text);
    if (selectors == nullptr)
        return nullptr;
    CssCachedSelectors entry = {hash, cache->uses + 1, prRawStringCopy(text), selectors};
    ++cache->uses;
    if (cache->entries.count < kCssSelectorCacheSize) {
        cache->index.insert(hash, cache->entries.count);
        cache->entries.push(entry);
        return selectors;
    }
    // The least recently used entry makes room.
    uint32_t oldest = 0;
    for (uint32_t i = 1; i < cache->entries.count; ++i) {
        if (cache->entries.data[i].used < cache->entries.data[oldest].used)
            oldest = i;
    }
    CssCachedSelectors* evicted = &cache->entries.data[oldest];
    cache->index.erase(evicted->hash, oldest);
    prRawStringReset(&evicted->text);
    cssSelectorsDestroy(evicted->selectors);
    *evicted = entry;
    cache->index.insert(hash, oldest);
    return selectors;
}

void cssSelectorCacheDestroy(CssSelectorCache* PR_NULLABLE cache) {
    if (cache == nullptr)
        return;
    for (uint32_t i = 0; i < cache->entries.count; ++i) {
        prRawStringReset(&cache->entries.data[i].text);
        cssSelectorsDestroy(cache->entries.data[i].selectors);
    }
    cache->entries.reset();
    cache->index.reset();
    prMemoryFree(cache);
}

void cssSelectorMatcherInit(CssSelectorMatcher* matcher, const PrCssSelectors* selectors,
        const PrDomElement* PR_NULLABLE scope, const PrDomDocument* document, const PrDomNode* PR_NULLABLE tree) {
    matcher->selectors = selectors;
    matcher->purpose = CssMatchPurpose_Script;
    matcher->document = document;
    matcher->scope = scope;
    matcher->readsHost = selectors->readsHost;
    bool shadows = tree != nullptr && matcher->readsHost && document->slots != nullptr;
    const PrDomShadowRoot* shadow = shadows ? domNodeShadowRoot(domNodeRoot(tree)) : nullptr;
    matcher->host = shadow != nullptr ? shadow->host : nullptr;
    matcher->hostArgument = false;
    matcher->quirks = document->quirks == DomQuirks_Yes;
    matcher->memos = {};
    matcher->walk = {};
    matcher->walks = {};
    matcher->runs = {};
    matcher->classes = {};
    matcher->localNames = {};
    matcher->structure = nullptr;
    matcher->target = nullptr;
    matcher->pseudoElements = false;

    matcher->subjects = {};
    for (uint32_t i = 0; i < selectors->list.count; ++i) {
        const CssComplexSelector* complex = &selectors->complex[selectors->list.first + i];
        if ((complex->flags & CssComplexFlag_Invalid) != 0)
            continue;
        CssSubjectFilter filter = {};
        if (!subjectFilter(matcher, selectors->list.first + i, &filter))
            continue;
        matcher->subjects.push(filter);
    }

    // The nearest compound left of the subject that has an ID, across child and descendant combinators.
    matcher->scopeId = PrStringView{};
    const CssComplexSelector* complex = subjectOf(selectors) != nullptr ? &selectors->complex[selectors->list.first] : nullptr;
    for (uint32_t i = 1; !matcher->quirks && complex != nullptr && i < complex->compoundCount; ++i) {
        CssCombinator combinator = selectors->compounds[complex->compound + i - 1].combinator;
        if (combinator != CssCombinator_Child && combinator != CssCombinator_Descendant)
            break;
        const CssSimpleSelector* left =
            simpleOf(selectors, &selectors->compounds[complex->compound + i], CssSimpleKind_Id);
        if (left != nullptr) {
            matcher->scopeId = cssSelectorName(selectors, left->name);
            break;
        }
    }
}

void cssSelectorMatcherReset(CssSelectorMatcher* matcher) {
    prMemoryFree(matcher->memos.slots);
    matcher->memos = {};
    matcher->walk.reset();
    matcher->walks.reset();
    matcher->runs.reset();
    matcher->subjects.reset();
    matcher->classes.reset();
    matcher->localNames.reset();
}

void cssSelectorMatcherForget(CssSelectorMatcher* matcher) {
    prMemoryFree(matcher->memos.slots);
    matcher->memos = {};
    matcher->walk.clear();
    matcher->walks.clear();
    matcher->runs.clear();
    // The names that the matcher compared stand for the atoms that the document had, and changes add atoms.
    matcher->classes.clear();
    matcher->localNames.clear();
}

bool cssSelectorMatcherMatches(CssSelectorMatcher* matcher, const PrDomElement* element) {
    matcher->target = element;
    bool matches = matchesList(matcher, matcher->selectors->list, element);
    matcher->target = nullptr;
    return matches;
}

bool cssSelectorMatcherMatchesComplex(CssSelectorMatcher* matcher, uint32_t complex, const PrDomElement* element) {
    const CssComplexSelector* selector = &matcher->selectors->complex[complex];
    matcher->target = element;
    bool matches = (selector->flags & CssComplexFlag_Invalid) == 0 &&
        matchMaybeHost(matcher, selector, 0, element, nullptr) == Match_Yes;
    matcher->target = nullptr;
    return matches;
}

bool cssSelectorMatcherMatchesSlotted(CssSelectorMatcher* matcher, uint32_t complex, const PrDomElement* element,
        const PrDomElement* slot) {
    const PrCssSelectors* selectors = matcher->selectors;
    const CssComplexSelector* selector = &selectors->complex[complex];
    if ((selector->flags & CssComplexFlag_Invalid) != 0)
        return false;
    // The pseudo-element ends the subject compound, and its argument is a compound that the slotted element matches,
    // outside the matcher's tree.
    const CssCompoundSelector* subject = &selectors->compounds[selector->compound];
    const CssSimpleSelector* slotted = &selectors->simple[subject->simple + subject->simpleCount - 1];
    if (slotted->kind != CssSimpleKind_PseudoElement || slotted->pseudoElement != CssPseudoElement_SlottedFn ||
            slotted->argument == kCssSelectorNone)
        return false;
    const PrDomElement* host = matcher->host;
    matcher->host = nullptr;
    matcher->target = element;
    bool matches = matchesList(matcher, selectors->arguments[slotted->argument].selectors, element);
    matcher->host = host;
    if (matches) {
        bool pseudoElements = matcher->pseudoElements;
        matcher->pseudoElements = true;
        matcher->target = slot;
        matches = matchFrom(matcher, selector, 0, slot, nullptr) == Match_Yes;
        matcher->pseudoElements = pseudoElements;
    }
    matcher->target = nullptr;
    return matches;
}

PrDomElement* PR_NULLABLE cssSelectorMatcherNext(CssSelectorMatcher* matcher, PrDomNode* root, PrDomNode* after) {
    const CssSubjectFilter* subjects = matcher->subjects.data;
    uint32_t count = matcher->subjects.count;
    if (count == 0)
        return nullptr;
    // If the tree keeps the one element with the subject ID of the single selector, that element is the only one to
    // test.
    PrStringView subjectId = count == 1 && matcher->selectors->list.count == 1 ? subjects[0].id : PrStringView{};
    PrDomNode* tree = subjectId.data != nullptr || matcher->scopeId.data != nullptr ? domNodeRoot(root) : nullptr;
    bool ids = tree != nullptr && domKeepsIds(tree);
    if (ids && subjectId.data != nullptr) {
        PrDomElement* first = nullptr;
        uint32_t found = domElementsById(tree, subjectId, &first);
        if (found == 0 || (found == 1 && after == first))
            return nullptr;
        if (found == 1 && after == root)
            return isDescendant(first, root) && passes(matcher, subjects, first) ? first : nullptr;
    }
    // If the tree keeps the one element with the ID that a compound left of the subject names, the subject is among its
    // descendants.
    PrDomNode* within = root;
    if (ids && matcher->scopeId.data != nullptr) {
        PrDomElement* scope = nullptr;
        uint32_t found = domElementsById(tree, matcher->scopeId, &scope);
        if (found == 0)
            return nullptr;
        if (found == 1 && scope != root && isDescendant(scope, root)) {
            within = scope;
            if (after == root)
                after = scope;
        } else if (found == 1 && scope != root && !isDescendant(root, scope)) {
            return nullptr;
        }
    }
    for (PrDomNode* node = domNodeNext(within, after); node != nullptr; node = domNodeNext(within, node)) {
        PrDomElement* element = domElementFromNode(node);
        if (element == nullptr)
            continue;
        for (uint32_t i = 0; i < count; ++i) {
            if (passes(matcher, &subjects[i], element))
                return element;
        }
    }
    return nullptr;
}

bool cssSelectorsMatch(const PrCssSelectors* selectors, const PrDomElement* element, const PrDomElement* PR_NULLABLE scope) {
    CssSelectorMatcher matcher;
    cssSelectorMatcherInit(&matcher, selectors, scope, element->document, element);
    bool matches = cssSelectorMatcherMatches(&matcher, element);
    cssSelectorMatcherReset(&matcher);
    return matches;
}

} // namespace pr_impl
