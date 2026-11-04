#include "css/src/css_selector.h"

#include "pr/float.h"
#include "pr/integer.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/name_table.h"
#include "pr/string_ops.h"

#include "css/src/css_keyword.h"
#include "css/src/css_parser.h"
#include "css/src/css_rule.h"
#include "css/src/css_serialize.h"
#include "dom/src/dom_attribute.h"
#include "dom/src/dom_node.h"

namespace pr_impl {
namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Names

// Adds a name, as written and in ASCII lowercase, and returns its index.
uint32_t addName(CssSelectorPool* pool, PrStringView text) {
    CssSelectorName name = {};
    name.size = uint32_t(text.size);
    name.hash = domAttributeHash(text);
    name.text = pool->text.size;
    prTextBufferAppend(&pool->text, text);

    bool lower = true;
    for (size_t i = 0; i < text.size; ++i)
        lower = lower && prAsciiLower(text.data[i]) == text.data[i];
    name.lowered = name.text;
    if (!lower) {
        name.lowered = pool->text.size;
        prTextBufferAppend(&pool->text, text);
        prAsciiLowerText(pool->text.data + name.lowered, text.size);
    }
    pool->names.push(name);
    return pool->names.count - 1;
}

// Returns the ASCII lowercase form of a name that the pool holds.
PrStringView nameLowered(const CssSelectorPool* pool, uint32_t name) {
    const CssSelectorName* entry = &pool->names.data[name];
    return PrStringView{pool->text.data + entry->lowered, entry->size};
}

// ---------------------------------------------------------------------------------------------------------------------
// Compiling
//
// A selector list parses to a tree of structural productions (css_parse.h), which is compiled here to the pool's
// records. Every run of records that a list, a complex selector or a compound selector owns is reserved before the
// records under it are created. The reservation is needed because an argument that is a selector list adds records of
// its own while its pseudo-class is being filled in.

struct Compiler {
    const CssValuePool* tree;
    const CssSelectorContext* context;
    CssSelectorPool* pool;
    bool nesting;       // true if the complex selector being compiled holds an `&`

    // True while the selectors being compiled are the argument of :is(), :where() or :not(), whose subjects the default
    // namespace does not restrict (selectors-4/#matches).
    bool matchesAny;
};

const CssValueNode* nodeAt(const Compiler* c, uint32_t index) {
    return &c->tree->values.data[index];
}

// Returns the index of the node after `index` and everything under it.
uint32_t after(const Compiler* c, uint32_t index) {
    return index + nodeAt(c, index)->next;
}

void compileComplex(Compiler* c, uint32_t node, uint32_t slot, bool top);

// Compiles the selector list that the nodes in [first, end) form. Each node is one complex selector, with a ',' between
// each two. A list production keeps no node of its own. `top` means the list being parsed rather than an argument. A
// nested rule's selectors are relative only in that list.
CssSelectorList compileList(Compiler* c, uint32_t first, uint32_t end, bool top) {
    CssSelectorList list = {c->pool->complex.count, 0};
    for (uint32_t i = first; i < end; i = after(c, i))
        list.count += cssValueIsLiteral(nodeAt(c, i), ',') ? 0 : 1;
    for (uint32_t i = 0; i < list.count; ++i)
        c->pool->complex.push(CssComplexSelector{});

    uint32_t slot = list.first;
    for (uint32_t i = first; i < end; i = after(c, i)) {
        if (!cssValueIsLiteral(nodeAt(c, i), ','))
            compileComplex(c, i, slot++, top);
    }
    return list;
}

bool listNests(const CssSelectorPool* pool, CssSelectorList list) {
    for (uint32_t i = 0; i < list.count; ++i) {
        if ((pool->complex.data[list.first + i].flags & CssComplexFlag_Nesting) != 0)
            return true;
    }
    return false;
}

// Returns the namespace that the specs name with `url`, or PrDomNamespace_Other.
PrDomNamespace namedNamespace(PrStringView url) {
    for (PrDomNamespace space = PrDomNamespace_Html; space < PrDomNamespace_Other; ++space) {
        if (url == domNamedNamespaceUrl(space))
            return space;
    }
    return PrDomNamespace_Other;
}

// Records the namespace `url` that a prefix names. `prefix` is the null view for the default namespace.
void addNamespace(Compiler* c, PrStringView prefix, PrStringView url, CssSimpleSelector* out) {
    CssSelectorNamespace space = {};
    space.prefix = prefix.data == nullptr ? kCssSelectorNone : addName(c->pool, prefix);
    space.url = addName(c->pool, url);
    space.named = namedNamespace(url);
    out->space = c->pool->namespaces.count;
    c->pool->namespaces.push(space);
}

// Compiles the namespace that a prefix names. `prefix` is the NsPrefix node, or zero if none is written. If none is
// written, an element's name is in the sheet's default namespace. A prefix that means the same as no prefix is stored
// as none. Such prefixes are `*|` for an element if the sheet declares no default namespace, a prefix that names the
// default namespace for an element, and `|` for an attribute (selectors-4/#type-nmsp, selectors-4/#attrnmsp).
void compileNamespace(Compiler* c, uint32_t prefix, bool element, CssSimpleSelector* out) {
    out->space = kCssSelectorNone;
    out->prefix = CssNamespacePrefix_None;
    const CssSheetData* sheet = c->context->sheet;
    PrStringView none = {nullptr, 0};
    PrStringView fallback = none;
    bool defaulted = element && sheet != nullptr && cssSheetNamespace(sheet, none, &fallback);
    if (prefix != 0) {
        const CssValueNode* first = nodeAt(c, prefix + 1);
        if (cssValueIsLiteral(first, '*')) {
            out->prefix = element && !defaulted ? CssNamespacePrefix_None : CssNamespacePrefix_Any;
            return;
        }
        if (first->kind != PrCssValueKind_Ident) {
            out->prefix = element ? CssNamespacePrefix_Empty : CssNamespacePrefix_None;
            return;
        }
        // checkNamespacePrefix has already verified that the prefix is declared.
        PrStringView name = cssValuePoolText(c->tree, first);
        PrStringView url = none;
        cssSheetNamespace(sheet, name, &url);
        if (!defaulted || url != fallback) {
            out->prefix = CssNamespacePrefix_Named;
            addNamespace(c, name, url, out);
            return;
        }
    }
    if (defaulted)
        addNamespace(c, none, fallback, out);
}

// Compiles the namespace and the name of a <wq-name>.
void compileQualifiedName(Compiler* c, uint32_t node, bool element, CssSimpleSelector* out) {
    uint32_t prefix = 0;
    for (uint32_t i = node + 1; i < after(c, node); i = after(c, i)) {
        if (cssValueIsProduction(nodeAt(c, i), CssValueDef_NsPrefix))
            prefix = i;
        else if (nodeAt(c, i)->kind == PrCssValueKind_Ident)
            out->name = addName(c->pool, cssValuePoolText(c->tree, nodeAt(c, i)));
    }
    compileNamespace(c, prefix, element, out);
}

bool isNth(CssPseudoClass pseudo) {
    switch (pseudo) {
        case CssPseudoClass_NthChildFn:
        case CssPseudoClass_NthLastChildFn:
        case CssPseudoClass_NthOfTypeFn:
        case CssPseudoClass_NthLastOfTypeFn:
        case CssPseudoClass_NthColFn:
        case CssPseudoClass_NthLastColFn:
            return true;
        default:
            return false;
    }
}

// The named view transition pseudo-elements (css-view-transitions-2/#named-view-transition-pseudo).
bool isNamedTransition(CssPseudoElement pseudo) {
    switch (pseudo) {
        case CssPseudoElement_ViewTransitionGroupFn:
        case CssPseudoElement_ViewTransitionGroupChildrenFn:
        case CssPseudoElement_ViewTransitionImagePairFn:
        case CssPseudoElement_ViewTransitionNewFn:
        case CssPseudoElement_ViewTransitionOldFn:
            return true;
        default:
            return false;
    }
}

// An argument with no items and no name.
CssSelectorArgument emptyArgument() {
    CssSelectorArgument argument = {};
    argument.items = kCssSelectorNone;
    argument.name = kCssSelectorNone;
    return argument;
}

// A simple selector that names nothing and has no kind yet.
CssSimpleSelector emptySimple() {
    CssSimpleSelector simple = {};
    simple.space = kCssSelectorNone;
    simple.name = kCssSelectorNone;
    simple.value = kCssSelectorNone;
    simple.argument = kCssSelectorNone;
    simple.pseudoClass = CssPseudoClass_None;
    return simple;
}

// Compiles the argument between the parentheses after a pseudo's name, in [first, end). The argument is a selector
// list, an An+B with the list after its `of`, or the identifiers, strings and integers that it is written with.
uint32_t compileArgument(Compiler* c, uint32_t first, uint32_t end, bool nth) {
    CssSelectorArgument argument = emptyArgument();
    if (nth) {
        argument.a = int32_t(nodeAt(c, first + 1)->number);
        argument.b = int32_t(nodeAt(c, first + 2)->number);
        first = after(c, first);
        if (first < end)
            first = after(c, first);
    }

    bool selectors = false;
    for (uint32_t i = first; i < end; i = after(c, i)) {
        PrCssValueKind kind = nodeAt(c, i)->kind;
        selectors = selectors || kind == PrCssValueKind_Production || kind == PrCssValueKind_Tokens;
    }

    if (selectors || nth) {
        argument.selectors = compileList(c, first, end, false);
    } else {
        for (uint32_t i = first; i < end; i = after(c, i)) {
            const CssValueNode* item = nodeAt(c, i);
            if (cssValueIsLiteral(item, '*'))
                argument.name = kCssSelectorAll;
            if (item->kind == PrCssValueKind_Literal)
                continue;
            uint32_t at;
            if (item->kind == PrCssValueKind_Integer || item->kind == PrCssValueKind_Number) {
                at = c->pool->numbers.count;
                c->pool->numbers.push(prF64SaturateI32(item->number));
            } else if (item->kind == PrCssValueKind_Keyword) {
                at = addName(c->pool, cssKeywordName(cssValueKeyword(item)));
            } else {
                at = addName(c->pool, cssValuePoolText(c->tree, item));
                c->pool->names.data[at].string = item->kind == PrCssValueKind_String;
            }
            if (argument.itemCount++ == 0)
                argument.items = at;
        }
    }
    c->pool->arguments.push(argument);
    return c->pool->arguments.count - 1;
}

// Compiles a <pseudo-class-selector>. `node` is the production. The value of its name holds the pseudo that it names,
// and its argument is between the '(' after the name and the closing ')'.
void compilePseudoClass(Compiler* c, uint32_t node, CssSimpleSelector* out) {
    uint32_t name = node + 2;
    CssPseudoClass pseudo = CssPseudoClass(nodeAt(c, name)->aux);
    out->kind = CssSimpleKind_PseudoClass;
    out->pseudoClass = pseudo;
    out->argument = kCssSelectorNone;
    if (!bitsetTest(kCssPseudoClassFunction, pseudo))
        return;

    bool outer = c->matchesAny;
    c->matchesAny = pseudo == CssPseudoClass_IsFn || pseudo == CssPseudoClass_WhereFn || pseudo == CssPseudoClass_NotFn;
    out->argument = compileArgument(c, name + 2, after(c, node) - 1, isNth(pseudo));
    c->matchesAny = outer;
    const CssSelectorArgument* argument = &c->pool->arguments.data[out->argument];
    c->nesting = c->nesting || listNests(c->pool, argument->selectors);
}

// Compiles a <pt-name-and-class-selector> in [first, end). It holds a name, `*` or neither, then its classes
// (css-view-transitions-2/#pseudo-element-class-additions).
uint32_t compileTransitionArgument(Compiler* c, uint32_t first, uint32_t end) {
    CssSelectorArgument argument = emptyArgument();
    bool dot = false;       // true if the previous value is the '.' of a class
    for (uint32_t i = first; i < end; ++i) {
        const CssValueNode* value = nodeAt(c, i);
        if (cssValueIsLiteral(value, '*')) {
            argument.name = kCssSelectorAll;
        } else if (value->kind == PrCssValueKind_Ident) {
            uint32_t at = addName(c->pool, cssValuePoolText(c->tree, value));
            if (!dot)
                argument.name = at;
            else if (argument.itemCount++ == 0)
                argument.items = at;
        }
        dot = cssValueIsLiteral(value, '.');
    }
    c->pool->arguments.push(argument);
    return c->pool->arguments.count - 1;
}

// Compiles a <pseudo-element-selector>. It is the syntax of a pseudo-class with a second colon, or one of the four
// pseudo-elements that CSS 2 wrote with one colon.
void compilePseudoElement(Compiler* c, uint32_t node, CssSimpleSelector* out) {
    out->kind = CssSimpleKind_PseudoElement;
    out->argument = kCssSelectorNone;
    out->pseudoElement = cssMatchedPseudoElement(c->tree, node);
    if (cssValueIsProduction(nodeAt(c, node + 1), CssValueDef_LegacyPseudoElementSelector))
        return;

    // The name follows a ':' and the ':' of the pseudo-class syntax. An argument is between the '(' after the name and
    // the ')' that closes the pseudo-class syntax.
    uint32_t inner = node + 2;
    uint32_t name = inner + 2;
    if (out->pseudoElement == CssPseudoElement_UnknownWebkit)
        out->name = addName(c->pool, cssValuePoolText(c->tree, nodeAt(c, name)));
    if (isNamedTransition(out->pseudoElement))
        out->argument = compileTransitionArgument(c, name + 2, after(c, inner) - 1);
    else if (bitsetTest(kCssPseudoElementFunction, out->pseudoElement))
        out->argument = compileArgument(c, name + 2, after(c, inner) - 1, false);
}

// Compiles an <attribute-selector>: '[' <wq-name> [ <attr-matcher> value <attr-modifier>? ]? ']'.
void compileAttribute(Compiler* c, uint32_t node, CssSimpleSelector* out) {
    out->kind = CssSimpleKind_Attribute;
    out->match = CssAttributeMatch_Exists;
    out->value = kCssSelectorNone;
    for (uint32_t i = node + 1; i < after(c, node); i = after(c, i)) {
        const CssValueNode* child = nodeAt(c, i);
        if (cssValueIsProduction(child, CssValueDef_WqName)) {
            compileQualifiedName(c, i, false, out);
        } else if (cssValueIsProduction(child, CssValueDef_AttrMatcher)) {
            const CssValueNode* first = nodeAt(c, i + 1);
            switch (cssValueLiteral(first)) {
                case '~': out->match = CssAttributeMatch_Includes; break;
                case '|': out->match = CssAttributeMatch_DashMatch; break;
                case '^': out->match = CssAttributeMatch_Prefix; break;
                case '$': out->match = CssAttributeMatch_Suffix; break;
                case '*': out->match = CssAttributeMatch_Substring; break;
                default: out->match = CssAttributeMatch_Equals; break;
            }
        } else if (cssValueIsProduction(child, CssValueDef_AttrModifier)) {
            bool insensitive = cssValueKeyword(nodeAt(c, i + 1)) == PrCssKeyword_I;
            out->letterCase = insensitive ? CssAttributeCase_Insensitive : CssAttributeCase_Sensitive;
        } else if (child->kind == PrCssValueKind_Ident || child->kind == PrCssValueKind_String) {
            out->value = addName(c->pool, cssValuePoolText(c->tree, child));
        }
    }
    if (out->letterCase == CssAttributeCase_Default && domCaseInsensitiveAttribute(nameLowered(c->pool, out->name)))
        out->letterCase = CssAttributeCase_Html;
}

// Compiles a <type-selector>: a qualified name, or a namespace and '*'.
void compileType(Compiler* c, uint32_t node, CssSimpleSelector* out) {
    const CssValueNode* first = nodeAt(c, node + 1);
    if (cssValueIsProduction(first, CssValueDef_WqName)) {
        out->kind = CssSimpleKind_Type;
        compileQualifiedName(c, node + 1, true, out);
        out->tag = domTagFromText(nameLowered(c->pool, out->name));
        return;
    }
    out->kind = CssSimpleKind_Universal;
    out->name = kCssSelectorNone;
    compileNamespace(c, cssValueIsProduction(first, CssValueDef_NsPrefix) ? node + 1 : 0, true, out);
}

// Returns true if a node is one simple selector of a compound. For a node that is not, countSimple counts the simple
// selectors under it.
bool isSimple(const CssValueNode* value) {
    if (cssValueIsLiteral(value, '&'))
        return true;
    if (value->kind != PrCssValueKind_Production)
        return false;
    switch (cssValueProduction(value)) {
        case CssValueDef_TypeSelector:
        case CssValueDef_IdSelector:
        case CssValueDef_ClassSelector:
        case CssValueDef_AttributeSelector:
        case CssValueDef_PseudoClassSelector:
        case CssValueDef_PseudoElementSelector:
            return true;
        default:
            return false;
    }
}

uint32_t countSimple(const Compiler* c, uint32_t first, uint32_t end) {
    uint32_t count = 0;
    for (uint32_t i = first; i < end; i = after(c, i)) {
        if (isSimple(nodeAt(c, i)))
            ++count;
        else if (nodeAt(c, i)->kind == PrCssValueKind_Production)
            count += countSimple(c, i + 1, after(c, i));
    }
    return count;
}

void compileSimple(Compiler* c, uint32_t node, uint32_t slot) {
    CssSimpleSelector simple = emptySimple();
    const CssValueNode* value = nodeAt(c, node);
    if (cssValueIsLiteral(value, '&')) {
        simple.kind = CssSimpleKind_Nesting;
        c->nesting = true;
    } else if (cssValueProduction(value) == CssValueDef_TypeSelector) {
        compileType(c, node, &simple);
    } else if (cssValueProduction(value) == CssValueDef_IdSelector) {
        simple.kind = CssSimpleKind_Id;
        simple.name = addName(c->pool, cssValuePoolText(c->tree, nodeAt(c, node + 1)));
    } else if (cssValueProduction(value) == CssValueDef_ClassSelector) {
        simple.kind = CssSimpleKind_Class;
        simple.name = addName(c->pool, cssValuePoolText(c->tree, nodeAt(c, node + 2)));
    } else if (cssValueProduction(value) == CssValueDef_AttributeSelector) {
        compileAttribute(c, node, &simple);
    } else if (cssValueProduction(value) == CssValueDef_PseudoClassSelector) {
        compilePseudoClass(c, node, &simple);
    } else {
        compilePseudoElement(c, node, &simple);
    }
    c->pool->simple.data[slot] = simple;
}

void fillSimple(Compiler* c, uint32_t first, uint32_t end, uint32_t* slot) {
    for (uint32_t i = first; i < end; i = after(c, i)) {
        if (isSimple(nodeAt(c, i)))
            compileSimple(c, i, (*slot)++);
        else if (nodeAt(c, i)->kind == PrCssValueKind_Production)
            fillSimple(c, i + 1, after(c, i), slot);
    }
}

// Returns true if the nodes in [first, end) of a compound hold a type selector or a universal selector.
bool writesType(const Compiler* c, uint32_t first, uint32_t end) {
    for (uint32_t i = first; i < end; i = after(c, i)) {
        const CssValueNode* value = nodeAt(c, i);
        if (cssValueIsProduction(value, CssValueDef_TypeSelector))
            return true;
        if (!isSimple(value) && value->kind == PrCssValueKind_Production && writesType(c, i + 1, after(c, i)))
            return true;
    }
    return false;
}

// Compiles one compound unit: a <compound-selector>, or a <complex-selector-unit> with the pseudo-elements after its
// compound. `subject` says whether it is the subject's compound.
void compileCompound(Compiler* c, uint32_t node, uint32_t slot, bool subject) {
    uint32_t first = node + 1;
    uint32_t end = after(c, node);
    // A compound without a type selector still matches only elements in the default namespace, if the sheet declares
    // one (selectors-4/#type-nmsp): it holds an implied universal selector in that namespace, first. The subject of a
    // selector in the argument of :is(), :where() or :not() is not restricted (selectors-4/#matches).
    const CssSheetData* sheet = c->context->sheet;
    PrStringView fallback = {nullptr, 0};
    bool implied = !(subject && c->matchesAny) && sheet != nullptr &&
        cssSheetNamespace(sheet, PrStringView{nullptr, 0}, &fallback) && !writesType(c, first, end);
    uint32_t simple = c->pool->simple.count;
    uint32_t count = countSimple(c, first, end) + (implied ? 1 : 0);
    c->pool->compounds.data[slot].simple = simple;
    c->pool->compounds.data[slot].simpleCount = count;
    for (uint32_t i = 0; i < count; ++i)
        c->pool->simple.push(CssSimpleSelector{});

    uint32_t at = simple;
    if (implied) {
        CssSimpleSelector universal = emptySimple();
        universal.kind = CssSimpleKind_Universal;
        addNamespace(c, PrStringView{nullptr, 0}, fallback, &universal);
        c->pool->simple.data[at++] = universal;
    }
    fillSimple(c, first, end, &at);
}

// Compiles a <combinator>: its first literal, and for a reference combinator, the attribute that it names.
CssCombinator compileCombinator(Compiler* c, uint32_t node, uint32_t* reference) {
    const CssValueNode* first = nodeAt(c, node + 1);
    switch (cssValueLiteral(first)) {
        case '>': return CssCombinator_Child;
        case '+': return CssCombinator_NextSibling;
        case '~': return CssCombinator_SubsequentSibling;
        case '|': return CssCombinator_Column;
        default: break;
    }

    CssSimpleSelector attribute = emptySimple();
    attribute.kind = CssSimpleKind_Attribute;
    compileQualifiedName(c, node + 2, false, &attribute);
    *reference = c->pool->simple.count;
    c->pool->simple.push(attribute);
    return CssCombinator_Reference;
}

bool isUnit(const CssValueNode* value) {
    return cssValueIsProduction(value, CssValueDef_ComplexSelectorUnit) ||
        cssValueIsProduction(value, CssValueDef_CompoundSelector);
}

// Compiles one complex selector into the reserved `slot`. `top` means a selector of the list being parsed rather than
// of an argument. A nested rule's selectors are relative only in that list.
void compileComplex(Compiler* c, uint32_t node, uint32_t slot, bool top) {
    // An item of a forgiving list that is not a selector.
    if (nodeAt(c, node)->kind == PrCssValueKind_Tokens) {
        CssComplexSelector invalid = {};
        invalid.compound = c->pool->compounds.count;
        invalid.pseudoElement = CssPseudoElement_None;
        invalid.flags = CssComplexFlag_Invalid;
        invalid.text = addName(c->pool, cssValuePoolText(c->tree, nodeAt(c, node)));
        c->pool->complex.data[slot] = invalid;
        return;
    }

    bool outer = c->nesting;
    c->nesting = false;

    // A relative selector is a leading combinator and a complex selector.
    uint32_t reference = kCssSelectorNone;
    CssCombinator leading = CssCombinator_None;
    uint32_t body = node;
    const CssValueNode* value = nodeAt(c, node);
    if (cssValueIsProduction(value, CssValueDef_RelativeSelector) ||
            cssValueIsProduction(value, CssValueDef_RelativeRealSelector)) {
        for (uint32_t i = node + 1; i < after(c, node); i = after(c, i)) {
            if (cssValueIsProduction(nodeAt(c, i), CssValueDef_Combinator))
                leading = compileCombinator(c, i, &reference);
            else
                body = i;
        }
    }

    // The units, and the combinators between them, in source order. A compound selector argument is one unit.
    uint32_t first = cssValueIsProduction(nodeAt(c, body), CssValueDef_CompoundSelector) ? body : body + 1;
    uint32_t end = after(c, body);
    uint32_t units = 0;
    for (uint32_t i = first; i < end; i = after(c, i))
        units += isUnit(nodeAt(c, i)) ? 1 : 0;

    CssComplexSelector complex = {};
    complex.compound = c->pool->compounds.count;
    complex.compoundCount = units;
    for (uint32_t i = 0; i < units; ++i) {
        CssCompoundSelector compound = {};
        compound.reference = kCssSelectorNone;
        c->pool->compounds.push(compound);
    }

    uint32_t written = 0;
    CssCombinator combinator = leading;
    uint32_t combinatorReference = reference;
    for (uint32_t i = first; i < end; i = after(c, i)) {
        if (cssValueIsProduction(nodeAt(c, i), CssValueDef_Combinator)) {
            combinator = compileCombinator(c, i, &combinatorReference);
            continue;
        }
        if (!isUnit(nodeAt(c, i)))
            continue;

        // The compounds run right to left, subject first.
        uint32_t at = complex.compound + units - 1 - written;
        if (written != 0 && combinator == CssCombinator_None)
            combinator = CssCombinator_Descendant;
        compileCompound(c, i, at, written == units - 1);
        c->pool->compounds.data[at].combinator = combinator;
        c->pool->compounds.data[at].reference = combinatorReference;
        combinator = CssCombinator_None;
        combinatorReference = kCssSelectorNone;
        ++written;
    }

    // The selector selects the pseudo-element that ends the subject's compound.
    const CssCompoundSelector* subject = &c->pool->compounds.data[complex.compound];
    complex.pseudoElement = CssPseudoElement_None;
    for (uint32_t i = 0; i < subject->simpleCount; ++i) {
        const CssSimpleSelector* simple = &c->pool->simple.data[subject->simple + i];
        if (simple->kind == CssSimpleKind_PseudoElement)
            complex.pseudoElement = simple->pseudoElement;
    }

    if (leading != CssCombinator_None)
        complex.flags |= CssComplexFlag_Relative;

    // css-nesting-1/#nesting: a nested rule's selector is relative to an implied `&`, unless it contains one and does
    // not start with a combinator. Directly inside @scope, the implied selector is :scope
    // (css-nesting-1/#nesting-at-scope).
    CssSelectorNesting nesting = c->context->nesting;
    if (top && nesting != CssSelectorNesting_None && (leading != CssCombinator_None || !c->nesting)) {
        complex.flags |= CssComplexFlag_Relative;
        CssCompoundSelector* leftmost = &c->pool->compounds.data[complex.compound + units - 1];
        if (leftmost->combinator == CssCombinator_None)
            leftmost->combinator = CssCombinator_Descendant;
        if (nesting == CssSelectorNesting_Style) {
            complex.flags |= CssComplexFlag_ImpliedNesting;
            c->nesting = true;
        } else {
            complex.flags |= CssComplexFlag_ImpliedScope;
        }
    }
    if (c->nesting)
        complex.flags |= CssComplexFlag_Nesting;

    c->pool->complex.data[slot] = complex;
    c->nesting = outer || c->nesting;
}

// ---------------------------------------------------------------------------------------------------------------------
// Planning: how the records are matched, beyond what they select

// The cost of testing a simple selector against an element, cheapest first: comparing a name, reading one property of
// the element, matching an argument's selectors, and searching beyond the element.
enum {
    Cost_Name,
    Cost_Element,
    Cost_Argument,
    Cost_Search,
    Cost_Count,
};
typedef uint8_t Cost;

Cost costOf(const CssSimpleSelector* simple) {
    switch (simple->kind) {
        case CssSimpleKind_Attribute:
            return Cost_Element;
        case CssSimpleKind_PseudoClass:
            if (simple->pseudoClass == CssPseudoClass_HasFn)
                return Cost_Search;
            return simple->argument == kCssSelectorNone ? Cost_Element : Cost_Argument;
        default:
            return Cost_Name;
    }
}

bool readsStructure(const CssSelectorPool* pool, const CssSimpleSelector* simple);

// Returns true if any simple selector of a selector in `list` reads structure.
bool listReadsStructure(const CssSelectorPool* pool, CssSelectorList list) {
    for (uint32_t i = 0; i < list.count; ++i) {
        const CssComplexSelector* complex = &pool->complex.data[list.first + i];
        for (uint32_t k = 0; k < complex->compoundCount; ++k) {
            const CssCompoundSelector* compound = &pool->compounds.data[complex->compound + k];
            for (uint32_t j = 0; j < compound->simpleCount; ++j) {
                if (readsStructure(pool, &pool->simple.data[compound->simple + j]))
                    return true;
            }
        }
    }
    return false;
}

// Returns true if a simple selector reads an element's position among its siblings or its children. Such a selector is
// a structural pseudo-class (selectors-4/#structural-pseudos), or :is(), :where() or :not() of one.
bool readsStructure(const CssSelectorPool* pool, const CssSimpleSelector* simple) {
    if (simple->kind != CssSimpleKind_PseudoClass)
        return false;
    switch (simple->pseudoClass) {
        case CssPseudoClass_Empty:
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
            return true;
        case CssPseudoClass_IsFn:
        case CssPseudoClass_WhereFn:
        case CssPseudoClass_NotFn:
            return simple->argument != kCssSelectorNone &&
                listReadsStructure(pool, pool->arguments.data[simple->argument].selectors);
        default:
            return false;
    }
}

// Returns the order in which a simple selector is tested: by cost, with the selectors that read structure last.
uint32_t orderOf(const CssSelectorPool* pool, const CssSimpleSelector* simple) {
    return costOf(simple) + (readsStructure(pool, simple) ? Cost_Count : 0);
}

// Numbers the compound's simple selectors in written order. The selectors before any pseudo-element are sorted stably
// by their test order, and the range of the selectors that read structure is recorded. The rest apply to the
// pseudo-element and keep their position.
void orderCompound(CssSelectorPool* pool, CssCompoundSelector* compound) {
    CssSimpleSelector* simple = pool->simple.data + compound->simple;
    uint32_t tested = 0;
    bool ordered = true;
    uint32_t placed = 0;
    for (uint32_t i = 0; i < compound->simpleCount; ++i) {
        simple[i].written = i;
        if (tested == i && simple[i].kind != CssSimpleKind_PseudoElement) {
            ordered = ordered && (i == 0 || orderOf(pool, &simple[i - 1]) <= orderOf(pool, &simple[i]));
            placed += readsStructure(pool, &simple[i]) ? 1 : 0;
            ++tested;
        }
    }
    // If the compound is too long to record the range, the range is not recorded and the selectors are tested in place.
    if (placed <= 0xff && tested <= 0xffff) {
        compound->placedCount = uint8_t(placed);
        compound->placed = uint16_t(tested - placed);
    }
    if (ordered)
        return;

    // A counting sort that uses the end of the pool as scratch.
    uint32_t scratch = pool->simple.count;
    for (uint32_t order = 0; order < 2 * Cost_Count; ++order) {
        for (uint32_t i = 0; i < tested; ++i) {
            CssSimpleSelector next = pool->simple.data[compound->simple + i];
            if (orderOf(pool, &next) == order)
                pool->simple.push(next);
        }
    }
    prMemCopy(pool->simple.data + compound->simple, pool->simple.data + scratch, sizeof(CssSimpleSelector) * tested);
    pool->simple.count = scratch;
}

// Computes the positions that the subject of `complex` may have relative to an anchor. The combinators decide, starting
// from the leftmost. Sibling combinators before any other step move along the anchor's siblings, and child and
// descendant combinators move down.
void reachOf(const CssSelectorPool* pool, CssComplexSelector* complex) {
    complex->reach = CssReach_Descendant;
    complex->siblings = 0;
    complex->depth = 0;
    bool below = false;
    for (uint32_t i = complex->compoundCount; i-- > 0;) {
        switch (pool->compounds.data[complex->compound + i].combinator) {
            case CssCombinator_None:
            case CssCombinator_Descendant:
                below = true;
                complex->depth = kCssReachAny;
                break;
            case CssCombinator_Child:
                below = true;
                complex->depth = complex->depth != kCssReachAny ? complex->depth + 1 : kCssReachAny;
                break;
            case CssCombinator_NextSibling:
            case CssCombinator_SubsequentSibling:
                if (below)
                    break;
                complex->reach = CssReach_Sibling;
                if (pool->compounds.data[complex->compound + i].combinator == CssCombinator_SubsequentSibling)
                    complex->siblings = kCssReachAny;
                else if (complex->siblings != kCssReachAny)
                    ++complex->siblings;
                break;
            case CssCombinator_Reference:
                complex->reach = CssReach_Tree;
                return;
            default:
                break;
        }
    }
}

void plan(CssSelectorPool* pool) {
    for (uint32_t i = 0; i < pool->compounds.count; ++i)
        orderCompound(pool, &pool->compounds.data[i]);
    for (uint32_t i = 0; i < pool->complex.count; ++i)
        reachOf(pool, &pool->complex.data[i]);
}

// ---------------------------------------------------------------------------------------------------------------------
// Holding

void clearPool(CssSelectorPool* pool) {
    pool->complex.count = 0;
    pool->compounds.count = 0;
    pool->simple.count = 0;
    pool->arguments.count = 0;
    pool->namespaces.count = 0;
    pool->names.count = 0;
    pool->numbers.count = 0;
    prTextBufferClear(&pool->text);
}

// Copies the records to `*at`, and moves `*at` past them.
template <typename T>
T* place(char** at, const RawArray<T>& records) {
    T* out = reinterpret_cast<T*>(*at);
    size_t size = sizeof(T) * records.count;
    if (size != 0)
        prMemCopy(out, records.data, size);
    *at += size;
    return out;
}

// Plans the pool's records and stores them with `list` in one allocation. The allocation holds the CssSelectors, then
// each run of records, then the text.
PrCssSelectors* hold(CssSelectorPool* pool, CssSelectorList list) {
    plan(pool);
    size_t size = sizeof(CssSelectors) + sizeof(CssComplexSelector) * pool->complex.count +
        sizeof(CssCompoundSelector) * pool->compounds.count + sizeof(CssSimpleSelector) * pool->simple.count +
        sizeof(CssSelectorArgument) * pool->arguments.count + sizeof(CssSelectorNamespace) * pool->namespaces.count +
        sizeof(CssSelectorName) * pool->names.count + sizeof(int32_t) * pool->numbers.count + pool->text.size;
    char* memory = static_cast<char*>(prMemoryAlloc(size));
    PrCssSelectors* out = reinterpret_cast<PrCssSelectors*>(memory);
    char* at = memory + sizeof(CssSelectors);
    out->list = list;
    out->size = uint32_t(size);
    out->complexCount = pool->complex.count;
    out->readsHost = false;
    for (uint32_t i = 0; i < pool->simple.count && !out->readsHost; ++i) {
        const CssSimpleSelector& simple = pool->simple.data[i];
        out->readsHost = simple.kind == CssSimpleKind_PseudoClass &&
            (simple.pseudoClass == CssPseudoClass_Host || simple.pseudoClass == CssPseudoClass_HostFn ||
                simple.pseudoClass == CssPseudoClass_HostContextFn);
    }
    out->complex = place(&at, pool->complex);
    out->compounds = place(&at, pool->compounds);
    out->simple = place(&at, pool->simple);
    out->arguments = place(&at, pool->arguments);
    out->namespaces = place(&at, pool->namespaces);
    out->names = place(&at, pool->names);
    out->numbers = place(&at, pool->numbers);
    out->text = at;
    if (pool->text.size != 0)
        prMemCopy(at, pool->text.data, pool->text.size);
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// Reading directly
//
// Some selector lists are read here without the grammar, into the same records that compiling their parse would create.
// Such a list uses only type, universal, ID, class and attribute selectors, pseudo-classes without arguments, and a
// final pseudo-element, joined by the descendant, child and sibling combinators. Anything else, valid or not, is left
// to the grammar. A false result means only that the list was left to the grammar, not that it is invalid.

struct Direct {
    const CssTokenArray* tokens;
    uint32_t pos;
    uint32_t end;
    CssSelectorPool* pool;
    bool engine;                    // true if the selectors may name the engine's private pseudo-classes
};

CssTokenKind directKind(const Direct* d) {
    return d->pos < d->end ? cssTokenKind(d->tokens, d->pos) : CssTokenKind_Eof;
}

bool directDelim(const Direct* d, char delim) {
    return directKind(d) == CssTokenKind_Delim && cssTokenDelim(d->tokens, d->pos) == uint32_t(uint8_t(delim));
}

void directSkipWhitespace(Direct* d) {
    while (directKind(d) == CssTokenKind_Whitespace)
        ++d->pos;
}

// Reads the value of a token of `kind` written without escapes, and steps over the token.
bool directPlain(Direct* d, CssTokenKind kind, PrStringView* out) {
    if (directKind(d) != kind || (cssTokenFlags(d->tokens, d->pos) & CssTokenFlag_PlainText) == 0)
        return false;
    *out = cssTokenValueText(d->tokens, d->pos++);
    return true;
}

CssSimpleSelector directSimple(CssSimpleKind kind) {
    CssSimpleSelector simple = {};
    simple.kind = kind;
    simple.space = kCssSelectorNone;
    simple.name = kCssSelectorNone;
    simple.value = kCssSelectorNone;
    simple.argument = kCssSelectorNone;
    simple.pseudoClass = CssPseudoClass_None;
    return simple;
}

// Reads an <attribute-selector> with no namespace prefix, after its '['.
bool directAttribute(Direct* d, uint32_t open, CssSimpleSelector* out) {
    uint32_t close = cssTokenCloser(d->tokens, open);
    if (close >= d->end)
        return false;
    *out = directSimple(CssSimpleKind_Attribute);
    out->match = CssAttributeMatch_Exists;
    PrStringView name;
    directSkipWhitespace(d);
    if (!directPlain(d, CssTokenKind_Ident, &name))
        return false;
    out->name = addName(d->pool, name);
    directSkipWhitespace(d);
    if (d->pos != close) {
        // An <attr-matcher>'s two delims are one token each, with nothing between them.
        if (directKind(d) != CssTokenKind_Delim)
            return false;
        switch (cssTokenDelim(d->tokens, d->pos)) {
            case '=': out->match = CssAttributeMatch_Equals; break;
            case '~': out->match = CssAttributeMatch_Includes; break;
            case '|': out->match = CssAttributeMatch_DashMatch; break;
            case '^': out->match = CssAttributeMatch_Prefix; break;
            case '$': out->match = CssAttributeMatch_Suffix; break;
            case '*': out->match = CssAttributeMatch_Substring; break;
            default: return false;
        }
        ++d->pos;
        if (out->match != CssAttributeMatch_Equals) {
            if (!directDelim(d, '='))
                return false;
            ++d->pos;
        }
        directSkipWhitespace(d);
        PrStringView value;
        if (!directPlain(d, CssTokenKind_Ident, &value) && !directPlain(d, CssTokenKind_String, &value))
            return false;
        out->value = addName(d->pool, value);
        directSkipWhitespace(d);
        PrStringView modifier;
        if (directPlain(d, CssTokenKind_Ident, &modifier)) {
            if (prAsciiEqualsFold(modifier, "i"))
                out->letterCase = CssAttributeCase_Insensitive;
            else if (prAsciiEqualsFold(modifier, "s"))
                out->letterCase = CssAttributeCase_Sensitive;
            else
                return false;
            directSkipWhitespace(d);
        }
        if (d->pos != close)
            return false;
    }
    d->pos = close + 1;
    if (out->letterCase == CssAttributeCase_Default && domCaseInsensitiveAttribute(nameLowered(d->pool, out->name)))
        out->letterCase = CssAttributeCase_Html;
    return true;
}

// Reads a pseudo-class or pseudo-element after its ':'. A pseudo-element has two colons, or is one of those that CSS 2
// wrote with one colon.
bool directPseudo(Direct* d, CssSimpleSelector* out) {
    bool element = directKind(d) == CssTokenKind_Colon;
    if (element)
        ++d->pos;
    PrStringView name;
    if (!directPlain(d, CssTokenKind_Ident, &name))
        return false;
    CssPseudoClass pseudoClass = element ? CssPseudoClass_None : cssPseudoClass(name, false);
    if (pseudoClass >= CssPseudoClass_FirstPrivate && !d->engine)
        pseudoClass = CssPseudoClass_None;
    if (pseudoClass != CssPseudoClass_None) {
        *out = directSimple(CssSimpleKind_PseudoClass);
        out->pseudoClass = pseudoClass;
        return true;
    }
    if (!element && !prAsciiEqualsFold(name, "before") && !prAsciiEqualsFold(name, "after") &&
            !prAsciiEqualsFold(name, "first-line") && !prAsciiEqualsFold(name, "first-letter"))
        return false;
    CssPseudoElement pseudoElement = cssPseudoElement(name, false);
    if (pseudoElement == CssPseudoElement_None)
        return false;
    *out = directSimple(CssSimpleKind_PseudoElement);
    out->pseudoElement = pseudoElement;
    return true;
}

// Reads one compound selector, pushing its simple selectors onto the pool. Sets `*ended` if it ends with a
// pseudo-element, which also ends the complex selector.
bool directCompound(Direct* d, CssCompoundSelector* out, bool* ended) {
    CssSelectorPool* pool = d->pool;
    out->simple = pool->simple.count;
    PrStringView text;
    if (directPlain(d, CssTokenKind_Ident, &text)) {
        CssSimpleSelector simple = directSimple(CssSimpleKind_Type);
        simple.name = addName(pool, text);
        simple.tag = domTagFromText(nameLowered(pool, simple.name));
        pool->simple.push(simple);
    } else if (directDelim(d, '*')) {
        ++d->pos;
        pool->simple.push(directSimple(CssSimpleKind_Universal));
    }
    for (;;) {
        CssSimpleSelector simple;
        CssTokenKind kind = directKind(d);
        if (kind == CssTokenKind_Hash) {
            if ((cssTokenFlags(d->tokens, d->pos) & CssTokenFlag_Id) == 0 || !directPlain(d, CssTokenKind_Hash, &text))
                return false;
            simple = directSimple(CssSimpleKind_Id);
            simple.name = addName(pool, text);
        } else if (directDelim(d, '.')) {
            ++d->pos;
            if (!directPlain(d, CssTokenKind_Ident, &text))
                return false;
            simple = directSimple(CssSimpleKind_Class);
            simple.name = addName(pool, text);
        } else if (kind == CssTokenKind_OpenBracket) {
            if (!directAttribute(d, d->pos++, &simple))
                return false;
        } else if (kind == CssTokenKind_Colon) {
            ++d->pos;
            if (!directPseudo(d, &simple))
                return false;
        } else {
            break;
        }
        pool->simple.push(simple);
        if (simple.kind == CssSimpleKind_PseudoElement) {
            *ended = true;
            break;
        }
    }
    out->simpleCount = pool->simple.count - out->simple;
    return out->simpleCount != 0;
}

// Reads one complex selector, up to the ',' or the end that follows it, and steps over the ','. Sets `*more` if a ','
// followed.
bool directComplex(Direct* d, CssComplexSelector* out, bool* more) {
    CssSelectorPool* pool = d->pool;
    *out = CssComplexSelector{};
    out->compound = pool->compounds.count;
    out->pseudoElement = CssPseudoElement_None;
    CssCombinator combinator = CssCombinator_None;
    for (;;) {
        CssCompoundSelector compound = {};
        compound.reference = kCssSelectorNone;
        compound.combinator = combinator;
        bool ended = false;
        if (!directCompound(d, &compound, &ended))
            return false;
        pool->compounds.push(compound);
        if (ended) {
            const CssSimpleSelector* last = &pool->simple.last();
            out->pseudoElement = last->pseudoElement;
        }

        bool spaced = directKind(d) == CssTokenKind_Whitespace;
        directSkipWhitespace(d);
        CssTokenKind kind = directKind(d);
        if (kind == CssTokenKind_Comma || kind == CssTokenKind_Eof) {
            *more = kind == CssTokenKind_Comma;
            if (*more)
                ++d->pos;
            break;
        }
        if (ended)
            return false;
        if (directDelim(d, '>'))
            combinator = CssCombinator_Child;
        else if (directDelim(d, '+'))
            combinator = CssCombinator_NextSibling;
        else if (directDelim(d, '~'))
            combinator = CssCombinator_SubsequentSibling;
        else if (spaced)
            combinator = CssCombinator_Descendant;
        else
            return false;
        if (combinator != CssCombinator_Descendant) {
            ++d->pos;
            directSkipWhitespace(d);
        }
    }

    // The compounds run right to left, subject first.
    out->compoundCount = pool->compounds.count - out->compound;
    CssCompoundSelector* compounds = pool->compounds.data + out->compound;
    for (uint32_t i = 0, j = out->compoundCount - 1; i < j; ++i, --j) {
        CssCompoundSelector swapped = compounds[i];
        compounds[i] = compounds[j];
        compounds[j] = swapped;
    }
    return true;
}

// Reads the selector list in `span` directly into the cleared pool, if it can be read directly.
bool directList(CssSelectorPool* pool, const CssTokenArray* tokens, CssTokenSpan span, bool engine, CssSelectorList* out) {
    Direct d = {tokens, span.start, span.start + span.count, pool, engine};
    *out = CssSelectorList{0, 0};
    for (bool more = true; more;) {
        directSkipWhitespace(&d);
        CssComplexSelector complex;
        more = false;
        if (!directComplex(&d, &complex, &more))
            return false;
        pool->complex.push(complex);
        ++out->count;
    }
    return true;
}

// Parses the selector list in `span` with the grammar, and compiles the parse.
PrCssSelectors* PR_NULLABLE parseCompiled(CssParser* parser, const CssTokenStream* ts,
        CssTokenSpan span, const CssSelectorContext* context) {
    CssTokenStream stream = cssSubStream(ts, span);
    CssParseContext parse = {context->sheet};
    CssValueDef root =
        context->nesting == CssSelectorNesting_None ? CssValueDef_SelectorList : CssValueDef_RelativeSelectorList;

    uint32_t values = parser->pool.values.count;
    uint32_t text = parser->pool.text.count;
    CssValueSpan tree = {};
    PrCssSelectors* out = nullptr;
    if (cssParseProduction(root, &stream, &parse, &parser->pool, &parser->match, &tree)) {
        // A <selector-list> keeps a node, and the complex selectors are its children.
        CssSelectorPool* pool = &parser->selectors;
        clearPool(pool);
        Compiler c = {.tree = &parser->pool, .context = context, .pool = pool, .nesting = false, .matchesAny = false};
        uint32_t first = tree.start;
        if (cssValueIsProduction(nodeAt(&c, first), CssValueDef_SelectorList))
            ++first;
        CssSelectorList list = compileList(&c, first, tree.start + tree.count, true);
        out = hold(pool, list);
        cssSelectorsSpecify(out, context->parent);
    }
    parser->pool.values.count = values;
    parser->pool.text.count = text;
    return out;
}

#ifndef NDEBUG
// Returns true if two selector lists have the same records. Reading a list directly and compiling its parse must
// produce the same records.
bool selectorsEquals(const PrCssSelectors* a, const PrCssSelectors* b) {
    if (a->size != b->size || a->complexCount != b->complexCount || a->list.first != b->list.first ||
            a->list.count != b->list.count)
        return false;
    for (uint32_t i = 0; i < a->complexCount; ++i) {
        const CssComplexSelector& x = a->complex[i];
        const CssComplexSelector& y = b->complex[i];
        if (x.compound != y.compound || x.compoundCount != y.compoundCount ||
                x.specificity != y.specificity || x.pseudoElement != y.pseudoElement ||
                x.flags != y.flags || x.reach != y.reach || x.siblings != y.siblings || x.depth != y.depth)
            return false;
        for (uint32_t j = x.compound; j < x.compound + x.compoundCount; ++j) {
            const CssCompoundSelector& p = a->compounds[j];
            const CssCompoundSelector& q = b->compounds[j];
            if (p.simple != q.simple || p.simpleCount != q.simpleCount || p.reference != q.reference ||
                    p.combinator != q.combinator)
                return false;
            for (uint32_t k = p.simple; k < p.simple + p.simpleCount; ++k) {
                const CssSimpleSelector& m = a->simple[k];
                const CssSimpleSelector& n = b->simple[k];
                if (m.kind != n.kind || m.prefix != n.prefix || m.match != n.match ||
                        m.letterCase != n.letterCase || m.pseudoClass != n.pseudoClass ||
                        m.pseudoElement != n.pseudoElement || m.tag != n.tag || m.space != n.space ||
                        m.name != n.name || m.value != n.value || m.argument != n.argument || m.written != n.written)
                    return false;
            }
        }
    }
    uint32_t names = uint32_t(a->numbers - reinterpret_cast<const int32_t*>(a->names)) *
        uint32_t(sizeof(int32_t)) / uint32_t(sizeof(CssSelectorName));
    for (uint32_t i = 0; i < names; ++i) {
        if (cssSelectorName(a, i) != cssSelectorName(b, i) || cssSelectorNameLowered(a, i) != cssSelectorNameLowered(b, i) ||
                a->names[i].hash != b->names[i].hash || a->names[i].string != b->names[i].string)
            return false;
    }
    return true;
}
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Specificity (selectors-4/#specificity-rules)

// The three counts of a specificity, which are added and compared by component.
struct Counts {
    uint32_t ids;
    uint32_t classes;
    uint32_t types;
};

void addSpecificity(Counts* counts, CssSpecificity specificity) {
    counts->ids += specificity >> (2 * kCssSpecificityCountBits);
    counts->classes += (specificity >> kCssSpecificityCountBits) & kCssSpecificityMaxCount;
    counts->types += specificity & kCssSpecificityMaxCount;
}

// Returns the greatest specificity among a list's selectors. :is(), :not(), :has() and `&` take this specificity
// (css-nesting-1/#nest-selector).
CssSpecificity greatestSpecificity(const PrCssSelectors* selectors, CssSelectorList list) {
    CssSpecificity greatest = 0;
    for (uint32_t i = 0; i < list.count; ++i) {
        CssSpecificity specificity = selectors->complex[list.first + i].specificity;
        greatest = specificity > greatest ? specificity : greatest;
    }
    return greatest;
}

// Adds a simple selector's specificity. `nesting` is the specificity of `&`.
void addSimpleSpecificity(const PrCssSelectors* selectors, const CssSimpleSelector* simple,
        CssSpecificity nesting, Counts* counts) {
    switch (simple->kind) {
        case CssSimpleKind_Id:
            ++counts->ids;
            break;
        case CssSimpleKind_Class:
        case CssSimpleKind_Attribute:
            ++counts->classes;
            break;
        case CssSimpleKind_Type:
            ++counts->types;
            break;
        case CssSimpleKind_PseudoElement: {
            // A pseudo-element counts as a type selector. ::slotted() adds its argument's specificity
            // (css-shadow-1/#slotted-pseudo). A named view transition pseudo-element with `*` and no class counts
            // nothing (css-view-transitions-2/#pseudo-element-class-additions).
            const CssSelectorArgument* argument =
                simple->argument != kCssSelectorNone ? &selectors->arguments[simple->argument] : nullptr;
            if (argument == nullptr || argument->name != kCssSelectorAll || argument->itemCount != 0)
                ++counts->types;
            if (simple->pseudoElement == CssPseudoElement_SlottedFn)
                addSpecificity(counts, greatestSpecificity(selectors, argument->selectors));
            break;
        }
        case CssSimpleKind_Nesting:
            addSpecificity(counts, nesting);
            break;
        case CssSimpleKind_PseudoClass: {
            if (simple->argument == kCssSelectorNone) {
                ++counts->classes;
                break;
            }
            // :where() adds nothing. :is(), :not() and :has() add the greatest of their arguments. :nth-child() with an
            // `of` list adds that and a pseudo-class.
            CssSelectorList list = selectors->arguments[simple->argument].selectors;
            switch (simple->pseudoClass) {
                case CssPseudoClass_WhereFn:
                    break;
                case CssPseudoClass_IsFn:
                case CssPseudoClass_NotFn:
                case CssPseudoClass_HasFn:
                    addSpecificity(counts, greatestSpecificity(selectors, list));
                    break;
                default:
                    ++counts->classes;
                    addSpecificity(counts, greatestSpecificity(selectors, list));
                    break;
            }
            break;
        }
        default:
            break;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Serializing (cssom-1/#serializing-selectors)

void serializeList(PrTextBuffer* out, const PrCssSelectors* selectors, CssSelectorList list);

void serializeNamespace(PrTextBuffer* out, const PrCssSelectors* selectors, const CssSimpleSelector* simple) {
    switch (simple->prefix) {
        case CssNamespacePrefix_Any:
            prTextBufferAppend(out, "*|");
            break;
        case CssNamespacePrefix_Empty:
            prTextBufferAppendChar(out, '|');
            break;
        case CssNamespacePrefix_Named:
            cssSerializeIdent(out, cssSelectorName(selectors, selectors->namespaces[simple->space].prefix));
            prTextBufferAppendChar(out, '|');
            break;
        default:
            break;
    }
}

// Writes the name or `*` of an argument, then its items. Each item is written in its original form, as an identifier or
// a string. The ranges of :lang() are written this way, although cssom-1/#serialize-a-simple-selector writes strings
// (cssom/selectorSerialize.html). ::part() and ::picker() separate items with spaces, which their grammars use. A named
// view transition pseudo-element writes each item as a class. The other pseudos separate items with commas.
void serializeItems(PrTextBuffer* out, const PrCssSelectors* selectors,
        const CssSelectorArgument* argument, CssPseudoClass pseudoClass, CssPseudoElement pseudoElement) {
    if (argument->name == kCssSelectorAll)
        prTextBufferAppendChar(out, '*');
    else if (argument->name != kCssSelectorNone)
        cssSerializeIdent(out, cssSelectorName(selectors, argument->name));

    bool spaced = pseudoElement == CssPseudoElement_PartFn || pseudoElement == CssPseudoElement_PickerFn;
    bool classes = isNamedTransition(pseudoElement);
    bool numbers = pseudoClass == CssPseudoClass_HeadingFn;
    for (uint32_t i = 0; i < argument->itemCount; ++i) {
        if (classes)
            prTextBufferAppendChar(out, '.');
        else if (i != 0)
            prTextBufferAppend(out, spaced ? " " : ", ");
        if (numbers) {
            cssSerializeNumber(out, double(selectors->numbers[argument->items + i]));
        } else if (selectors->names[argument->items + i].string) {
            cssSerializeString(out, cssSelectorName(selectors, argument->items + i));
        } else {
            cssSerializeIdent(out, cssSelectorName(selectors, argument->items + i));
        }
    }
}

void serializeArgument(PrTextBuffer* out, const PrCssSelectors* selectors, uint32_t index,
        CssPseudoClass pseudoClass, CssPseudoElement pseudoElement) {
    const CssSelectorArgument* argument = &selectors->arguments[index];
    prTextBufferAppendChar(out, '(');
    if (isNth(pseudoClass)) {
        cssSerializeNth(out, argument->a, argument->b);
        if (argument->selectors.count != 0) {
            prTextBufferAppend(out, " of ");
            serializeList(out, selectors, argument->selectors);
        }
    } else if (argument->itemCount != 0 || argument->name != kCssSelectorNone) {
        serializeItems(out, selectors, argument, pseudoClass, pseudoElement);
    } else {
        serializeList(out, selectors, argument->selectors);
    }
    prTextBufferAppendChar(out, ')');
}

void serializeSimple(PrTextBuffer* out, const PrCssSelectors* selectors, const CssSimpleSelector* simple) {
    switch (simple->kind) {
        case CssSimpleKind_Type:
            serializeNamespace(out, selectors, simple);
            cssSerializeIdent(out, cssSelectorName(selectors, simple->name));
            break;
        case CssSimpleKind_Universal:
            serializeNamespace(out, selectors, simple);
            prTextBufferAppendChar(out, '*');
            break;
        case CssSimpleKind_Id:
            prTextBufferAppendChar(out, '#');
            cssSerializeIdent(out, cssSelectorName(selectors, simple->name));
            break;
        case CssSimpleKind_Class:
            prTextBufferAppendChar(out, '.');
            cssSerializeIdent(out, cssSelectorName(selectors, simple->name));
            break;
        case CssSimpleKind_Attribute: {
            prTextBufferAppendChar(out, '[');
            serializeNamespace(out, selectors, simple);
            cssSerializeIdent(out, cssSelectorName(selectors, simple->name));
            if (simple->match != CssAttributeMatch_Exists) {
                static constexpr PrStringView kMatchers[] = {"",   "=",  "~=", "|=", "^=", "$=", "*="};
                prTextBufferAppend(out, kMatchers[simple->match]);
                cssSerializeString(out, cssSelectorName(selectors, simple->value));
            }
            if (simple->letterCase == CssAttributeCase_Insensitive)
                prTextBufferAppend(out, " i");
            else if (simple->letterCase == CssAttributeCase_Sensitive)
                prTextBufferAppend(out, " s");
            prTextBufferAppendChar(out, ']');
            break;
        }
        case CssSimpleKind_PseudoClass:
            prTextBufferAppendChar(out, ':');
            prTextBufferAppend(out, kCssPseudoClassName[simple->pseudoClass]);
            if (simple->argument != kCssSelectorNone)
                serializeArgument(out, selectors, simple->argument, simple->pseudoClass, CssPseudoElement_None);
            break;
        case CssSimpleKind_PseudoElement:
            prTextBufferAppend(out, "::");
            if (simple->pseudoElement == CssPseudoElement_UnknownWebkit) {
                // selectors-4/#unknown--webkit--pseudo-elements: in ASCII lowercase.
                prTextBufferAppend(out, cssSelectorNameLowered(selectors, simple->name));
            } else {
                prTextBufferAppend(out, kCssPseudoElementName[simple->pseudoElement]);
            }
            if (simple->argument != kCssSelectorNone)
                serializeArgument(out, selectors, simple->argument, CssPseudoClass_None, simple->pseudoElement);
            break;
        case CssSimpleKind_Nesting:
            prTextBufferAppendChar(out, '&');
            break;
        default:
            break;
    }
}

// Writes a combinator other than the descendant one, without the spaces around it.
void serializeCombinatorSymbol(PrTextBuffer* out, const PrCssSelectors* selectors, const CssCompoundSelector* compound) {
    switch (compound->combinator) {
        case CssCombinator_Child: prTextBufferAppendChar(out, '>'); break;
        case CssCombinator_NextSibling: prTextBufferAppendChar(out, '+'); break;
        case CssCombinator_SubsequentSibling: prTextBufferAppendChar(out, '~'); break;
        case CssCombinator_Column: prTextBufferAppend(out, "||"); break;
        case CssCombinator_Reference: {
            const CssSimpleSelector* attribute = &selectors->simple[compound->reference];
            prTextBufferAppendChar(out, '/');
            serializeNamespace(out, selectors, attribute);
            cssSerializeIdent(out, cssSelectorName(selectors, attribute->name));
            prTextBufferAppendChar(out, '/');
            break;
        }
        default:
            break;
    }
}

// Writes a combinator between two spaces, or the single space that is the descendant combinator.
void serializeCombinator(PrTextBuffer* out, const PrCssSelectors* selectors, const CssCompoundSelector* compound) {
    prTextBufferAppendChar(out, ' ');
    if (compound->combinator == CssCombinator_Descendant)
        return;
    serializeCombinatorSymbol(out, selectors, compound);
    prTextBufferAppendChar(out, ' ');
}

// Serializes a complex selector (cssom-1/#serialize-a-selector). A relative selector with an implied `&` is written
// with the `&` (css-nesting-1/#cssom).
void serializeComplex(PrTextBuffer* out, const PrCssSelectors* selectors, const CssComplexSelector* complex) {
    if ((complex->flags & CssComplexFlag_Invalid) != 0) {
        prTextBufferAppend(out, cssSelectorName(selectors, complex->text));
        return;
    }
    uint32_t last = complex->compound + complex->compoundCount - 1;
    const CssCompoundSelector* leftmost = &selectors->compounds[last];
    if ((complex->flags & CssComplexFlag_ImpliedNesting) != 0) {
        prTextBufferAppendChar(out, '&');
        serializeCombinator(out, selectors, leftmost);
    } else if ((complex->flags & CssComplexFlag_Relative) != 0 && leftmost->combinator != CssCombinator_Descendant) {
        serializeCombinatorSymbol(out, selectors, leftmost);
        prTextBufferAppendChar(out, ' ');
    }

    RawArray<uint32_t> written = {};
    for (uint32_t i = last + 1; i-- > complex->compound;) {
        const CssCompoundSelector* compound = &selectors->compounds[i];
        if (i != last)
            serializeCombinator(out, selectors, compound);
        // The simple selectors in written order, in case testing reordered them.
        const CssSimpleSelector* simple = &selectors->simple[compound->simple];
        bool reordered = false;
        for (uint32_t j = 0; j < compound->simpleCount; ++j)
            reordered = reordered || simple[j].written != j;
        written.count = 0;
        if (reordered) {
            for (uint32_t j = 0; j < compound->simpleCount; ++j)
                written.push(0);
            for (uint32_t j = 0; j < compound->simpleCount; ++j)
                written.data[simple[j].written] = j;
        }
        // A universal selector with no prefix is omitted from a compound that holds more.
        for (uint32_t j = 0; j < compound->simpleCount; ++j) {
            const CssSimpleSelector* next = &simple[written.count != 0 ? written.data[j] : j];
            if (next->kind != CssSimpleKind_Universal || next->prefix != CssNamespacePrefix_None ||
                    compound->simpleCount == 1) {
                serializeSimple(out, selectors, next);
            }
        }
    }
    written.reset();
}

void serializeList(PrTextBuffer* out, const PrCssSelectors* selectors, CssSelectorList list) {
    for (uint32_t i = 0; i < list.count; ++i) {
        if (i != 0)
            prTextBufferAppend(out, ", ");
        serializeComplex(out, selectors, &selectors->complex[list.first + i]);
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Selector pools

void cssSelectorPoolReset(CssSelectorPool* pool) {
    pool->complex.reset();
    pool->compounds.reset();
    pool->simple.reset();
    pool->arguments.reset();
    pool->namespaces.reset();
    pool->names.reset();
    pool->numbers.reset();
    prTextBufferReset(&pool->text);
}

PrStringView cssPseudoName(CssPseudo pseudo) {
    switch (pseudo) {
        case CssPseudo_Before: return "::before";
        case CssPseudo_After: return "::after";
        case CssPseudo_Marker: return "::marker";
        case CssPseudo_FieldContent: return "::field-content";
        case CssPseudo_Placeholder: return "::placeholder";
        case CssPseudo_Selection: return "::selection";
        case CssPseudo_BeforeMarker: return "::before::marker";
        case CssPseudo_AfterMarker: return "::after::marker";
        case CssPseudo_FirstLine: return "::first-line";
        case CssPseudo_FirstLetter: return "::first-letter";
        default: return "";
    }
}

CssPseudo cssSelectorPseudo(const PrCssSelectors* selectors, uint32_t complex) {
    const CssComplexSelector* c = &selectors->complex[complex];
    if (c->pseudoElement == CssPseudoElement_None)
        return CssPseudo_None;
    // The subject compound's pseudo-elements keep their written order, after the selectors of the originating element.
    const CssCompoundSelector* subject = &selectors->compounds[c->compound];
    CssPseudoElement first = CssPseudoElement_None;
    CssPseudoElement second = CssPseudoElement_None;
    for (uint32_t k = 0; k < subject->simpleCount; ++k) {
        const CssSimpleSelector* simple = &selectors->simple[subject->simple + k];
        if (simple->kind == CssSimpleKind_PseudoElement) {
            if (second != CssPseudoElement_None)
                return CssPseudo_Count;
            (first == CssPseudoElement_None ? first : second) = simple->pseudoElement;
        } else if (first != CssPseudoElement_None) {
            return CssPseudo_Count;
        }
    }
    if (second == CssPseudoElement_None) {
        switch (first) {
            case CssPseudoElement_Before:
                return CssPseudo_Before;
            case CssPseudoElement_After:
                return CssPseudo_After;
            case CssPseudoElement_Marker:
                return CssPseudo_Marker;
            case CssPseudoElement_FirstLine:
                return CssPseudo_FirstLine;
            case CssPseudoElement_FirstLetter:
                return CssPseudo_FirstLetter;
            case CssPseudoElement_FieldContent:
                return CssPseudo_FieldContent;
            case CssPseudoElement_Placeholder:
                return CssPseudo_Placeholder;
            case CssPseudoElement_Selection:
                return CssPseudo_Selection;
            default:
                return CssPseudo_Count;
        }
    }
    if (second != CssPseudoElement_Marker)
        return CssPseudo_Count;
    if (first == CssPseudoElement_Before)
        return CssPseudo_BeforeMarker;
    return first == CssPseudoElement_After ? CssPseudo_AfterMarker : CssPseudo_Count;
}

void cssSelectorsDestroy(PrCssSelectors* PR_NULLABLE selectors) {
    prMemoryFree(selectors);
}

PrCssSelectors* cssSelectorsCopy(const PrCssSelectors* selectors) {
    char* memory = static_cast<char*>(prMemoryAlloc(selectors->size));
    prMemCopy(memory, selectors, selectors->size);
    // Each run moves with the allocation.
    PrCssSelectors* out = reinterpret_cast<PrCssSelectors*>(memory);
    auto moved = [&](auto* run) {
        return reinterpret_cast<decltype(run)>(memory +
            (reinterpret_cast<const char*>(run) - reinterpret_cast<const char*>(selectors)));
    };
    out->complex = moved(selectors->complex);
    out->compounds = moved(selectors->compounds);
    out->simple = moved(selectors->simple);
    out->arguments = moved(selectors->arguments);
    out->namespaces = moved(selectors->namespaces);
    out->names = moved(selectors->names);
    out->numbers = moved(selectors->numbers);
    out->text = moved(selectors->text);
    return out;
}

PrStringView cssSelectorName(const PrCssSelectors* selectors, uint32_t name) {
    const CssSelectorName* entry = &selectors->names[name];
    return PrStringView{selectors->text + entry->text, entry->size};
}

PrStringView cssSelectorNameLowered(const PrCssSelectors* selectors, uint32_t name) {
    const CssSelectorName* entry = &selectors->names[name];
    return PrStringView{selectors->text + entry->lowered, entry->size};
}

void cssSelectorsSpecify(PrCssSelectors* selectors, const PrCssSelectors* PR_NULLABLE parent) {
    // css-nesting-1/#nest-selector: `&` has the greatest specificity of the parent's selectors. With no parent, `&` is
    // :scope and counts as a pseudo-class.
    CssSpecificity nesting = parent != nullptr ? greatestSpecificity(parent, parent->list) : cssSpecificity(0, 1, 0);

    // From the last back, because an argument's selectors come after the selector whose argument they are.
    for (uint32_t i = selectors->complexCount; i-- > 0;) {
        CssComplexSelector* complex = &selectors->complex[i];
        Counts counts = {};
        for (uint32_t j = 0; j < complex->compoundCount; ++j) {
            const CssCompoundSelector* compound = &selectors->compounds[complex->compound + j];
            for (uint32_t k = 0; k < compound->simpleCount; ++k)
                addSimpleSpecificity(selectors, &selectors->simple[compound->simple + k], nesting, &counts);
        }
        if ((complex->flags & CssComplexFlag_ImpliedNesting) != 0)
            addSpecificity(&counts, nesting);
        else if ((complex->flags & CssComplexFlag_ImpliedScope) != 0)
            ++counts.classes;
        complex->specificity = cssSpecificity(counts.ids, counts.classes, counts.types);
    }
}

PrCssSelectors* PR_NULLABLE cssParseSelectors(CssParser* parser, const CssTokenStream* ts,
        CssTokenSpan span, const CssSelectorContext* context) {
    if (span.count == 0)
        return nullptr;

    // A sheet that declares a default namespace gives one to type and universal selectors. Reading directly leaves that
    // case to the grammar.
    PrStringView fallback;
    CssSelectorPool* pool = &parser->selectors;
    clearPool(pool);
    CssSelectorList list = {};
    if (context->nesting == CssSelectorNesting_None && (context->sheet == nullptr ||
            !cssSheetNamespace(context->sheet, PrStringView{}, &fallback)) &&
            directList(pool, ts->tokens, span, parser->match.engine, &list)) {
        PrCssSelectors* read = hold(pool, list);
        cssSelectorsSpecify(read, context->parent);
#ifndef NDEBUG
        PrCssSelectors* compiled = parseCompiled(parser, ts, span, context);
        PR_ASSERT(compiled != nullptr && selectorsEquals(read, compiled));
        cssSelectorsDestroy(compiled);
#endif
        return read;
    }
    return parseCompiled(parser, ts, span, context);
}

PrCssSelectors* PR_NULLABLE cssParseSelectorText(CssParser* parser, PrStringView text, const CssSelectorContext* context) {
    CssTokenStream ts = cssParserTokenize(parser, text);
    return cssParseSelectors(parser, &ts, cssStreamTrimmed(&ts), context);
}

bool cssIsComplexSelectorNode(const CssValueNode* value) {
    return cssValueIsProduction(value, CssValueDef_ComplexSelector) ||
        cssValueIsProduction(value, CssValueDef_ComplexRealSelector) ||
        cssValueIsProduction(value, CssValueDef_RelativeSelector) ||
        cssValueIsProduction(value, CssValueDef_RelativeRealSelector);
}

PrCssSelectors* PR_NULLABLE cssCompilePreludeSelectors(CssParser* parser, CssValueSpan span,
        const CssSheetData* PR_NULLABLE sheet) {
    CssSelectorContext context = {};
    context.sheet = sheet;
    CssSelectorPool* pool = &parser->selectors;
    clearPool(pool);
    Compiler c = {.tree = &parser->pool, .context = &context, .pool = pool, .nesting = false, .matchesAny = false};
    uint32_t end = span.start + span.count;
    CssSelectorList list = {pool->complex.count, 0};
    // A selector's own subtree holds the selectors of its arguments, which it compiles.
    for (uint32_t i = span.start; i < end; ++i) {
        if (cssIsComplexSelectorNode(nodeAt(&c, i))) {
            ++list.count;
            i = after(&c, i) - 1;
        }
    }
    if (list.count == 0)
        return nullptr;
    for (uint32_t i = 0; i < list.count; ++i)
        pool->complex.push(CssComplexSelector{});
    uint32_t slot = list.first;
    for (uint32_t i = span.start; i < end; ++i) {
        if (cssIsComplexSelectorNode(nodeAt(&c, i))) {
            compileComplex(&c, i, slot++, false);
            i = after(&c, i) - 1;
        }
    }
    return hold(pool, list);
}

void cssSerializeSelectors(PrTextBuffer* out, const PrCssSelectors* selectors) {
    serializeList(out, selectors, selectors->list);
}

void cssSerializeSelectorRange(PrTextBuffer* out, const PrCssSelectors* selectors, uint32_t first, uint32_t count) {
    serializeList(out, selectors, CssSelectorList{selectors->list.first + first, count});
}

} // namespace pr_impl
