// The Cascade
//
// css-cascade-5 - https://drafts.csswg.org/css-cascade-5/
// css-cascade-6 - https://drafts.csswg.org/css-cascade-6/
// css-variables-1 - https://drafts.csswg.org/css-variables-1/
// css-values-5 - https://drafts.csswg.org/css-values-5/
// css-display-4 - https://drafts.csswg.org/css-display-4/
// css-nesting-1 - https://drafts.csswg.org/css-nesting-1/

#include "css/src/css_cascade.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/bit_cast.h"
#include "pr/count_of.h"
#include "pr/hash.h"
#include "pr/mem.h"
#include "pr/name_table.h"
#include "pr/sort.h"
#include "pr/string_ops.h"
#include "pr/url.h"

#include "css/src/css_at_rule.h"
#include "css/src/css_hints.h"
#include "css/src/css_inline_style.h"
#include "css/src/css_object.h"
#include "css/src/css_parse.h"
#include "css/src/css_parser.h"
#include "css/src/css_root.h"
#include "css/src/css_serialize.h"
#include "css/src/css_supports.h"
#include "css/src/css_syntax_definition.h"
#include "css/src/css_unit.h"
#include "dom/src/dom_class.h"
#include "dom/src/dom_element_state.h"
#include "dom/src/dom_slot.h"
#include "dom/src/dom_use.h"

namespace pr_impl {

namespace {

// The seed that a feature's name is hashed with, offset by the feature's kind.
constexpr uint64_t kFeatureSeed = 0x510e527fade682d1ull;

// The maximum number of bytes that a value may hold after its var() references are substituted. Past this limit the
// value is invalid at computed-value time, because nested references can grow it exponentially
// (css-values-5/#long-substitution).
constexpr uint32_t kMaxSubstitution = 1u << 20;

// Slack for flattening custom property sets. A new set flattens into a base of its own when twice the number of its
// entries above the base exceeds the base's count plus this slack. Past that point, layering no longer saves copying.
constexpr uint32_t kCustomFlattenSlack = 32;

// The seed that the entries an element matched are hashed with, to find the style cached for them.
constexpr uint64_t kMatchedSeed = 0x9b05688c2b3e6c1full;

// The arbitrary substitution functions (css-values-5/#arbitrary-substitution).
enum {
    Substitution_None,
    Substitution_Var,               // var() (css-variables-1/#using-variables)
    Substitution_Attr,              // attr() (css-values-5/#attr-notation)
};
typedef uint8_t Substitution;

// Returns the arbitrary substitution function whose name and opening parenthesis start at `i` of `text`, and that is
// not the end of a longer name, and writes the length of the name and parenthesis to `*length`.
Substitution substitutionAt(PrStringView text, size_t i, size_t* length) {
    char before = i != 0 ? text.data[i - 1] : ' ';
    if (prAsciiIsLetter(before) || prAsciiIsDigit(before) || before == '-')
        return Substitution_None;
    auto named = [&](PrStringView name) {
        return i + name.size < text.size && prAsciiEqualsFold(PrStringView{text.data + i, name.size}, name) &&
            text.data[i + name.size] == '(';
    };
    if (named("var")) {
        *length = 4;
        return Substitution_Var;
    }
    if (named("attr")) {
        *length = 5;
        return Substitution_Attr;
    }
    return Substitution_None;
}

// Returns true if a declaration of the block sets content, directly or through a shorthand such as `all`.
bool blockSetsContent(const CssDeclarationBlock* block) {
    const CssDeclaration* declarations = cssDeclarations(block);
    for (uint32_t k = 0; k < block->count; ++k) {
        PrCssProperty property = declarations[k].property;
        if (property == PrCssProperty_Content)
            return true;
        if (property >= PrCssProperty_Count)
            continue;
        CssPropertySpan longhands = kCssPropertyLonghands[property];
        for (uint32_t l = 0; l < longhands.count; ++l) {
            if (kCssPropertyRuns[longhands.start + l] == PrCssProperty_Content)
                return true;
        }
    }
    return false;
}

void file(CssRuleIndex& filed, uint32_t hash, uint32_t entry) {
    uint32_t at = filed.index.find(hash, [&](uint32_t i) { return filed.buckets.data[i].hash == hash; });
    if (at == kHashMissing) {
        at = filed.buckets.count;
        CssRuleBucket& bucket = filed.buckets.emplace();
        bucket = CssRuleBucket{};
        bucket.hash = hash;
        filed.index.insert(hash, at);
    }
    filed.buckets.data[at].entries.push(entry);
}

// The state that filing rules keeps as it walks them.
struct Filing {
    CssRuleSet* set;
    const PrDomDocument* document;
    const CssMediaEnvironment* PR_NULLABLE media;
    CssOrigin origin;
    uint32_t order;
    uint32_t anonymous;             // number of anonymous layers named so far
    const CssSheetData* sheet;      // the sheet whose rules are being filed
    PrTextBuffer name = {};
    RawArray<uint32_t> types;          // scratch: the types that a subject lists (typeAlternatives)

    // The tree whose sheets are being filed, or null for the user agent's sheets, and its depth.
    const PrDomNode* PR_NULLABLE scope;
    uint32_t depth;

    CssAppearance appearance;       // the used appearance that the sheet being filed applies under, or None for any
};

// Returns the index of the layer that `name` names, and appends the layer in layer order if it is new.
uint16_t layerIndex(Filing& f, PrStringView name) {
    for (uint32_t i = 0; i < f.set->layers.count; ++i) {
        if (prRawStringView(&f.set->layers.data[i]) == name)
            return uint16_t(i);
    }
    f.set->layers.push(prRawStringCopy(name));
    return uint16_t(f.set->layers.count - 1);
}

// Returns true if a compound matches across a shadow boundary, so that its element is not among the ancestors that a
// descendant or child combinator walks.
bool crossesShadow(const PrCssSelectors* selectors, const CssCompoundSelector* compound) {
    for (uint32_t k = 0; k < compound->simpleCount; ++k) {
        const CssSimpleSelector* simple = &selectors->simple[compound->simple + k];
        if (simple->kind == CssSimpleKind_PseudoElement)
            return true;
        if (simple->kind == CssSimpleKind_PseudoClass &&
                (simple->pseudoClass == CssPseudoClass_Host || simple->pseudoClass == CssPseudoClass_HostFn ||
                    simple->pseudoClass == CssPseudoClass_HostContextFn))
            return true;
    }
    return false;
}

// Returns the bucket hash of the entries whose subject's most selective feature is an attribute with the
// domAttributeHash `hash`. An element's attributes find this bucket without lowering their names.
uint32_t attributeBucket(uint32_t hash) {
    uint32_t bucket = (hash ^ uint32_t(CssFeature_Attribute)) * 0x9e3779b1u;
    return bucket != 0 ? bucket : 1;
}

// Returns the feature hash of a simple selector, or 0 if the ancestor filter does not track its kind of feature.
uint32_t ancestorFeature(const PrCssSelectors* selectors, const CssSimpleSelector* simple) {
    switch (simple->kind) {
        case CssSimpleKind_Type:
            if (simple->name != kCssSelectorAll)
                return cssFeatureHash(CssFeature_Type, cssSelectorNameLowered(selectors, simple->name));
            return 0;
        case CssSimpleKind_Id:
            return cssFeatureHash(CssFeature_Id, cssSelectorNameLowered(selectors, simple->name));
        case CssSimpleKind_Class:
            return cssFeatureHash(CssFeature_Class, cssSelectorNameLowered(selectors, simple->name));
        case CssSimpleKind_Attribute:
            return attributeBucket(selectors->names[simple->name].hash);
        default:
            return 0;
    }
}

// Calls `visit` with the name of the attribute that each attr() in the block's declarations names, or with the empty
// view for an attr() whose name another function gives.
template <typename Visit>
void eachAttrRead(const CssDeclarationBlock* block, Visit visit) {
    const CssDeclaration* declarations = cssDeclarations(block);
    for (uint32_t i = 0; i < block->count; ++i) {
        // A value that holds an attr() is kept as tokens, which are one value.
        if (declarations[i].valueCount != 1)
            continue;
        CssValueRun run = cssDeclarationValue(block, &declarations[i]);
        if (run.count != 1 || run.values[0].kind != PrCssValueKind_Tokens ||
                (run.values[0].aux & CssValueAuxFlag_ReadsAttributes) == 0)
            continue;
        PrStringView text = cssValueText(run, run.values);
        for (size_t at = 0; at < text.size; ++at) {
            size_t length = 0;
            if (prAsciiLower(text.data[at]) != 'a' || substitutionAt(text, at, &length) != Substitution_Attr)
                continue;
            size_t start = at + length;
            while (start < text.size && (prAsciiIsSpace(text.data[start]) || text.data[start] == '|'))
                ++start;
            auto isNameByte = [](char c) {
                return prAsciiIsLetter(c) || prAsciiIsDigit(c) || c == '-' || c == '_' || uint8_t(c) >= 0x80;
            };
            size_t end = start;
            while (end < text.size && isNameByte(text.data[end]))
                ++end;
            visit(PrStringView{text.data + start, end - start});
        }
    }
}

// Writes to `types` the types of the elements that can match `pseudo`, and returns their number. Returns 0 if elements
// of any type can match.
uint32_t pseudoClassTypes(CssPseudoClass pseudo, const PrStringView* PR_NULLABLE* PR_NONNULL types) {
    static constexpr PrStringView kHeadings[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
    static constexpr PrStringView kLinks[] = {"a", "area"};
    static constexpr PrStringView kAutofill[] = {"input"};
    switch (pseudo) {
        case CssPseudoClass_Heading:
        case CssPseudoClass_HeadingFn:
            *types = kHeadings;
            return countOf(kHeadings);
        case CssPseudoClass_Link:
        case CssPseudoClass_Visited:
        case CssPseudoClass_AnyLink:
            *types = kLinks;
            return countOf(kLinks);
        case CssPseudoClass_Autofill:
            *types = kAutofill;
            return countOf(kAutofill);
        default:
            return 0;
    }
}

// Appends to `out` the feature hash of each type that the compound requires, and returns true. A compound requires one
// of several types if it has an :is() or :where() whose arguments are all type selectors alone, as `:is(dir, menu, ul)`
// does. It also requires one of several types if it has a pseudo-class that only those types match, as :heading does.
// Returns false, with nothing appended, if the compound requires no such types.
bool typeAlternatives(const PrCssSelectors* selectors, const CssCompoundSelector* compound, RawArray<uint32_t>& out) {
    for (uint32_t k = 0; k < compound->simpleCount; ++k) {
        const CssSimpleSelector* simple = &selectors->simple[compound->simple + k];
        const PrStringView* types = nullptr;
        uint32_t count = simple->kind == CssSimpleKind_PseudoClass ? pseudoClassTypes(simple->pseudoClass, &types) : 0;
        if (count != 0) {
            for (uint32_t i = 0; i < count; ++i)
                out.push(cssFeatureHash(CssFeature_Type, types[i]));
            return true;
        }
    }
    for (uint32_t k = 0; k < compound->simpleCount; ++k) {
        const CssSimpleSelector* simple = &selectors->simple[compound->simple + k];
        if (simple->kind != CssSimpleKind_PseudoClass || simple->argument == kCssSelectorNone ||
                (simple->pseudoClass != CssPseudoClass_IsFn && simple->pseudoClass != CssPseudoClass_WhereFn))
            continue;
        const CssSelectorList& list = selectors->arguments[simple->argument].selectors;
        bool types = list.count != 0;
        for (uint32_t i = 0; i < list.count && types; ++i) {
            const CssComplexSelector* c = &selectors->complex[list.first + i];
            const CssCompoundSelector* only = &selectors->compounds[c->compound];
            const CssSimpleSelector* type = &selectors->simple[only->simple];
            types = (c->flags & CssComplexFlag_Invalid) == 0 && c->compoundCount == 1 && only->simpleCount == 1 &&
                type->kind == CssSimpleKind_Type && type->name != kCssSelectorAll;
        }
        if (!types)
            continue;
        for (uint32_t i = 0; i < list.count; ++i) {
            const CssComplexSelector* c = &selectors->complex[list.first + i];
            const CssSimpleSelector* type = &selectors->simple[selectors->compounds[c->compound].simple];
            out.push(cssFeatureHash(CssFeature_Type, cssSelectorNameLowered(selectors, type->name)));
        }
        return true;
    }
    return false;
}

// Writes to `out` up to kCssAncestorHashes features that the ancestors of an element matching `c` carry. The features
// come from the compounds that `c` reaches from its subject through descendant and child combinators alone. Ids,
// classes and attributes come before types, because an element's ancestors carry fewer of them. A selector that is
// matched relative to something other than the element's ancestors, such as a nested or relative selector, keeps none.
// Also writes to `anyOf` the types of the first of those compounds that requires one of several types
// (typeAlternatives), if it requires no more of them than kCssAncestorAlternatives. `types` is scratch space.
void collectAncestors(const PrCssSelectors* selectors, const CssComplexSelector* c, uint32_t* out, uint32_t* anyOf,
        RawArray<uint32_t>& types) {
    if ((c->flags & (CssComplexFlag_Relative | CssComplexFlag_Nesting | CssComplexFlag_ImpliedScope)) != 0)
        return;
    const CssCompoundSelector* compounds = selectors->compounds + c->compound;
    uint32_t reached = 0;
    while (reached + 1 < c->compoundCount) {
        CssCombinator combinator = compounds[reached].combinator;
        if ((combinator != CssCombinator_Descendant && combinator != CssCombinator_Child) ||
                crossesShadow(selectors, &compounds[reached + 1]))
            break;
        ++reached;
    }
    uint32_t n = 0;
    for (uint32_t pass = 0; pass < 2; ++pass) {
        for (uint32_t k = 1; k <= reached && n < kCssAncestorHashes; ++k) {
            const CssCompoundSelector* ancestor = &compounds[k];
            for (uint32_t i = 0; i < ancestor->simpleCount && n < kCssAncestorHashes; ++i) {
                const CssSimpleSelector* simple = &selectors->simple[ancestor->simple + i];
                // The first pass takes the features other than types, and the second takes the types.
                if ((simple->kind == CssSimpleKind_Type) != (pass == 1))
                    continue;
                uint32_t hash = ancestorFeature(selectors, simple);
                if (hash != 0)
                    out[n++] = hash;
            }
        }
    }
    for (uint32_t k = 1; k <= reached; ++k) {
        types.clear();
        if (!typeAlternatives(selectors, &compounds[k], types))
            continue;
        if (types.count <= kCssAncestorAlternatives)
            prMemCopy(anyOf, types.data, sizeof(uint32_t) * types.count);
        break;
    }
    types.clear();
}

// Returns an entry's rank (CssRuleEntry::rank), in the order in which normal declarations apply: by origin, then by
// encapsulation context, then by layer and specificity. A normal declaration of an outer context wins over one of an
// inner context (css-cascade-5/#cascade-context), so the deeper context applies first. Important declarations apply in
// reverse (applyOrigin).
uint64_t rankOf(CssOrigin origin, uint32_t depth, uint16_t layer, CssSpecificity specificity) {
    uint64_t inner = depth < 255 ? depth : 255;
    return uint64_t(origin) << 56 | (255 - inner) << 48 | uint64_t(layer) << 32 | specificity;
}

// Returns true if the subject compound of `c` ends in ::slotted() and holds no other pseudo-element.
bool endsInSlotted(const PrCssSelectors* selectors, const CssComplexSelector* c) {
    const CssCompoundSelector* subject = &selectors->compounds[c->compound];
    if (subject->simpleCount == 0)
        return false;
    for (uint32_t k = 0; k < subject->simpleCount; ++k) {
        const CssSimpleSelector* simple = &selectors->simple[subject->simple + k];
        if (simple->kind != CssSimpleKind_PseudoElement)
            continue;
        if (k + 1 != subject->simpleCount || simple->pseudoElement != CssPseudoElement_SlottedFn)
            return false;
    }
    return selectors->simple[subject->simple + subject->simpleCount - 1].kind == CssSimpleKind_PseudoElement;
}

// Returns true if the block declares `appearance` or a custom property, which the cascade reads to decide an element's
// used appearance.
bool decidesAppearance(const CssDeclarationBlock* block) {
    const CssDeclaration* declarations = cssDeclarations(block);
    for (uint32_t i = 0; i < block->count; ++i) {
        const CssDeclaration* d = &declarations[i];
        if (d->descriptor != PrCssDescriptor_None)
            continue;
        if (d->property == PrCssProperty_Count || d->property == PrCssProperty_Appearance)
            return true;
        CssPropertySpan longhands = kCssPropertyLonghands[d->property];
        for (uint32_t k = 0; k < longhands.count; ++k) {
            if (kCssPropertyRuns[longhands.start + k] == PrCssProperty_Appearance)
                return true;
        }
    }
    return false;
}

// Files each complex selector of `selectors` with the declarations of `block`, among the entries of what it styles. A
// selector is filed under the first of these that its subject has: its id, a class, an attribute that it names, or its
// type. An attribute comes before the type because fewer elements carry it. A subject with none of them may require one
// of several types (typeAlternatives). It is then filed under each type. An element has one type, so it finds the entry
// once. A selector with none of these is filed as universal.
void fileSelectors(Filing& f, const PrCssSelectors* selectors, uint32_t matcher, const CssDeclarationBlock* block,
        uint16_t layer) {
    CssRuleSet* set = f.set;
    PR_ASSERT(f.appearance == CssAppearance_None || !decidesAppearance(block));
    bool attributes = false;
    eachAttrRead(block, [&](PrStringView) { attributes = true; });
    // Whether the declarations set content, found once the block files a pseudo-element's selector.
    int8_t generates = -1;
    for (uint32_t i = 0; i < selectors->list.count; ++i) {
        uint32_t complex = selectors->list.first + i;
        const CssComplexSelector* c = &selectors->complex[complex];
        CssPseudo pseudo = (c->flags & CssComplexFlag_Invalid) != 0 ? CssPseudo_Count : cssSelectorPseudo(selectors, complex);
        // A selector of ::slotted() styles the elements that a slot of its tree is assigned.
        bool slotted = pseudo == CssPseudo_Count && (c->flags & CssComplexFlag_Invalid) == 0 && f.scope != nullptr &&
            endsInSlotted(selectors, c);
        if (slotted)
            pseudo = CssPseudo_None;
        if (pseudo == CssPseudo_Count)
            continue;
        CssRuleEntry entry{
            .selectors = selectors,
            .block = block,
            .complex = complex,
            .matcher = matcher,
            .specificity = c->specificity,
            .order = f.order++,
            .layer = layer,
            .origin = f.origin,
            .attributes = attributes,
            .pseudo = pseudo,
            .appearance = f.appearance,
            .sheet = f.sheet,
            .scope = f.scope,
            .depth = f.depth,
            .rank = rankOf(f.origin, f.depth, layer, c->specificity),
            .ancestors = {},
            .anyAncestor = {},
        };
        collectAncestors(selectors, c, entry.ancestors, entry.anyAncestor, f.types);
        uint32_t at = set->entries.count;
        set->entries.push(entry);
        if (slotted) {
            set->slotted.push(at);
            continue;
        }
        const CssCompoundSelector* subject = &selectors->compounds[c->compound];
        uint32_t hash = 0;
        for (uint32_t k = 0; k < subject->simpleCount && hash == 0; ++k) {
            const CssSimpleSelector* simple = &selectors->simple[subject->simple + k];
            if (simple->kind == CssSimpleKind_Id)
                hash = cssFeatureHash(CssFeature_Id, cssSelectorNameLowered(selectors, simple->name));
        }
        for (uint32_t k = 0; k < subject->simpleCount && hash == 0; ++k) {
            const CssSimpleSelector* simple = &selectors->simple[subject->simple + k];
            if (simple->kind == CssSimpleKind_Class)
                hash = cssFeatureHash(CssFeature_Class, cssSelectorNameLowered(selectors, simple->name));
        }
        for (uint32_t k = 0; k < subject->simpleCount && hash == 0; ++k) {
            const CssSimpleSelector* simple = &selectors->simple[subject->simple + k];
            if (simple->kind == CssSimpleKind_Attribute)
                hash = attributeBucket(selectors->names[simple->name].hash);
        }
        for (uint32_t k = 0; k < subject->simpleCount && hash == 0; ++k) {
            const CssSimpleSelector* simple = &selectors->simple[subject->simple + k];
            if (simple->kind == CssSimpleKind_Type && simple->name != kCssSelectorAll)
                hash = cssFeatureHash(CssFeature_Type, cssSelectorNameLowered(selectors, simple->name));
        }
        f.types.clear();
        bool ends = pseudo == CssPseudo_Before || pseudo == CssPseudo_After;
        if (ends && generates < 0)
            generates = blockSetsContent(block) ? 1 : 0;
        CssRuleIndex* indexes[] = {&set->filed[pseudo], ends && generates > 0 ? &set->generating : nullptr};
        for (CssRuleIndex* filed : indexes) {
            if (filed == nullptr)
                continue;
            if (hash != 0) {
                file(*filed, hash, at);
            } else if (typeAlternatives(selectors, subject, f.types)) {
                for (uint32_t t = 0; t < f.types.count; ++t)
                    file(*filed, f.types.data[t], at);
            } else {
                filed->universal.push(at);
            }
        }
    }
}

// Where a compound sits in its complex selector, which decides what a change to one of its features can restyle.
struct Reach {
    bool self;
    bool laterSiblings;
    bool siblings;
    bool subtree;
    uint32_t descendant;            // the feature of the descendants restyled, or 0 for none
};

// Every reach at once: what a change can restyle if the selector's shape does not say.
constexpr Reach kReachAll{true, true, true, true, 0};

Reach unite(Reach a, const Reach& b) {
    a.self |= b.self;
    a.laterSiblings |= b.laterSiblings;
    a.siblings |= b.siblings;
    a.subtree |= b.subtree;
    if (a.descendant == 0)
        a.descendant = b.descendant;
    else if (b.descendant != 0 && b.descendant != a.descendant)
        a.subtree = true;
    return a;
}

// Returns the index of the invalidation of the feature that hashes to `hash`, or kHashMissing.
uint32_t invalidationAt(const CssRuleSet* set, uint32_t hash) {
    return set->invalidationIndex.find(hash, [&](uint32_t i) { return set->invalidations.data[i].hash == hash; });
}

// Returns the index of the invalidation of the feature that hashes to `hash` for elements that carry `qualifier`, and
// creates it if the set has none.
uint32_t invalidationFor(CssRuleSet* set, uint32_t hash, uint32_t qualifier) {
    uint32_t head = invalidationAt(set, hash);
    uint32_t at = head;
    while (at != kHashMissing && set->invalidations.data[at].qualifier != qualifier)
        at = set->invalidations.data[at].next;
    if (at != kHashMissing)
        return at;
    at = set->invalidations.count;
    CssInvalidation& made = set->invalidations.emplace();
    made = CssInvalidation{};
    made.hash = hash;
    made.qualifier = qualifier;
    made.next = kHashMissing;
    if (head == kHashMissing) {
        set->invalidationIndex.insert(hash, at);
    } else {
        made.next = set->invalidations.data[head].next;
        set->invalidations.data[head].next = at;
    }
    return at;
}

// Records that a change to the feature that hashes to `hash` restyles what `reach` names. If `qualifier` is not 0, the
// record applies only to a change on an element that carries `qualifier`.
void invalidate(CssRuleSet* set, uint32_t hash, const Reach& reach, uint32_t qualifier = 0) {
    uint32_t at = invalidationFor(set, hash, qualifier);
    CssInvalidation& invalidation = set->invalidations.data[at];
    invalidation.self |= reach.self;
    invalidation.laterSiblings |= reach.laterSiblings;
    invalidation.siblings |= reach.siblings;
    invalidation.subtree |= reach.subtree;
    if (invalidation.subtree) {
        invalidation.descendants.reset();
        return;
    }
    if (reach.descendant == 0)
        return;
    for (uint32_t i = 0; i < invalidation.descendants.count; ++i) {
        if (invalidation.descendants.data[i] == reach.descendant)
            return;
    }
    invalidation.descendants.push(reach.descendant);
}

// Records that a change to the feature that hashes to `hash` can change whether `anchor` matches a :has() argument.
void invalidateAnchor(CssRuleSet* set, uint32_t hash, const CssHasAnchor& anchor) {
    uint32_t at = invalidationFor(set, hash, 0);
    cssHasAnchorsAdd(&set->invalidations.data[at].anchors, anchor);
    cssHasAnchorsAdd(&set->anchors, anchor);
}

void invalidateAttribute(CssRuleSet* set, const char* name, const Reach& reach) {
    invalidate(set, cssFeatureHash(CssFeature_Attribute, name), reach);
}

// Returns the most selective feature of a compound that an element carries and that a descendant invalidation tests
// for, or 0 for a compound with none.
uint32_t subjectFeature(const PrCssSelectors* selectors, const CssCompoundSelector* compound) {
    const CssFeature order[] = {CssFeature_Id, CssFeature_Class, CssFeature_Type, CssFeature_Attribute};
    const CssSimpleKind kinds[] = {CssSimpleKind_Id, CssSimpleKind_Class, CssSimpleKind_Type, CssSimpleKind_Attribute};
    for (uint32_t f = 0; f < 4; ++f) {
        for (uint32_t k = 0; k < compound->simpleCount; ++k) {
            const CssSimpleSelector* simple = &selectors->simple[compound->simple + k];
            if (simple->kind == kinds[f] && simple->name != kCssSelectorAll)
                return cssFeatureHash(order[f], cssSelectorNameLowered(selectors, simple->name));
        }
    }
    return 0;
}

void collectComplex(CssRuleSet* set, const PrCssSelectors* selectors, uint32_t complex, const Reach* PR_NULLABLE outer);

// Records `anchor` for every feature of a :has() argument's complex selector, at any depth. A change to such a feature
// on the anchor's descendants or later siblings can change whether the anchor matches.
void collectHas(CssRuleSet* set, const PrCssSelectors* selectors, uint32_t complex, const CssHasAnchor& anchor) {
    const CssComplexSelector* c = &selectors->complex[complex];
    if ((c->flags & CssComplexFlag_Invalid) != 0)
        return;
    // Types and structure change only as children are added and removed, which reaches every anchor.
    cssHasAnchorsAdd(&set->anchors, anchor);
    for (uint32_t i = 0; i < c->compoundCount; ++i) {
        const CssCompoundSelector* compound = &selectors->compounds[c->compound + i];
        if (compound->combinator == CssCombinator_Column || compound->combinator == CssCombinator_Reference)
            set->reads.global = true;
        for (uint32_t k = 0; k < compound->simpleCount; ++k) {
            const CssSimpleSelector* simple = &selectors->simple[compound->simple + k];
            switch (simple->kind) {
                case CssSimpleKind_Id:
                    invalidateAnchor(set, cssFeatureHash(CssFeature_Id, cssSelectorNameLowered(selectors,
                        simple->name)), anchor);
                    continue;
                case CssSimpleKind_Class:
                    invalidateAnchor(set,
                        cssFeatureHash(CssFeature_Class, cssSelectorNameLowered(selectors, simple->name)), anchor);
                    continue;
                case CssSimpleKind_Attribute:
                    invalidateAnchor(set, cssFeatureHash(CssFeature_Attribute, cssSelectorNameLowered(selectors,
                        simple->name)), anchor);
                    continue;
                case CssSimpleKind_PseudoClass:
                    break;
                default:
                    continue;
            }
            switch (simple->pseudoClass) {
                case CssPseudoClass_Hover:
                case CssPseudoClass_Active:
                case CssPseudoClass_Focus:
                case CssPseudoClass_FocusVisible:
                case CssPseudoClass_FocusWithin:
                case CssPseudoClass_Target:
                    invalidateAnchor(set, cssFeatureHash(CssFeature_PseudoClass,
                        kCssPseudoClassName[simple->pseudoClass]), anchor);
                    break;
                case CssPseudoClass_IsFn:
                case CssPseudoClass_WhereFn:
                case CssPseudoClass_NotFn:
                case CssPseudoClass_HasFn:
                case CssPseudoClass_Root:
                case CssPseudoClass_Scope:
                    break;
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
                case CssPseudoClass_Empty:
                    // A child added or removed reaches the anchors through CssRuleSet::anchors.
                    set->reads.structure = true;
                    break;
                case CssPseudoClass_LangFn:
                case CssPseudoClass_DirFn: {
                    // An element takes its language and direction from its ancestors, so a change to either reaches
                    // every element under it. Those elements restyle whole, which matches the anchors among them again.
                    // The anchors above the change restyle as they do for any feature. xml:lang is identified by its
                    // local name, which is lang.
                    bool lang = simple->pseudoClass == CssPseudoClass_LangFn;
                    uint32_t hash = cssFeatureHash(CssFeature_Attribute, lang ? "lang" : "dir");
                    invalidate(set, hash, kReachAll);
                    invalidateAnchor(set, hash, anchor);
                    if (!lang)
                        set->reads.direction = true;
                    break;
                }
                default:
                    // A state that attributes decide, or the element's place among links.
                    set->reads.states = true;
                    invalidateAnchor(set, cssFeatureHash(CssFeature_State, ""), anchor);
                    break;
            }
            if (simple->argument == kCssSelectorNone)
                continue;
            const CssSelectorList& list = selectors->arguments[simple->argument].selectors;
            for (uint32_t a = 0; a < list.count; ++a)
                collectHas(set, selectors, list.first + a, anchor);
        }
    }
}

// Returns the best feature of a compound that an anchor carries, or 0. This is the feature that subjectFeature finds.
uint32_t compoundFeature(const PrCssSelectors* selectors, const CssCompoundSelector* compound) {
    return subjectFeature(selectors, compound);
}

// Records what a change to each feature of a compound restyles, and what else its selectors read.
void collectCompound(CssRuleSet* set, const PrCssSelectors* selectors, const CssCompoundSelector* compound, const Reach& reach) {
    CssRuleReads* reads = &set->reads;
    for (uint32_t k = 0; k < compound->simpleCount; ++k) {
        const CssSimpleSelector* simple = &selectors->simple[compound->simple + k];
        switch (simple->kind) {
            case CssSimpleKind_Id:
                invalidate(set, cssFeatureHash(CssFeature_Id, cssSelectorNameLowered(selectors, simple->name)), reach);
                continue;
            case CssSimpleKind_Class:
                invalidate(set, cssFeatureHash(CssFeature_Class, cssSelectorNameLowered(selectors, simple->name)), reach);
                continue;
            case CssSimpleKind_Attribute:
                invalidate(set, cssFeatureHash(CssFeature_Attribute, cssSelectorNameLowered(selectors, simple->name)), reach);
                continue;
            case CssSimpleKind_PseudoClass:
                break;
            case CssSimpleKind_PseudoElement:
                // The argument of ::slotted() reads the slotted element itself.
                if (simple->pseudoElement == CssPseudoElement_SlottedFn && simple->argument != kCssSelectorNone) {
                    const CssSelectorList& slotted = selectors->arguments[simple->argument].selectors;
                    Reach slottedReach{true, false, false, false, 0};
                    for (uint32_t i = 0; i < slotted.count; ++i)
                        collectComplex(set, selectors, slotted.first + i, &slottedReach);
                }
                continue;
            default:
                continue;
        }
        // The selectors of a pseudo-class's argument restyle what the pseudo-class's own place restyles.
        const CssSelectorList* list =
            simple->argument != kCssSelectorNone ? &selectors->arguments[simple->argument].selectors : nullptr;
        Reach argument = reach;
        switch (simple->pseudoClass) {
            case CssPseudoClass_Hover:
            case CssPseudoClass_Active:
            case CssPseudoClass_Focus:
            case CssPseudoClass_FocusVisible:
            case CssPseudoClass_FocusWithin:
            case CssPseudoClass_Target:
                // A state matches only if the rest of the compound matches, so only an element that carries the
                // compound's feature can change what the state reaches.
                invalidate(set, cssFeatureHash(CssFeature_PseudoClass, kCssPseudoClassName[simple->pseudoClass]), reach,
                    compoundFeature(selectors, compound));
                break;
            case CssPseudoClass_IsFn:
            case CssPseudoClass_WhereFn:
            case CssPseudoClass_NotFn:
            case CssPseudoClass_Root:
            case CssPseudoClass_Scope:
            case CssPseudoClass_Host:
            case CssPseudoClass_HostFn:
                break;
            case CssPseudoClass_HostContextFn:
                // The argument reads the host's shadow-including ancestors, whose change can reach anything below.
                argument = unite(reach, kReachAll);
                break;
            case CssPseudoClass_HasFn: {
                // A feature that the argument reads restyles the anchors that a change to it can make match, as this
                // compound's place says: the ancestors, and also the earlier siblings for an argument that reaches
                // siblings.
                bool previous = false;
                for (uint32_t i = 0; list != nullptr && i < list->count; ++i)
                    previous = previous || selectors->complex[list->first + i].reach != CssReach_Descendant;
                CssHasAnchor anchor{
                    compoundFeature(selectors, compound), reach.self, reach.laterSiblings,
                    reach.siblings,
                    reach.subtree, reach.descendant, previous,
                };
                for (uint32_t i = 0; list != nullptr && i < list->count; ++i)
                    collectHas(set, selectors, list->first + i, anchor);
                list = nullptr;
                break;
            }
            // States that the element's attributes decide (html/#pseudo-classes). The names of those attributes stand
            // for the states. An SVG a is a link only outside every other link (SVG2/linking.html#AElement), so an
            // ancestor's href decides it too.
            case CssPseudoClass_Link:
            case CssPseudoClass_Visited:
            case CssPseudoClass_AnyLink:
            case CssPseudoClass_LocalLink:
                invalidateAttribute(set, "href", unite(reach, Reach{.self = true, .subtree = true}));
                break;
            case CssPseudoClass_Open:
                invalidateAttribute(set, "open", reach);
                break;
            case CssPseudoClass_PrListBox:
                invalidateAttribute(set, "multiple", reach);
                invalidateAttribute(set, "size", reach);
                break;
            case CssPseudoClass_Required:
            case CssPseudoClass_Optional:
                // An input's type decides whether the input can be required at all.
                invalidateAttribute(set, "required", reach);
                invalidateAttribute(set, "type", reach);
                break;
            // States that an element takes from its ancestors' attributes.
            case CssPseudoClass_LangFn:
                // xml:lang is identified by its local name, which is lang.
                invalidateAttribute(set, "lang", unite(reach, kReachAll));
                break;
            case CssPseudoClass_DirFn:
                invalidateAttribute(set, "dir", unite(reach, kReachAll));
                reads->direction = true;
                break;
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
            case CssPseudoClass_Empty:
                reads->structure = true;
                // Whether a sibling matches the `of` selectors decides the element's place among its siblings.
                argument.self = true;
                argument.siblings = true;
                break;
            default:
                // A state that attributes decide, either of the element or of an ancestor, such as a fieldset's
                // disabled state.
                reads->states = true;
                invalidate(set, cssFeatureHash(CssFeature_State, ""), unite(reach, Reach{true, false, false, true, 0}));
                break;
        }
        for (uint32_t i = 0; list != nullptr && i < list->count; ++i)
            collectComplex(set, selectors, list->first + i, &argument);
    }
}

// Records what a change to each feature of a complex selector restyles. A selector in a pseudo-class's argument has the
// reach `outer` of the pseudo-class's place. A feature further left in such a selector restyles any element near that
// place.
void collectComplex(CssRuleSet* set, const PrCssSelectors* selectors, uint32_t complex, const Reach* PR_NULLABLE outer) {
    const CssComplexSelector* c = &selectors->complex[complex];
    if ((c->flags & CssComplexFlag_Invalid) != 0)
        return;
    uint32_t subject = subjectFeature(selectors, &selectors->compounds[c->compound]);
    bool descendant = false;
    bool sibling = false;
    for (uint32_t i = 0; i < c->compoundCount; ++i) {
        const CssCompoundSelector* compound = &selectors->compounds[c->compound + i];
        Reach reach{};
        if (outer != nullptr) {
            reach = i == 0 ? *outer : unite(*outer, Reach{false, true, false, true, 0});
        } else {
            reach.self = i == 0;
            reach.laterSiblings = sibling;
            // A descendant combinator to the right of a sibling combinator reaches the descendants of later siblings,
            // which laterSiblings restyles whole. To the left of one, it reaches the siblings of descendants, which are
            // descendants too.
            if (descendant && (sibling || subject == 0))
                reach.subtree = true;
            else if (descendant)
                reach.descendant = subject;
        }
        collectCompound(set, selectors, compound, reach);
        switch (compound->combinator) {
            case CssCombinator_Descendant:
            case CssCombinator_Child:
                descendant = true;
                break;
            case CssCombinator_NextSibling:
            case CssCombinator_SubsequentSibling:
                // A sibling added or removed changes which siblings are next to which.
                set->reads.structure = true;
                sibling = true;
                break;
            case CssCombinator_Column:
            case CssCombinator_Reference:
                set->reads.global = true;
                break;
            default:
                break;
        }
    }
}

uint32_t addMatcher(CssRuleSet* set, const PrCssSelectors* selectors) {
    for (uint32_t i = 0; i < selectors->list.count; ++i) {
        // A selector that styles nothing restyles nothing. One of a pseudo-element restyles its originating element.
        // One of ::slotted() styles a slot's children in the flat tree, which a change to the slot's features reaches.
        uint32_t complex = selectors->list.first + i;
        const CssComplexSelector* c = &selectors->complex[complex];
        if (cssSelectorPseudo(selectors, complex) != CssPseudo_Count) {
            collectComplex(set, selectors, complex, nullptr);
        } else if ((c->flags & CssComplexFlag_Invalid) == 0 && endsInSlotted(selectors, c)) {
            Reach slot{true, false, false, true, 0};
            collectComplex(set, selectors, complex, &slot);
        }
    }
    set->lists.push(selectors);
    return set->lists.count - 1;
}

void fileRules(Filing& f, const RawArray<CssRuleData* PR_NONNULL>& rules, uint16_t layer, PrStringView prefix);

// Returns the name of the layer that an @layer rule's prelude names at `index` among its comma-separated names, under
// `prefix`.
PrStringView layerName(Filing& f, PrStringView names, uint32_t index, PrStringView prefix) {
    prTextBufferClear(&f.name);
    if (prefix.size != 0) {
        prTextBufferAppend(&f.name, prefix);
        prTextBufferAppendChar(&f.name, '.');
    }
    uint32_t at = 0;
    for (size_t i = 0; i < names.size; ++i) {
        char c = names.data[i];
        if (c == ',') {
            ++at;
            continue;
        }
        if (at == index && !prAsciiIsSpace(c))
            prTextBufferAppendChar(&f.name, c);
    }
    return prTextBufferView(&f.name);
}

// Returns true if `list` matches the filing's media. The set records the list with the result.
bool matchesMedia(Filing& f, const CssMediaQueries* list) {
    bool matched = cssMediaListMatches(list, f.media);
    f.set->media.push(CssMediaAnswer{list, matched});
    return matched;
}

// Files the rules of the sheet that an @import rule imported (css-cascade-5/#at-import), in the layer that the rule's
// layer or layer() names, or in `layer` if it names none. Files nothing if the rule's supports() condition fails. The
// prelude holds the URL, then the layer and the supports() condition that the rule gives.
void fileImport(Filing& f, const CssRuleData* rule, uint16_t layer, PrStringView prefix) {
    CssValueRun prelude = cssValueListRun(&rule->prelude);
    bool layered = false;
    PrTextBuffer named{};
    // The layer and the condition may sit inside productions that group them, so every node is read. The arguments of a
    // function are read only as part of the function.
    for (uint32_t i = 0; i < prelude.count; ++i) {
        const CssValueNode* value = &prelude.values[i];
        if (cssValueIsKeyword(value, PrCssKeyword_Layer)) {
            layered = true;
            continue;
        }
        if (value->kind != PrCssValueKind_Function)
            continue;
        CssValueRun arguments{value + 1, value->next - 1, prelude.text};
        PrCssKeyword function = cssValueKeyword(value);
        // A supports() whose arguments are neither a supports condition nor a declaration is a condition that fails.
        bool condition = arguments.count != 0 && arguments.values[0].kind == PrCssValueKind_Production;
        if (function == PrCssKeyword_Supports && (!condition || !cssSupportsEvaluate(arguments))) {
            prTextBufferReset(&named);
            return;
        }
        if (function == PrCssKeyword_Layer) {
            layered = true;
            cssSerializeValues(&named, arguments);
        }
        i += value->next - 1;
    }
    uint16_t index = layer;
    PrRawString name{};
    if (layered) {
        // An import into an anonymous layer is a layer of its own (css-cascade-5/#at-import).
        PrStringView names = prTextBufferView(&named);
        if (names.size == 0) {
            prTextBufferClear(&f.name);
            prTextBufferAppendChar(&f.name, '\x01');
            prTextBufferAppendU64(&f.name, f.anonymous++, 10);
            name = prRawStringCopy(prTextBufferView(&f.name));
        } else {
            name = prRawStringCopy(layerName(f, names, 0, prefix));
        }
        index = layerIndex(f, prRawStringView(&name));
    }
    const CssSheetData* importing = f.sheet;
    f.sheet = rule->imported;
    fileRules(f, rule->imported->rules, index, layered ? prRawStringView(&name) : prefix);
    f.sheet = importing;
    prRawStringReset(&name);
    prTextBufferReset(&named);
}

void fileAtRule(Filing& f, const CssRuleData* rule, uint16_t layer, PrStringView prefix) {
    switch (kCssAtRuleAlias[rule->atRule]) {
        case CssAtRule_Media:
            if (f.media != nullptr && matchesMedia(f, &rule->media))
                fileRules(f, rule->children, layer, prefix);
            break;
        case CssAtRule_Supports:
            if (cssSupportsEvaluate(cssValueListRun(&rule->prelude)))
                fileRules(f, rule->children, layer, prefix);
            break;
        case CssAtRule_Import:
            if (rule->imported != nullptr && (f.media == nullptr || matchesMedia(f, &rule->media)))
                fileImport(f, rule, layer, prefix);
            break;
        case CssAtRule_Keyframes: {
            // A later rule of a name replaces an earlier one (css-animations-1/#keyframes).
            PrTextBuffer prelude{};
            cssSerializePrelude(&prelude, rule);
            PrStringView name = prStringTrim(prTextBufferView(&prelude));
            if (name.size >= 2 && (name.data[0] == '"' || name.data[0] == '\''))
                name = PrStringView{name.data + 1, name.size - 2};
            bool replaced = false;
            for (uint32_t i = 0; i < f.set->keyframes.count && !replaced; ++i) {
                if (prRawStringView(&f.set->keyframes.data[i].name) == name) {
                    f.set->keyframes.data[i].rule = rule;
                    replaced = true;
                }
            }
            if (!replaced)
                f.set->keyframes.push(CssKeyframes{prRawStringCopy(name), rule});
            prTextBufferReset(&prelude);
            break;
        }
        case CssAtRule_CounterStyle:
            f.set->counterStyleRules.push(rule);
            break;
        case CssAtRule_Layer: {
            PrTextBuffer prelude{};
            cssSerializePrelude(&prelude, rule);
            PrStringView names = prTextBufferView(&prelude);
            if (!rule->block) {
                // A statement declares the order of its layers (css-cascade-5/#layer-empty).
                uint32_t count = 1;
                for (size_t i = 0; i < names.size; ++i)
                    count += names.data[i] == ',' ? 1 : 0;
                for (uint32_t i = 0; i < count; ++i)
                    layerIndex(f, layerName(f, names, i, prefix));
                prTextBufferReset(&prelude);
                break;
            }
            // A block with no name is a layer of its own (css-cascade-5/#layer-block).
            PrRawString name{};
            if (names.size == 0) {
                prTextBufferClear(&f.name);
                prTextBufferAppendChar(&f.name, '\x01');
                prTextBufferAppendU64(&f.name, f.anonymous++, 10);
                name = prRawStringCopy(prTextBufferView(&f.name));
            } else {
                name = prRawStringCopy(layerName(f, names, 0, prefix));
            }
            uint16_t index = layerIndex(f, prRawStringView(&name));
            fileRules(f, rule->children, index, prRawStringView(&name));
            prRawStringReset(&name);
            prTextBufferReset(&prelude);
            break;
        }
        default:
            break;
    }
}

void fileRules(Filing& f, const RawArray<CssRuleData* PR_NONNULL>& rules, uint16_t layer, PrStringView prefix) {
    for (uint32_t i = 0; i < rules.count; ++i) {
        const CssRuleData* rule = rules.data[i];
        if (rule->kind == CssRuleDataKind_At) {
            fileAtRule(f, rule, layer, prefix);
            continue;
        }
        if (rule->kind != CssRuleDataKind_Style || rule->selectors == nullptr)
            continue;
        uint32_t matcher = addMatcher(f.set, rule->selectors);
        fileSelectors(f, rule->selectors, matcher, &rule->style, layer);
        // The declarations after a nested rule apply in the same way as the rule's own declarations
        // (css-nesting-1/#nested-declarations-rule).
        for (uint32_t c = 0; c < rule->children.count; ++c) {
            const CssRuleData* child = rule->children.data[c];
            if (child->kind == CssRuleDataKind_NestedDeclarations)
                fileSelectors(f, rule->selectors, matcher, &child->style, layer);
        }
    }
}

// Returns true if `a` applies before `b`. Entries are ordered by rank (rankOf), then by order of appearance.
bool appliesBefore(const CssRuleEntry& a, const CssRuleEntry& b) {
    if (a.rank != b.rank)
        return a.rank < b.rank;
    return a.order < b.order;
}

// Returns the CSS-wide keyword that a run is, or PrCssKeyword_Count.
PrCssKeyword wideKeyword(CssValueRun run) {
    bool keyword = run.count == 1 && run.values[0].kind == PrCssValueKind_Keyword;
    if (keyword && cssIsCssWideKeyword(cssValueKeyword(run.values)))
        return cssValueKeyword(run.values);
    return PrCssKeyword_Count;
}

// An element's presentational hints, in the order that they apply: those that another element gives it
// (cssSharedHints), then those of its own attributes (cssHints).
struct CssHintBlocks {
    const CssDeclarationBlock* PR_NULLABLE shared;
    const CssDeclarationBlock* PR_NULLABLE own;
};

// The place of a block of declarations in the cascade. A revert or revert-layer in the block rolls back from this place
// (css-cascade-5/#default).
struct CssPlace {
    // The declarations that a rollback can reach: the matched rules, and the element's presentational hints. The hints
    // follow the rules of the hint origin.
    const CssRuleSet* set;
    CssHintBlocks hints;

    CssOrigin origin;
    uint32_t layer;                        // the block's layer, or past kCssUnlayered for the element's style attribute
    const CssSheetData* PR_NULLABLE sheet; // the sheet of the rule that holds the block, or null for no rule
};

// Returns the last normal declaration of `longhand` before the layer `layer` of `origin`. Returns no declaration if no
// earlier declaration sets the longhand.
CssCascaded rolledBack(const CssCascadeScratch& s, const CssPlace& place, CssOrigin origin, uint32_t layer,
        PrCssProperty longhand) {
    CssCascaded winner{};
    auto consider = [&](const CssDeclarationBlock* block, const CssSheetData* sheet, CssOrigin declared) {
        const CssDeclaration* declarations = cssDeclarations(block);
        for (uint32_t k = 0; k < block->count; ++k) {
            const CssDeclaration* d = &declarations[k];
            if (d->important || d->descriptor != PrCssDescriptor_None || d->property == PrCssProperty_Count)
                continue;
            CssPropertySpan longhands = kCssPropertyLonghands[d->property];
            bool sets = d->property == longhand;
            for (uint32_t l = 0; l < longhands.count && !sets; ++l)
                sets = kCssPropertyRuns[longhands.start + l] == longhand;
            if (sets)
                winner = CssCascaded{
                    .declaration = d,
                    .block = block,
                    .longhand = longhand,
                    .origin = declared,
                    .order = 0,
                    .sheet = sheet,
                };
        }
    };
    auto considerHints = [&]() {
        if (place.hints.shared != nullptr)
            consider(place.hints.shared, nullptr, CssOrigin_Hint);
        if (place.hints.own != nullptr)
            consider(place.hints.own, nullptr, CssOrigin_Hint);
    };
    bool hints = origin == CssOrigin_Author;
    for (uint32_t i = 0; i < s.applyingCount; ++i) {
        const CssRuleEntry& entry = place.set->entries.data[s.applying[i]];
        if (entry.origin > origin || (entry.origin == origin && entry.layer >= layer))
            continue;
        if (hints && entry.origin == CssOrigin_Author) {
            considerHints();
            hints = false;
        }
        consider(entry.block, entry.sheet, entry.origin);
    }
    if (hints)
        considerHints();
    return winner;
}

// Records that `property` has a winner, which it did not have before.
void declare(CssCascadeScratch& s, PrCssProperty property) {
    s.declared.push(property);
    CssStyleGroup group = cssStyleSlot(property).group;
    if (group != CssStyleGroup_Count)
        s.declaredGroups |= 1u << group;
}

// Records the declarations of `block` that have the importance `important` as the winners so far. `place` is the
// block's place in the cascade. A revert rolls a longhand back to the origin before the block's. A revert-layer rolls a
// longhand back to the layer before the block's. The hint origin counts as part of the author origin for revert, and as
// an origin of its own for revert-layer (css-cascade-5/#preshint).
void apply(CssCascadeScratch& s, const CssDeclarationBlock* block, bool important, const CssPlace& place) {
    const CssDeclaration* declarations = cssDeclarations(block);
    for (uint32_t i = 0; i < block->count; ++i) {
        const CssDeclaration* d = &declarations[i];
        if (d->important != important || d->descriptor != PrCssDescriptor_None)
            continue;
        CssCascaded cascaded{
            .declaration = d,
            .block = block,
            .longhand = d->property,
            .origin = place.origin,
            .order = ++s.applied,
            .sheet = place.sheet,
        };
        PrCssKeyword wide = PrCssKeyword_Count;
        if (d->valueCount == 1 && d->property != PrCssProperty_Count)
            wide = wideKeyword(cssDeclarationValue(block, d));
        bool reverted = wide == PrCssKeyword_Revert || wide == PrCssKeyword_RevertLayer;
        CssOrigin origin = place.origin;
        uint32_t layer = place.layer;
        if (wide == PrCssKeyword_Revert) {
            origin = place.origin == CssOrigin_UserAgent ? CssOrigin_UserAgent : CssOrigin_Hint;
            layer = 0;
        }
        if (d->property == PrCssProperty_Count) {
            PrStringView name = cssDeclarationName(block, d);
            uint32_t hash = cssCustomHash(name);
            uint32_t at = s.customIndex.find(hash, [&](uint32_t k) {
                return cssDeclarationName(s.custom.data[k].block, s.custom.data[k].declaration) == name;
            });
            if (at != kHashMissing) {
                s.custom.data[at] = cascaded;
            } else {
                s.customIndex.insert(hash, s.custom.count);
                s.custom.push(cascaded);
            }
            continue;
        }
        CssPropertySpan longhands = kCssPropertyLonghands[d->property];
        uint32_t count = longhands.count != 0 ? longhands.count : 1;
        for (uint32_t k = 0; k < count; ++k) {
            PrCssProperty longhand = longhands.count != 0 ? kCssPropertyRuns[longhands.start + k] : d->property;
            CssCascaded& winner = s.winners.data[longhand];
            if (winner.declaration == nullptr)
                declare(s, longhand);
            winner = cascaded;
            winner.longhand = longhand;
            if (reverted) {
                // A longhand with no earlier declaration takes its default. It applies before every declaration, so a
                // flow-relative longhand that rolls back to a declaration wins over its physical counterpart.
                winner = rolledBack(s, place, origin, layer, longhand);
                winner.order = winner.declaration != nullptr ? cascaded.order : 0;
            }
        }
    }
}

// Applies the matched entries of `origin` in order, or for important declarations, their encapsulation contexts and
// their layers in reverse (css-cascade-5/#cascade-context, css-cascade-5/#layer-ordering).
void applyOrigin(CssCascadeScratch& s, const CssRuleSet* set, const CssHintBlocks& hints, CssOrigin origin,
        bool important) {
    const uint32_t* matched = s.applying;
    uint32_t count = s.applyingCount;
    if (!important) {
        for (uint32_t i = 0; i < count; ++i) {
            const CssRuleEntry& entry = set->entries.data[matched[i]];
            if (entry.origin == origin)
                apply(s, entry.block, false, CssPlace{set, hints, origin, entry.layer, entry.sheet});
        }
        return;
    }
    // Runs of one layer, from the last run back.
    uint32_t end = count;
    while (end != 0) {
        uint32_t start = end - 1;
        uint16_t layer = set->entries.data[matched[start]].layer;
        uint32_t depth = set->entries.data[matched[start]].depth;
        while (start != 0 && set->entries.data[matched[start - 1]].layer == layer &&
                set->entries.data[matched[start - 1]].depth == depth &&
                set->entries.data[matched[start - 1]].origin == set->entries.data[matched[end - 1]].origin)
            --start;
        for (uint32_t i = start; i < end; ++i) {
            const CssRuleEntry& entry = set->entries.data[matched[i]];
            if (entry.origin == origin)
                apply(s, entry.block, true, CssPlace{set, hints, origin, entry.layer, entry.sheet});
        }
        end = start;
    }
}

// Gives the winner of each flow-relative longhand the place of its physical counterpart if the flow-relative longhand
// applies later. The mapping uses the element's own computed writing mode and direction (css-logical-1/#box). The two
// properties share the physical property's computed value, which inherits as the physical property does
// (csswg-drafts#3029).
void mapFlowRelative(CssCascadeScratch& s, const CssComputedStyle* style, bool* readsFont) {
    PrCssKeyword writingMode = PrCssKeyword_Unknown;
    PrCssKeyword direction = PrCssKeyword_Unknown;
    uint32_t count = s.declared.count;
    for (uint32_t i = 0; i < count; ++i) {
        PrCssProperty property = s.declared.data[i];
        if (!cssIsFlowRelative(property))
            continue;
        if (writingMode == PrCssKeyword_Unknown) {
            *readsFont = true;
            writingMode = cssValueKeyword(cssComputedValue(style, PrCssProperty_WritingMode).values);
            direction = cssValueKeyword(cssComputedValue(style, PrCssProperty_Direction).values);
        }
        PrCssProperty physical = cssPhysicalProperty(property, writingMode, direction);
        const CssCascaded& logical = s.winners.data[property];
        CssCascaded& target = s.winners.data[physical];
        if (target.declaration != nullptr && target.order > logical.order)
            continue;
        if (target.declaration == nullptr)
            declare(s, physical);
        target = logical;
    }
}

// Returns the longhands that important declarations won, or null if none did.
const CssImportantProperties* PR_NULLABLE importantOf(CssCascadeScratch& s) {
    RawArray<PrCssProperty> important;
    for (uint32_t i = 0; i < s.declared.count; ++i) {
        PrCssProperty property = s.declared.data[i];
        const CssDeclaration* d = s.winners.data[property].declaration;
        if (d != nullptr && d->important)
            important.push(property);
    }
    if (important.count == 0)
        return nullptr;
    for (uint32_t i = 1; i < important.count; ++i) {
        PrCssProperty property = important.data[i];
        uint32_t j = i;
        for (; j > 0 && important.data[j - 1] > property; --j)
            important.data[j] = important.data[j - 1];
        important.data[j] = property;
    }
    const CssImportantProperties* made = cssImportantPropertiesCreate(important.data, important.count);
    important.reset();
    return made;
}

// Forgets the winners of the last cascade, its custom properties and the shorthand that it parsed.
void forgetWinners(CssCascadeScratch& s) {
    for (uint32_t i = 0; i < s.declared.count; ++i)
        s.winners.data[s.declared.data[i]] = CssCascaded{};
    s.declared.clear();
    s.declaredGroups = 0;
    s.applied = 0;
    s.shorthand = nullptr;
    // The index's slots are cleared only if the last cascade declared a custom property. Most elements declare none,
    // and the number of slots equals the most custom properties that any element declared.
    if (s.custom.count != 0) {
        s.custom.clear();
        s.customIndex.clear();
    }
}

// --- The matched properties cache ---

// Empties a slot, releasing what it holds.
void forgetMatched(CssMatchedStyle& cached) {
    cssComputedStyleRelease(cached.style);
    for (uint32_t g = 0; g < CssStyleGroup_Count; ++g) {
        cssStyleValuesRelease(cached.parentGroups[g]);
        cached.parentGroups[g] = nullptr;
    }
    cssCustomPropertiesRelease(cached.parentCustom);
    cached.style = nullptr;
    cached.parentCustom = nullptr;
    cached.hash = 0;
    cached.matched.clear();
}

void forgetMatchedStyles(CssCascadeScratch& s) {
    for (uint32_t i = 0; i < s.matchedStyles.count; ++i) {
        if (s.matchedStyles.data[i].hash != 0)
            forgetMatched(s.matchedStyles.data[i]);
    }
    for (uint32_t i = 0; i < s.cachedGroups.count; ++i) {
        CssCachedGroup& cached = s.cachedGroups.data[i];
        if (cached.hash == 0)
            continue;
        cssStyleValuesRelease(cached.values);
        cssStyleValuesRelease(cached.parentValues);
        cached.values = nullptr;
        cached.parentValues = nullptr;
        cached.hash = 0;
    }
}

// Returns the set of kCssMatchedWays slots for the entries that the element matched, whose hash is `hash`.
CssMatchedStyle* matchedSet(CssCascadeScratch& s, uint32_t hash) {
    if (s.matchedStyles.count == 0)
        s.matchedStyles.resizeZeroed(kCssMatchedStyles);
    return s.matchedStyles.data + hash % (kCssMatchedStyles / kCssMatchedWays) * kCssMatchedWays;
}

// Returns the slot of `set` in which a style for `hash` is stored: a slot that holds a style for the same entries, an
// empty slot, or the least recently used slot.
CssMatchedStyle& matchedVictim(CssMatchedStyle* set, uint32_t hash) {
    CssMatchedStyle* victim = &set[0];
    for (uint32_t way = 0; way < kCssMatchedWays; ++way) {
        CssMatchedStyle& slot = set[way];
        if (slot.hash == hash || slot.style == nullptr)
            return slot;
        if (slot.used < victim->used)
            victim = &slot;
    }
    return *victim;
}

// Returns true if two fonts give font-relative lengths the same bases.
bool sameBases(const CssFontBases& a, const CssFontBases& b) {
    return a.em == b.em && a.lh == b.lh;
}

// How a cached style fits an element.
enum {
    Fit_None,                       // the style does not apply
    Fit_Same,                       // the style applies as it is
    Fit_Rebase,                     // the style applies with the parent's groups in place of those it passed through
};
typedef uint8_t Fit;

// Returns how the style of `cached` fits the element that `input` describes, which matched the scratch's candidates,
// whose hash is `hash`. A style fits if it was computed for the same entries, relative to the same inputs, and with the
// same kind of widget or appearance of what originates it. A parent's groups that do not inherit are compared only if
// the style read one, which `inherit` does. No value reads the locale, so a style cached under another locale fits
// except for its locale.
Fit fits(const CssMatchedStyle& cached, uint32_t hash, const CssCascadeScratch& s, const CssCascadeInput& input,
        uint64_t sharedHints, DomWidget widget, CssAppearance parentAppearance) {
    if (cached.style == nullptr || cached.hash != hash || cached.matched.count != s.candidates.count ||
            !prMemEquals(cached.matched.data, s.candidates.data, sizeof(uint32_t) * s.candidates.count) ||
            cached.pseudo != input.pseudo || cached.sharedHints != sharedHints || cached.widget != widget ||
            cached.parentAppearance != parentAppearance)
        return Fit_None;
    if (cached.root != (input.parent == nullptr) || !sameBases(cached.rootFont, input.rootFont))
        return Fit_None;
    if (cached.readsViewport &&
            (cached.viewport.width != input.viewport.width || cached.viewport.height != input.viewport.height))
        return Fit_None;
    if (input.parent == nullptr)
        return Fit_Same;
    if (input.parent->custom != cached.parentCustom || input.parent->childrenAreItems != cached.parentItems)
        return Fit_None;
    Fit fit = Fit_Same;
    for (uint32_t g = 0; g < CssStyleGroup_Count; ++g) {
        bool inherited = cssStyleGroupInherited(CssStyleGroup(g));
        if ((!inherited && !cached.style->inheritsReset) || input.parent->groups[g] == cached.parentGroups[g])
            continue;
        bool passes = inherited && !cached.style->inheritsReset && (cached.passThrough & (1u << g)) != 0 &&
            (g != CssStyleGroup_Font || !cached.readsFont);
        if (!passes)
            return Fit_None;
        fit = Fit_Rebase;
    }
    return fit;
}

// Returns the locale of the element's language (html/#language): the locale of the tag that the element declares, and
// otherwise its parent's locale. For the root, it is the locale of the document's default language. A tag is resolved
// only for the element that declares it, by lookup, so that a subtag that the registry does not list leaves the locale
// of the subtags before it.
PrLocale localeOf(const CssCascadeInput& input) {
    PrStringView own = domElementOwnLanguage(input.element);
    if (own.data != nullptr)
        return prLocaleFromTag(own);
    if (input.parent != nullptr)
        return input.parent->locale;
    return prLocaleFromTag(domDocumentLanguage(input.element->document));
}

// Returns a new style with the groups and custom properties of `style`, in `locale`.
const CssComputedStyle* withLocale(const CssComputedStyle* style, PrLocale locale) {
    CssComputedStyle* copy = allocZeroed<CssComputedStyle>();
    copy->references.storeRelaxed(1);
    for (uint32_t g = 0; g < CssStyleGroup_Count; ++g) {
        cssStyleValuesRetain(style->groups[g]);
        copy->groups[g] = style->groups[g];
    }
    if (style->custom != nullptr)
        cssCustomPropertiesRetain(style->custom);
    copy->custom = style->custom;
    if (style->important != nullptr)
        cssImportantPropertiesRetain(style->important);
    copy->important = style->important;
    copy->locale = locale;
    copy->appearance = style->appearance;
    copy->inheritsReset = style->inheritsReset;
    copy->staticInline = style->staticInline;
    copy->childrenAreItems = style->childrenAreItems;
    copy->font = style->font;
    return copy;
}

// Returns a copy of the cached style of `cached` for an element whose parent is `parent`, with the parent's groups in
// place of those that the cached style passed through, and with `locale`.
const CssComputedStyle* rebased(const CssMatchedStyle& cached, const CssComputedStyle* parent, PrLocale locale) {
    const CssComputedStyle* style = cached.style;
    CssComputedStyle* copy = allocZeroed<CssComputedStyle>();
    copy->references.storeRelaxed(1);
    for (uint32_t g = 0; g < CssStyleGroup_Count; ++g) {
        bool passed = (cached.passThrough & (1u << g)) != 0;
        copy->groups[g] = passed ? parent->groups[g] : style->groups[g];
        cssStyleValuesRetain(copy->groups[g]);
    }
    if (style->custom != nullptr)
        cssCustomPropertiesRetain(style->custom);
    copy->custom = style->custom;
    if (style->important != nullptr)
        cssImportantPropertiesRetain(style->important);
    copy->important = style->important;
    copy->locale = locale;
    copy->appearance = style->appearance;
    copy->inheritsReset = style->inheritsReset;
    copy->staticInline = style->staticInline;
    copy->childrenAreItems = style->childrenAreItems;
    copy->font = (cached.passThrough & (1u << CssStyleGroup_Font)) != 0 ? parent->font : style->font;
    return copy;
}

// Stores `style`, computed for the scratch's candidates under `input`, in `slot`.
void store(CssMatchedStyle& slot, uint32_t hash, uint64_t sharedHints, DomWidget widget,
        CssAppearance parentAppearance, const CssCascadeScratch& s, const CssCascadeInput& input,
        const CssComputedStyle* style, bool readsViewport, bool readsFont) {
    forgetMatched(slot);
    slot.hash = hash;
    slot.matched.append(s.candidates.span());
    cssComputedStyleRetain(style);
    slot.style = style;
    slot.root = input.parent == nullptr;
    if (input.parent != nullptr) {
        for (uint32_t g = 0; g < CssStyleGroup_Count; ++g) {
            cssStyleValuesRetain(input.parent->groups[g]);
            slot.parentGroups[g] = input.parent->groups[g];
        }
        if (input.parent->custom != nullptr)
            cssCustomPropertiesRetain(input.parent->custom);
        slot.parentCustom = input.parent->custom;
        slot.parentItems = input.parent->childrenAreItems;
    }
    slot.rootFont = input.rootFont;
    slot.pseudo = input.pseudo;
    slot.widget = widget;
    slot.parentAppearance = parentAppearance;
    slot.sharedHints = sharedHints;
    slot.readsFont = readsFont;
    slot.passThrough = s.passThrough;
    slot.viewport = input.viewport;
    slot.readsViewport = readsViewport;
    slot.used = s.matchedLookups;
}

// Returns true if the element's memo holds a style computed from what the cascade now has: the same entries of the same
// rule set, the same parent's style, root font, kind of widget, shared hints and language, and the same viewport if the
// style read it.
bool remembers(const CssCascadeMemo& memo, const CssRuleSet* set, const CssCascadeScratch& s,
        const CssCascadeInput& input, uint64_t sharedHints, DomWidget widget, PrLocale locale) {
    if (memo.style == nullptr || memo.generation != set->generation || memo.parent != input.parent ||
            input.pseudo != CssPseudo_None || memo.widget != widget ||
            memo.sharedHints != sharedHints || memo.style->locale != locale ||
            !sameBases(memo.rootFont, input.rootFont))
        return false;
    if (memo.readsViewport &&
            (memo.viewport.width != input.viewport.width || memo.viewport.height != input.viewport.height))
        return false;
    return memo.matched.count == s.candidates.count &&
        prMemEquals(memo.matched.data, s.candidates.data, sizeof(uint32_t) * s.candidates.count);
}

// Keeps `style`, which the element now has from the entries it matched, in its memo.
void remember(CssCascadeMemo* memo, const CssRuleSet* set, const CssCascadeScratch& s, const CssCascadeInput& input,
        const CssComputedStyle* style, uint64_t sharedHints, DomWidget widget, bool readsViewport) {
    cssComputedStyleRetain(style);
    if (input.parent != nullptr)
        cssComputedStyleRetain(input.parent);
    cssCascadeMemoClear(memo);
    memo->style = style;
    memo->parent = input.parent;
    memo->matched.append(s.candidates.span());
    memo->generation = set->generation;
    memo->rootFont = input.rootFont;
    memo->viewport = input.viewport;
    memo->readsViewport = readsViewport;
    memo->widget = widget;
    memo->sharedHints = sharedHints;
}

// --- The ancestor filter ---

void addFeature(CssCascadeScratch& s, uint32_t hash) {
    uint32_t a = hash & (kCssAncestorFilterWords * 64 - 1);
    uint32_t b = (hash >> 12) & (kCssAncestorFilterWords * 64 - 1);
    s.ancestorBits[a >> 6] |= uint64_t(1) << (a & 63);
    s.ancestorBits[b >> 6] |= uint64_t(1) << (b & 63);
}

bool mayCarry(const CssCascadeScratch& s, uint32_t hash) {
    uint32_t a = hash & (kCssAncestorFilterWords * 64 - 1);
    uint32_t b = (hash >> 12) & (kCssAncestorFilterWords * 64 - 1);
    return (s.ancestorBits[a >> 6] >> (a & 63) & 1) != 0 && (s.ancestorBits[b >> 6] >> (b & 63) & 1) != 0;
}

// Adds the features of `element` that the ancestor filter tracks: its id, classes, type and attribute names.
void addFeatures(CssCascadeScratch& s, const PrDomElement* element) {
    PrStringView id = domElementId(element);
    if (id.size != 0)
        addFeature(s, cssFeatureHash(CssFeature_Id, id));
    if (element->classes != kDomClassesNone) {
        DomClassList classes = domClassesList(element->document, element->classes);
        for (uint32_t i = 0; i < classes.count; ++i)
            addFeature(s, cssFeatureHash(CssFeature_Class, domClassesName(element->document, classes.atoms[i])));
    }
    addFeature(s, cssFeatureHash(CssFeature_Type, domElementName(element)));
    for (const PrDomNode* node = element->firstAttribute; node != nullptr; node = node->next)
        addFeature(s, attributeBucket(static_cast<const PrDomAttr*>(node)->hash));
}

// Brings the filter to the ancestors of `element`. The filter is kept as is if they are the last element's ancestors.
// If the element's parent is a child of the last element's parent, the parent's features are added. Otherwise the
// filter is built again.
void filterAncestors(CssCascadeScratch& s, const PrDomElement* element) {
    const PrDomNode* parent = element->parent;
    if (parent == s.ancestorsOf && s.ancestorsEpoch == s.epoch)
        return;
    const PrDomElement* parentElement = domElementFromNode(parent);
    if (parentElement != nullptr && s.ancestorsOf != nullptr && parent->parent == s.ancestorsOf && s.ancestorsEpoch == s.epoch) {
        addFeatures(s, parentElement);
    } else {
        prMemZero(s.ancestorBits, sizeof(s.ancestorBits));
        for (const PrDomElement* at = parentElement; at != nullptr; at = domElementFromNode(at->parent))
            addFeatures(s, at);
    }
    s.ancestorsOf = parent;
    s.ancestorsEpoch = s.epoch;
}

// --- Matching ---

// Brings the scratch's matchers up to date with the rule set.
void follow(CssCascadeScratch& s, const CssRuleSet* set) {
    if (s.set == set && s.generation == set->generation)
        return;
    forgetMatchedStyles(s);
    for (uint32_t i = 0; i < s.matchers.count; ++i)
        cssSelectorMatcherReset(&s.matchers.data[i]);
    s.matchers.clear();
    s.epochs.clear();
    for (uint32_t i = 0; i < set->lists.count; ++i) {
        CssSelectorMatcher& matcher = s.matchers.emplace();
        cssSelectorMatcherInit(&matcher, set->lists.data[i], nullptr, set->document, nullptr);
        matcher.purpose = CssMatchPurpose_Style;
        s.epochs.push(s.epoch);
    }
    s.set = set;
    s.generation = set->generation;
}

// Brings the scratch's bucket hashes to those of `element` (CssCascadeScratch::featureHashes).
void elementFeatures(CssCascadeScratch& s, const PrDomElement* element) {
    if (s.featuresOf == element && s.featuresEpoch == s.epoch)
        return;
    RawArray<uint32_t>& hashes = s.featureHashes;
    hashes.clear();
    PrStringView id = domElementId(element);
    if (id.size != 0)
        hashes.push(cssFeatureHash(CssFeature_Id, id));
    if (element->classes != kDomClassesNone) {
        DomClassList classes = domClassesList(element->document, element->classes);
        for (uint32_t i = 0; i < classes.count; ++i)
            hashes.push(cssFeatureHash(CssFeature_Class, domClassesName(element->document, classes.atoms[i])));
    }
    hashes.push(cssFeatureHash(CssFeature_Type, domElementName(element)));
    // Each attribute's bucket is visited once, even if attributes of several namespaces share a name.
    for (const PrDomNode* node = element->firstAttribute; node != nullptr; node = node->next) {
        uint32_t hash = static_cast<const PrDomAttr*>(node)->hash;
        bool seen = false;
        for (const PrDomNode* before = element->firstAttribute; before != node && !seen; before = before->next)
            seen = static_cast<const PrDomAttr*>(before)->hash == hash;
        if (!seen)
            hashes.push(attributeBucket(hash));
    }
    s.featuresOf = element;
    s.featuresEpoch = s.epoch;
}

// Returns the matcher of the entry's selectors, ready to match. In a document with shadow trees (`scoped`), it matches
// in the entry's tree, whose host is its context.
CssSelectorMatcher* matcherOf(CssCascadeScratch& s, const CssRuleEntry& e, bool scoped) {
    CssSelectorMatcher* matcher = &s.matchers.data[e.matcher];
    if (s.epochs.data[e.matcher] != s.epoch) {
        cssSelectorMatcherForget(matcher);
        s.epochs.data[e.matcher] = s.epoch;
    }
    matcher->structure = &s.structure;
    if (scoped && matcher->readsHost) {
        const PrDomShadowRoot* shadow = e.scope != nullptr ? domNodeShadowRoot(e.scope) : nullptr;
        matcher->host = shadow != nullptr ? shadow->host : nullptr;
    }
    return matcher;
}

// Returns the root of the tree whose rules an element matches: its shadow root if it is in a shadow tree, and its
// document otherwise, or null if the set has no scopes to test. An element of a use element's shadow tree matches the
// rules of the tree that its host matches, whose sheets are the shadow tree's (SVG2/struct.html#UseStyleInheritance).
const PrDomNode* PR_NULLABLE treeOf(const CssRuleSet* set, const PrDomElement* element) {
    if (!set->scoped)
        return nullptr;
    const PrDomNode* root = domNodeRoot(element);
    while (domIsUseShadowRoot(root))
        root = domNodeRoot(domNodeShadowRoot(root)->host);
    return (root->flags & DomNodeFlag_ShadowRoot) != 0 ? root : element->document;
}

// Returns true if `element`, in the tree whose root is `tree` (treeOf), or its pseudo-element that the entry's selector
// styles, matches the entry. The ancestor filter rejects most entries whose ancestors the element lacks without
// matching them. It is brought to the element's ancestors once an entry reads them, which `*filtered` records across
// the element's entries. `Scoped` is true if the set tests scopes, in which case `tree` is the element's tree (treeOf),
// and false for a set that tests none, whose matching costs nothing for them.
template <bool Scoped>
bool matchesEntry(const CssRuleSet* set, CssCascadeScratch& s, const PrDomElement* element,
        const PrDomNode* PR_NULLABLE tree, uint32_t entry, bool* filtered) {
    const CssRuleEntry& e = set->entries.data[entry];
    // An element matches the rules of its own tree, and a host also those of its shadow tree that use :host.
    if (Scoped && e.scope != nullptr && e.scope != tree &&
            (e.scope != static_cast<const PrDomNode*>(domShadowRootOfHost(element)) ||
                !s.matchers.data[e.matcher].readsHost))
        return false;
    if ((e.ancestors[0] != 0 || e.anyAncestor[0] != 0) && !*filtered) {
        filterAncestors(s, element);
        *filtered = true;
    }
    for (uint32_t k = 0; k < kCssAncestorHashes && e.ancestors[k] != 0; ++k) {
        if (!mayCarry(s, e.ancestors[k]))
            return false;
    }
    if (e.anyAncestor[0] != 0) {
        bool carries = false;
        for (uint32_t k = 0; k < kCssAncestorAlternatives && e.anyAncestor[k] != 0 && !carries; ++k)
            carries = mayCarry(s, e.anyAncestor[k]);
        if (!carries)
            return false;
    }
    CssSelectorMatcher* matcher = matcherOf(s, e, Scoped);
    matcher->pseudoElements = e.pseudo != CssPseudo_None;
    bool matches = cssSelectorMatcherMatchesComplex(matcher, e.complex, element);
    matcher->pseudoElements = false;
    return matches;
}

// Calls `visit` with each entry of `filed` that `element` may match: those filed under its features, and the universal
// entries. Stops once `visit` returns false.
template <typename Visit>
void matchEach(const CssRuleSet*, CssCascadeScratch& s, const PrDomElement* element, const CssRuleIndex& filed, Visit visit) {
    elementFeatures(s, element);
    for (uint32_t i = 0; i < s.featureHashes.count; ++i) {
        uint32_t hash = s.featureHashes.data[i];
        uint32_t at = filed.index.find(hash, [&](uint32_t k) { return filed.buckets.data[k].hash == hash; });
        if (at == kHashMissing)
            continue;
        const CssRuleBucket& b = filed.buckets.data[at];
        for (uint32_t k = 0; k < b.entries.count; ++k) {
            if (!visit(b.entries.data[k]))
                return;
        }
    }
    for (uint32_t i = 0; i < filed.universal.count; ++i) {
        if (!visit(filed.universal.data[i]))
            return;
    }
}

// Collects the entries of `filed` that `element`, or its pseudo-element that they style, matches, in the order they
// apply.
void match(const CssRuleSet* set, CssCascadeScratch& s, const PrDomElement* element, const CssRuleIndex& filed) {
    s.candidates.clear();
    bool filtered = false;
    const PrDomNode* tree = treeOf(set, element);
    if (tree != nullptr) {
        matchEach(set, s, element, filed, [&](uint32_t entry) {
            if (matchesEntry<true>(set, s, element, tree, entry, &filtered))
                s.candidates.push(entry);
            return true;
        });
    } else {
        matchEach(set, s, element, filed, [&](uint32_t entry) {
            if (matchesEntry<false>(set, s, element, nullptr, entry, &filtered))
                s.candidates.push(entry);
            return true;
        });
    }
    // The rules of ::slotted() in the trees of the slots that the element is assigned to, through slots assigned to
    // slots, apply to the element itself (css-shadow-1/#slotted-pseudo).
    bool slotted = tree != nullptr && set->slotted.count != 0 && &filed == &set->filed[CssPseudo_None] &&
        (element->flags & DomNodeFlag_Slotted) != 0;
    for (const PrDomElement* slot = slotted ? domAssignedSlot(element) : nullptr; slot != nullptr;
            slot = domAssignedSlot(slot)) {
        const PrDomNode* slotTree = domNodeRoot(slot);
        for (uint32_t i = 0; i < set->slotted.count; ++i) {
            const CssRuleEntry& e = set->entries.data[set->slotted.data[i]];
            if (e.scope != slotTree)
                continue;
            if (cssSelectorMatcherMatchesSlotted(matcherOf(s, e, true), e.complex, element, slot))
                s.candidates.push(set->slotted.data[i]);
        }
    }
    sortItems(s.candidates.span(), [&](uint32_t a, uint32_t b) {
        return appliesBefore(set->entries.data[a], set->entries.data[b]);
    });
}

// Sets `out` to the block-level display that blockifying the computed display `run` gives (css-display-4/#blockify).
// The parser holds the two-keyword forms of a display as their legacy keywords. Each inline-level display becomes its
// block-level equivalent, and inline-block becomes block. A table-internal display becomes block, with flow as its
// inner display (css-display-4/#transformations). Returns false if `run` is neither inline-level nor table-internal.
bool blockified(CssValueRun run, PrCssKeyword* out) {
    if (run.count == 2 && cssValueIsKeyword(run.values, PrCssKeyword_Inline) &&
            cssValueIsKeyword(run.values + 1, PrCssKeyword_ListItem)) {
        *out = PrCssKeyword_ListItem;
        return true;
    }
    if (run.count != 1 || run.values[0].kind != PrCssValueKind_Keyword)
        return false;
    switch (cssValueKeyword(run.values)) {
        case PrCssKeyword_Inline:
        case PrCssKeyword_InlineBlock: *out = PrCssKeyword_Block; return true;
        case PrCssKeyword_InlineTable: *out = PrCssKeyword_Table; return true;
        case PrCssKeyword_InlineFlex: *out = PrCssKeyword_Flex; return true;
        case PrCssKeyword_InlineGrid: *out = PrCssKeyword_Grid; return true;
        case PrCssKeyword_TableRowGroup:
        case PrCssKeyword_TableHeaderGroup:
        case PrCssKeyword_TableFooterGroup:
        case PrCssKeyword_TableRow:
        case PrCssKeyword_TableColumnGroup:
        case PrCssKeyword_TableColumn:
        case PrCssKeyword_TableCell:
        case PrCssKeyword_TableCaption: *out = PrCssKeyword_Block; return true;
        default: return false;
    }
}

// The result of looking up a var() reference.
enum {
    Reference_Found,                // the value was appended
    Reference_Missing,              // no value, so the fallback applies
    Reference_Invalid,              // a value that cannot be substituted, which makes the whole value invalid
};
typedef uint8_t Reference;

// Returns true if `text` is a <number-token> alone (css-syntax-3/#consume-number).
bool isNumberToken(PrStringView text) {
    size_t i = 0;
    if (i < text.size && (text.data[i] == '+' || text.data[i] == '-'))
        ++i;
    size_t digits = 0;
    for (; i < text.size && prAsciiIsDigit(text.data[i]); ++i)
        ++digits;
    if (i + 1 < text.size && text.data[i] == '.' && prAsciiIsDigit(text.data[i + 1])) {
        for (++i; i < text.size && prAsciiIsDigit(text.data[i]); ++i)
            ++digits;
    }
    if (digits == 0)
        return false;
    if (i < text.size && (text.data[i] == 'e' || text.data[i] == 'E')) {
        size_t at = i + 1;
        if (at < text.size && (text.data[at] == '+' || text.data[at] == '-'))
            ++at;
        if (at < text.size && prAsciiIsDigit(text.data[at])) {
            for (i = at; i < text.size && prAsciiIsDigit(text.data[i]); ++i) {
            }
        }
    }
    return i == text.size;
}

// Appends what attr() gives `element`, for its first argument `first` with its own functions substituted
// (css-values-5/#attr-substitution). `fallback` says whether a fallback follows the comma. Returns Reference_Missing on
// failure, which takes the fallback, or makes the value invalid without one. With neither a type nor a fallback, a
// failure gives the empty string instead.
Reference attrValue(const PrDomElement* PR_NULLABLE element, PrStringView first, bool fallback, PrTextBuffer* out) {
    PrStringView arguments = prStringTrim(first);
    // <attr-name>: an identifier, after an optional '|' that names no namespace. A namespace prefix names a namespace
    // that the engine does not declare.
    size_t at = 0;
    if (at < arguments.size && arguments.data[at] == '|')
        ++at;
    size_t nameStart = at;
    while (at < arguments.size &&
            (prAsciiIsLetter(arguments.data[at]) || prAsciiIsDigit(arguments.data[at]) || arguments.data[at] == '-' ||
                arguments.data[at] == '_' || uint8_t(arguments.data[at]) >= 0x80))
        ++at;
    PrStringView name{arguments.data + nameStart, at - nameStart};
    PrStringView type = prStringTrim(PrStringView{arguments.data + at, arguments.size - at});
    bool raw = type.size == 0 || prAsciiEqualsFold(type, "raw-string");
    auto fail = [&]() {
        if (raw && !fallback) {
            prTextBufferAppend(out, "\"\"");
            return Reference_Found;
        }
        return Reference_Missing;
    };
    bool parsed = name.size != 0 && !prAsciiIsDigit(name.data[0]) &&
        (at == arguments.size || prAsciiIsSpace(arguments.data[at]) || type.size == 0);
    if (!parsed || element == nullptr)
        return fail();
    // An HTML element's attribute names are lowercase in an HTML document, and attr() names them as attribute selectors
    // do.
    PrTextBuffer lowered{};
    if (domIsHtmlInHtmlDocument(element)) {
        for (size_t i = 0; i < name.size; ++i)
            prTextBufferAppendChar(&lowered, prAsciiLower(name.data[i]));
        name = prTextBufferView(&lowered);
    }
    PrStringView value = domAttributeValue(element, name);
    Reference result = Reference_Found;
    if (value.data == nullptr) {
        result = fail();
    } else if (raw) {
        cssSerializeString(out, value);
    } else if (prAsciiEqualsFold(type, "number") || prAsciiEqualsFold(type, "%") ||
            cssUnitFromText(type) != PrCssUnit_Unknown) {
        // A number, or a dimension or percentage of the number in the unit.
        PrStringView number = prStringTrim(value);
        if (!isNumberToken(number)) {
            result = fail();
        } else {
            prTextBufferAppend(out, number);
            if (!prAsciiEqualsFold(type, "number"))
                prTextBufferAppend(out, type);
        }
    } else if (type.size > 6 && prAsciiEqualsFold(PrStringView{type.data, 5}, "type(") && type.data[type.size - 1] == ')') {
        // The value as the syntax parses it. <url> is not among the syntax's components.
        CssParser parser;
        cssParserInit(&parser);
        CssSyntaxDefinition syntax{};
        bool valid = cssSyntaxDefinitionParse(&parser, prStringTrim(PrStringView{type.data + 5, type.size - 6}), &syntax);
        for (uint32_t i = 0; valid && i < syntax.components.count; ++i)
            valid = syntax.components.data[i].type != CssValueDef_Url;
        if (valid && cssSyntaxValueMatches(&parser, &syntax, value))
            prTextBufferAppend(out, prStringTrim(value));
        else
            result = fail();
        cssSyntaxDefinitionReset(&syntax);
        cssParserReset(&parser);
    } else {
        result = fail();
    }
    prTextBufferReset(&lowered);
    return result;
}

// Appends `text` to `out`, with each arbitrary substitution function in it replaced: a var() by what `lookup` appends
// for the custom property that it names, and an attr() by the attribute of `lookup.element` that it names, or either by
// its fallback (css-values-5/#substitute-arbitrary-substitution-function). Sets `lookup.tainted` if it replaces an
// attr() (css-values-5/#attr-security). Returns false if a function has neither, or if the result grows past
// kMaxSubstitution (css-values-5/#long-substitution). Either case makes the value invalid.
template <typename Lookup>
bool substitute(PrTextBuffer* out, PrStringView text, Lookup& lookup) {
    size_t i = 0;
    while (i < text.size) {
        size_t length = 0;
        char lead = prAsciiLower(text.data[i]);
        Substitution function = lead == 'v' || lead == 'a' ? substitutionAt(text, i, &length) : Substitution_None;
        if (function == Substitution_None) {
            prTextBufferAppendChar(out, text.data[i++]);
            continue;
        }
        // The arguments, up to the matching parenthesis.
        size_t start = i + length;
        size_t at = start;
        uint32_t nesting = 1;
        size_t comma = 0;
        for (; at < text.size && nesting != 0; ++at) {
            char c = text.data[at];
            if (c == '(')
                ++nesting;
            else if (c == ')')
                --nesting;
            else if (c == ',' && nesting == 1 && comma == 0)
                comma = at;
        }
        size_t close = nesting == 0 ? at - 1 : text.size;
        PrStringView first{text.data + start, (comma != 0 ? comma : close) - start};
        Reference found = Reference_Missing;
        if (function == Substitution_Var) {
            found = lookup(prStringTrim(first), out);
        } else {
            lookup.tainted = true;
            PrTextBuffer arguments{};
            if (!substitute(&arguments, first, lookup)) {
                prTextBufferReset(&arguments);
                return false;
            }
            found = attrValue(lookup.element, prTextBufferView(&arguments), comma != 0, out);
            prTextBufferReset(&arguments);
        }
        if (found == Reference_Invalid)
            return false;
        if (found == Reference_Missing) {
            if (comma == 0 || !substitute(out, PrStringView{text.data + comma + 1, close - comma - 1}, lookup))
                return false;
        }
        if (out->size > kMaxSubstitution)
            return false;
        i = close + 1;
    }
    return true;
}

// Looks references up in computed custom properties, whose values hold no references, and attr() in the attributes of
// `element`, the element styled or a pseudo-element's originating element.
struct ComputedLookup {
    const CssCustomProperties* PR_NULLABLE custom;
    const PrDomElement* PR_NULLABLE element;
    bool tainted;                   // an attr() was replaced

    Reference operator()(PrStringView name, PrTextBuffer* out) const {
        PrStringView value{};
        if (!cssCustomValue(custom, name, &value) || prStringTrim(value).size == 0)
            return Reference_Missing;
        prTextBufferAppend(out, value);
        return Reference_Found;
    }
};

// Returns the value that a winning declaration gives the longhand it declares, with its substitutions made. The result
// is a run of the longhand's value, or the empty run if the declaration is invalid at computed-value time
// (css-values-5/#invalid-at-computed-value-time).
CssValueRun declaredValue(CssCascadeScratch& s, const CssCascaded& cascaded, const CssCustomProperties* PR_NULLABLE custom) {
    PrCssProperty property = cascaded.longhand;
    CssValueRun run = cssDeclarationValue(cascaded.block, cascaded.declaration);
    bool pending = run.count == 1 && run.values[0].kind == PrCssValueKind_Tokens;
    if (!pending)
        return run;
    PrCssProperty declared = cascaded.declaration->property;
    bool parsed = declared != property && s.shorthand == cascaded.declaration;
    prTextBufferClear(&s.text);
    ComputedLookup lookup{custom, s.element, false};
    if (!parsed && !substitute(&s.text, cssValueText(run, run.values), lookup))
        return CssValueRun{};
    s.substituted.values.count = 0;
    s.substituted.text.count = 0;
    // A URL made from an attribute's value makes the declaration invalid (css-values-5/#attr-security).
    auto tainted = [&](CssValueRun value) {
        for (uint32_t i = 0; i < value.count; ++i) {
            if (value.values[i].kind == PrCssValueKind_Url)
                return true;
        }
        return false;
    };
    if (declared == property) {
        if (!cssParseDeclaredValue(cssPropertyDeclaredName(property), prTextBufferView(&s.text), &s.substituted, nullptr))
            return CssValueRun{};
        CssValueRun value = cssValuePoolRun(&s.substituted, CssValueSpan{0, s.substituted.values.count});
        return lookup.tainted && tainted(value) ? CssValueRun{} : value;
    }
    // A shorthand's value is substituted whole, and its longhands are read from the result. Each of the element's
    // longhands that it declares reads the one parse.
    CssDeclarationBlock* block = &s.shorthandBlock;
    if (!parsed) {
        PrStringView text = prTextBufferView(&s.text);
        PrStringView last = prTextBufferView(&s.parsedText);
        if (s.parsedShorthand != declared || text.size != last.size || !prMemEquals(text.data, last.data, text.size)) {
            PrTextBuffer declaration{};
            prTextBufferAppend(&declaration, cssPropertyName(declared));
            prTextBufferAppendChar(&declaration, ':');
            prTextBufferAppend(&declaration, text);
            cssDeclarationBlockParse(block, prTextBufferView(&declaration), nullptr);
            prTextBufferReset(&declaration);
            s.parsedShorthand = declared;
            prTextBufferClear(&s.parsedText);
            prTextBufferAppend(&s.parsedText, text);
        }
        s.shorthand = cascaded.declaration;
        s.shorthandTainted = lookup.tainted;
    }
    uint32_t at = cssDeclarationBlockFind(block, cssPropertyDeclaredName(property));
    CssValueRun longhand{};
    if (at != block->count) {
        CssValuePool* pool = &s.substituted;
        CssValueSpan span = cssValuePoolAddRun(pool, cssDeclarationValue(block, &cssDeclarations(block)[at]));
        longhand = cssValuePoolRun(pool, span);
    }
    return s.shorthandTainted && tainted(longhand) ? CssValueRun{} : longhand;
}

// The state of a custom property's value as references to it are resolved.
enum {
    Resolution_Pending,
    Resolution_Resolving,           // a reference to it now is a cycle (css-values-5/#cyclic-substitution-contexts)
    Resolution_Resolved,
    Resolution_Invalid,             // the guaranteed-invalid value (css-variables-1/#guaranteed-invalid-value)
};
typedef uint8_t Resolution;

// A custom property that the element declares: its name, its declared value, and, once resolved, that value with its
// references substituted.
struct CustomEntry {
    PrStringView name;
    uint32_t hash;
    PrStringView declared;
    Resolution resolution;
    PrTextBuffer resolved;
};

// Resolves references among the element's declared custom properties, each property once. A name that the element does
// not declare takes the parent's computed value, whose references are already substituted.
struct CustomLookup {
    RawArray<CustomEntry>* entries;
    const HashIndex* byName;        // positions in `entries` by name hash
    const CssCustomProperties* PR_NULLABLE parent;
    const PrDomElement* PR_NULLABLE element; // whose attributes attr() reads
    bool tainted;                            // an attr() was replaced

    Reference operator()(PrStringView name, PrTextBuffer* out) {
        auto named = [&](uint32_t k) { return entries->data[k].name == name; };
        uint32_t i = byName->find(cssCustomHash(name), named);
        if (i == kHashMissing)
            return ComputedLookup{parent, element, false}(name, out);
        if (!resolve(i))
            return Reference_Invalid;
        PrStringView value = prTextBufferView(&entries->data[i].resolved);
        if (prStringTrim(value).size == 0)
            return Reference_Missing;
        prTextBufferAppend(out, value);
        return Reference_Found;
    }

    bool resolve(uint32_t index) {
        CustomEntry& entry = entries->data[index];
        if (entry.resolution == Resolution_Resolved)
            return true;
        if (entry.resolution != Resolution_Pending) {
            entry.resolution = Resolution_Invalid;
            return false;
        }
        entry.resolution = Resolution_Resolving;
        PrTextBuffer value{};
        bool valid = substitute(&value, entry.declared, *this);
        // The entry may have moved while references were resolved, so it is found again by index.
        CustomEntry& resolved = entries->data[index];
        if (!valid || resolved.resolution == Resolution_Invalid) {
            prTextBufferReset(&value);
            resolved.resolution = Resolution_Invalid;
            return false;
        }
        resolved.resolved = value;
        resolved.resolution = Resolution_Resolved;
        return true;
    }
};

// Returns the CSS-wide keyword that a custom property's value is, or PrCssKeyword_Count. A custom property's value is
// stored as tokens, so the keyword is recognised from its text: a single identifier
// (css-variables-1/#defining-variables).
PrCssKeyword customWideKeyword(CssValueRun run) {
    PrCssKeyword keyword = wideKeyword(run);
    if (keyword != PrCssKeyword_Count || run.count != 1 || run.values[0].kind != PrCssValueKind_Tokens)
        return keyword;
    PrCssKeyword named = cssKeywordFromText(prStringTrim(cssValueText(run, run.values)));
    return cssIsCssWideKeyword(named) ? named : PrCssKeyword_Count;
}

// Returns the computed custom properties of an element whose parent has the custom properties `parent`. The result is
// the parent's properties with the element's declarations over them, and each declared value has its references
// substituted. See CssCustomProperties for the layering.
const CssCustomProperties* PR_NULLABLE customProperties(CssCascadeScratch& s, CssStyleTable* table,
        const CssCustomProperties* PR_NULLABLE parent) {
    if (s.custom.count == 0) {
        if (parent != nullptr)
            cssCustomPropertiesRetain(parent);
        return parent;
    }
    // Resolve the declared properties.
    RawArray<CustomEntry> entries;
    entries.reserve(s.custom.count);
    for (uint32_t k = 0; k < s.custom.count; ++k) {
        const CssCascaded& c = s.custom.data[k];
        PrStringView name = cssDeclarationName(c.block, c.declaration);
        CssValueRun run = cssDeclarationValue(c.block, c.declaration);
        PrCssKeyword keyword = customWideKeyword(run);
        CustomEntry entry{name, cssCustomHash(name), "", Resolution_Pending, {}};
        if (keyword == PrCssKeyword_Initial) {
            entry.resolution = Resolution_Invalid;
        } else if (keyword != PrCssKeyword_Count) {
            // inherit, unset and revert take the parent's value, because custom properties inherit.
            PrStringView inherited{};
            if (cssCustomValue(parent, name, &inherited)) {
                entry.declared = inherited;
                entry.resolution = Resolution_Resolved;
                prTextBufferAppend(&entry.resolved, inherited);
            } else {
                entry.resolution = Resolution_Invalid;
            }
        } else if (run.count != 0 && cssValueHasText(run.values[0].kind)) {
            entry.declared = cssValueText(run, run.values);
        }
        entries.push(entry);
    }
    CustomLookup lookup{&entries, &s.customIndex, parent, s.element, false};
    for (uint32_t i = 0; i < entries.count; ++i)
        lookup.resolve(i);

    // The declared rows, in entry order.
    RawArray<CssCustomRow> declared;
    declared.reserve(entries.count);
    for (uint32_t i = 0; i < entries.count; ++i) {
        const CustomEntry& e = entries.data[i];
        bool invalid = e.resolution != Resolution_Resolved;
        declared.push(CssCustomRow{e.hash, e.name, invalid ? PrStringView{} : prTextBufferView(&e.resolved), invalid});
    }
    sortItems(declared.span(), [](const CssCustomRow& a, const CssCustomRow& b) {
        return cssCustomBefore(a.hash, a.name, b.hash, b.name);
    });

    // The new set is layered over the base of the parent's chain and holds the parent's own entries. A parent with no
    // base is itself the base. Once the entries above the base outgrow it, the new set is flattened and becomes a base
    // itself.
    const CssCustomProperties* base = parent == nullptr ? nullptr : parent->base != nullptr ? parent->base : parent;
    RawArray<CssCustomRow> inherited;
    if (parent != nullptr && parent->base != nullptr) {
        for (uint32_t i = 0; i < parent->count; ++i) {
            const CssCustomEntry& e = parent->entries[i];
            PrStringView name{parent->text + e.name, e.nameSize};
            bool invalid = e.valueSize == kCssCustomInvalid;
            inherited.push(CssCustomRow{
                e.hash, name,
                invalid ? PrStringView{} : PrStringView{parent->text + e.value, e.valueSize},
                invalid,
            });
        }
    }
    if (base != nullptr && (inherited.count + declared.count) * 2 > base->count + kCustomFlattenSlack) {
        inherited.clear();
        cssCustomEach(parent, [&](const CssCustomRow& row) { inherited.push(row); });
        base = nullptr;
    }

    // Merge the inherited and declared rows. A declared row wins over an inherited row of the same name. An invalid row
    // is kept only to hide the base's entry of its name.
    RawArray<CssCustomRow> rows;
    rows.reserve(inherited.count + declared.count);
    auto add = [&](const CssCustomRow& row) {
        PrStringView ignored{};
        if (!row.invalid || (base != nullptr && cssCustomValue(base, row.name, &ignored)))
            rows.push(row);
    };
    uint32_t i = 0;
    uint32_t d = 0;
    while (i < inherited.count || d < declared.count) {
        if (d == declared.count) {
            add(inherited.data[i++]);
        } else if (i == inherited.count) {
            add(declared.data[d++]);
        } else {
            const CssCustomRow& x = inherited.data[i];
            const CssCustomRow& y = declared.data[d];
            if (cssCustomBefore(x.hash, x.name, y.hash, y.name)) {
                add(x);
                ++i;
            } else {
                if (x.hash == y.hash && x.name == y.name)
                    ++i;
                add(y);
                ++d;
            }
        }
    }
    const CssCustomProperties* made = cssCustomPropertiesCreate(table, base, rows.data, rows.count);
    rows.reset();
    inherited.reset();
    declared.reset();
    for (uint32_t k = 0; k < entries.count; ++k)
        prTextBufferReset(&entries.data[k].resolved);
    entries.reset();
    return made;
}

// Adds the value at `slot` of `group`: the parent's computed value if `inherit` is set and there is a parent, and the
// initial value otherwise. This is the value that an element without a cascaded value takes
// (css-cascade-5/#defaulting).
void addDefault(CssStyleBuilder* builder, CssStyleGroup group, uint32_t slot, bool inherit,
        const CssComputedStyle* PR_NULLABLE parent, const CssStyleValues* initial) {
    if (inherit && parent != nullptr)
        cssStyleAddValue(builder, cssStyleValue(parent->groups[group], slot));
    else
        cssStyleAddValue(builder, cssStyleValue(initial, slot));
}

// Returns true if the element floats: its computed float is not none (CSS2/#float-position).
bool floats(CssCascadeScratch& s, const CssCascadeInput& input, const CssCustomProperties* PR_NULLABLE custom) {
    const CssCascaded& cascaded = s.winners.data[PrCssProperty_Float];
    if (cascaded.declaration == nullptr)
        return false;
    CssValueRun run = declaredValue(s, cascaded, custom);
    PrCssKeyword keyword = run.count == 0 ? PrCssKeyword_Unset : wideKeyword(run);
    if (keyword == PrCssKeyword_Inherit) {
        if (input.parent == nullptr)
            return false;
        run = cssComputedValue(input.parent, PrCssProperty_Float);
    } else if (keyword != PrCssKeyword_Count) {
        return false;
    }
    return run.count == 1 && run.values[0].kind == PrCssValueKind_Keyword &&
        cssValueKeyword(run.values) != PrCssKeyword_None;
}

// Returns true if the element is absolutely positioned: its position cascades to absolute or fixed. Absolute
// positioning blockifies the display and makes the float none (css-display-4/#transformations, CSS2/#dis-pos-flo).
bool outOfFlow(CssCascadeScratch& s, const CssCascadeInput& input, const CssCustomProperties* PR_NULLABLE custom) {
    const CssCascaded& cascaded = s.winners.data[PrCssProperty_Position];
    if (cascaded.declaration == nullptr)
        return false;
    CssValueRun run = declaredValue(s, cascaded, custom);
    PrCssKeyword keyword = run.count == 0 ? PrCssKeyword_Unset : wideKeyword(run);
    if (keyword == PrCssKeyword_Inherit) {
        if (input.parent == nullptr)
            return false;
        run = cssComputedValue(input.parent, PrCssProperty_Position);
    } else if (keyword != PrCssKeyword_Count) {
        return false;
    }
    return run.count == 1 && (cssValueIsKeyword(run.values, PrCssKeyword_Absolute) ||
        cssValueIsKeyword(run.values, PrCssKeyword_Fixed));
}

// Returns the overflow keyword that an axis cascades to before its pair is normalized: the declared keyword, the
// parent's keyword if the axis inherits, or the initial visible. Sets `*inherits` if the axis takes the parent's
// keyword.
PrCssKeyword cascadedOverflow(CssCascadeScratch& s, PrCssProperty property, const CssCascadeInput& input,
        const CssCustomProperties* PR_NULLABLE custom, bool* inherits) {
    const CssCascaded& cascaded = s.winners.data[property];
    if (cascaded.declaration == nullptr)
        return PrCssKeyword_Visible;
    CssValueRun run = declaredValue(s, cascaded, custom);
    PrCssKeyword keyword = run.count == 0 ? PrCssKeyword_Unset : wideKeyword(run);
    if (keyword == PrCssKeyword_Inherit && input.parent != nullptr) {
        *inherits = true;
        run = cssComputedValue(input.parent, property);
    } else if (keyword != PrCssKeyword_Count) {
        return PrCssKeyword_Visible;
    }
    bool single = run.count == 1 && run.values[0].kind == PrCssValueKind_Keyword;
    return single ? cssValueKeyword(run.values) : PrCssKeyword_Visible;
}

// The properties that disable native appearance for widgets (css-ui-4/#appearance-disabling-properties).
constexpr PrCssProperty kDisablingProperties[] = {
    PrCssProperty_BackgroundColor, PrCssProperty_BackgroundImage, PrCssProperty_BackgroundAttachment,
    PrCssProperty_BackgroundPosition, PrCssProperty_BackgroundClip, PrCssProperty_BackgroundOrigin,
    PrCssProperty_BackgroundSize,
    PrCssProperty_BorderTopColor, PrCssProperty_BorderRightColor, PrCssProperty_BorderBottomColor,
    PrCssProperty_BorderLeftColor, PrCssProperty_BorderBlockStartColor, PrCssProperty_BorderBlockEndColor,
    PrCssProperty_BorderInlineStartColor, PrCssProperty_BorderInlineEndColor,
    PrCssProperty_BorderTopStyle, PrCssProperty_BorderRightStyle, PrCssProperty_BorderBottomStyle,
    PrCssProperty_BorderLeftStyle, PrCssProperty_BorderBlockStartStyle, PrCssProperty_BorderBlockEndStyle,
    PrCssProperty_BorderInlineStartStyle, PrCssProperty_BorderInlineEndStyle,
    PrCssProperty_BorderTopWidth, PrCssProperty_BorderRightWidth, PrCssProperty_BorderBottomWidth,
    PrCssProperty_BorderLeftWidth, PrCssProperty_BorderBlockStartWidth, PrCssProperty_BorderBlockEndWidth,
    PrCssProperty_BorderInlineStartWidth, PrCssProperty_BorderInlineEndWidth,
    PrCssProperty_BorderImageSource, PrCssProperty_BorderImageSlice, PrCssProperty_BorderImageWidth,
    PrCssProperty_BorderImageOutset, PrCssProperty_BorderImageRepeat,
    PrCssProperty_BorderTopLeftRadius, PrCssProperty_BorderTopRightRadius, PrCssProperty_BorderBottomRightRadius,
    PrCssProperty_BorderBottomLeftRadius, PrCssProperty_BorderStartStartRadius, PrCssProperty_BorderStartEndRadius,
    PrCssProperty_BorderEndStartRadius, PrCssProperty_BorderEndEndRadius,
};

// Returns true if the author's declarations devolve a widget (css-ui-4/#appearance-disabling-properties): one of them
// wins a property that disables native appearance, once reverts have rolled back. Presentational hints count as the
// author's, since they enter the cascade at the author level (css-cascade-5/#preshint).
bool devolves(const CssCascadeScratch& s) {
    for (PrCssProperty property : kDisablingProperties) {
        const CssCascaded& winner = s.winners.data[property];
        if (winner.declaration != nullptr && winner.origin != CssOrigin_UserAgent)
            return true;
    }
    return false;
}

// Returns the keyword that appearance cascades to: the declared keyword, the parent's computed keyword if the property
// inherits, or the initial none.
PrCssKeyword cascadedAppearance(CssCascadeScratch& s, const CssCascadeInput& input,
        const CssCustomProperties* PR_NULLABLE custom) {
    const CssCascaded& cascaded = s.winners.data[PrCssProperty_Appearance];
    if (cascaded.declaration == nullptr)
        return PrCssKeyword_None;
    CssValueRun run = declaredValue(s, cascaded, custom);
    PrCssKeyword keyword = run.count == 0 ? PrCssKeyword_Unset : wideKeyword(run);
    if (keyword == PrCssKeyword_Inherit && input.parent != nullptr)
        run = cssComputedValue(input.parent, PrCssProperty_Appearance);
    else if (keyword != PrCssKeyword_Count)
        return PrCssKeyword_None;
    if (run.count == 1 && run.values[0].kind == PrCssValueKind_Keyword)
        return cssValueKeyword(run.values);
    return PrCssKeyword_None;
}

// Returns how an element whose appearance cascades to `keyword` is rendered (css-ui-4/#appearance-switching). The
// <compat-auto> and <compat-special> keywords are used as auto. Every widget supports base appearance
// (css-forms-1/#appearance), and base-select gives a select its base appearance and any other widget its native one.
CssAppearance usedAppearance(PrCssKeyword keyword, const PrDomElement* element, DomWidget widget, bool devolved) {
    if (widget == DomWidget_None)
        return CssAppearance_None;
    if (keyword == PrCssKeyword_None)
        return CssAppearance_Primitive;
    if (keyword == PrCssKeyword_Base || (keyword == PrCssKeyword_BaseSelect && domIsHtml(element, PrDomTag_Select)))
        return CssAppearance_Base;
    return widget == DomWidget_Devolvable && devolved ? CssAppearance_Primitive : CssAppearance_Native;
}

// Returns the computed overflow of an axis whose cascaded value is `keyword`, given `other` in the other axis
// (css-overflow-3/#overflow-properties). visible computes to auto and clip to hidden if the other axis is neither
// visible nor clip.
PrCssKeyword computedOverflow(PrCssKeyword keyword, PrCssKeyword other) {
    if (other == PrCssKeyword_Visible || other == PrCssKeyword_Clip)
        return keyword;
    if (keyword == PrCssKeyword_Visible)
        return PrCssKeyword_Auto;
    if (keyword == PrCssKeyword_Clip)
        return PrCssKeyword_Hidden;
    return keyword;
}

// Returns the computed value of `match-parent` for `property`, which is text-align-all or text-align-last
// (css-text-3/#text-align-property). This is the parent's value, except that start and end become left or right
// according to the parent's direction. The root's value is start.
CssValueNode matchParent(const CssComputedStyle* PR_NULLABLE parent, PrCssProperty property) {
    if (parent == nullptr)
        return cssKeywordValue(PrCssKeyword_Start);
    CssValueRun value = cssComputedValue(parent, property);
    PrCssKeyword keyword = value.count == 1 ? cssValueKeyword(value.values) : PrCssKeyword_Start;
    if (keyword != PrCssKeyword_Start && keyword != PrCssKeyword_End)
        return value.values[0];
    CssValueRun direction = cssComputedValue(parent, PrCssProperty_Direction);
    bool rtl = direction.count == 1 && cssValueIsKeyword(direction.values, PrCssKeyword_Rtl);
    return cssKeywordValue((keyword == PrCssKeyword_Start) != rtl ? PrCssKeyword_Left : PrCssKeyword_Right);
}

// Returns the text of the URL that `value` names, if it is a url() or a src() (css-values-4/#urls), and writes the
// index of the value that holds the text to `at`. Returns the null view for any other value.
PrStringView urlText(CssValueRun run, uint32_t index, uint32_t* at) {
    const CssValueNode* value = &run.values[index];
    if (value->kind == PrCssValueKind_Url) {
        *at = index;
        return cssValueText(run, value);
    }
    bool function = value->kind == PrCssValueKind_Function &&
        (cssValueKeyword(value) == PrCssKeyword_Url || cssValueKeyword(value) == PrCssKeyword_Src);
    if (!function || value->next < 2 || value[1].kind != PrCssValueKind_String)
        return PrStringView{};
    *at = index + 1;
    return cssValueText(run, &value[1]);
}

// Returns `run` with each relative URL that its url()s and src()s name resolved against the URL of `sheet`, which is
// the base of the sheet's URLs (css-values-4/#relative-urls). A URL of a fragment alone refers to the document that
// uses it, and stays as written. Returns `run` itself if the sheet was given as text, which leaves its URLs relative to
// the document's base URL.
CssValueRun resolveUrls(CssCascadeScratch& s, CssValueRun run, const CssSheetData* PR_NULLABLE sheet) {
    if (sheet == nullptr || prRawStringIsNull(&sheet->href))
        return run;
    bool any = false;
    for (uint32_t i = 0; i < run.count && !any; ++i) {
        uint32_t at = 0;
        PrStringView text = urlText(run, i, &at);
        any = text.size != 0 && text.data[0] != '#';
    }
    if (!any)
        return run;
    PrUrl* base = nullptr;
    if (prUrlCreate(prRawStringView(&sheet->href), nullptr, PrCharset_Utf8, &base) != PrStatus_Ok) {
        prUrlDestroy(base);
        return run;
    }
    s.urls.values.count = 0;
    s.urls.text.count = 0;
    CssValueSpan span = cssValuePoolAddRun(&s.urls, run);
    for (uint32_t i = 0; i < run.count; ++i) {
        uint32_t at = 0;
        PrStringView text = urlText(run, i, &at);
        if (text.size == 0 || text.data[0] == '#')
            continue;
        PrUrl* url = nullptr;
        if (prUrlCreate(text, base, PrCharset_Utf8, &url) == PrStatus_Ok) {
            PrRawString resolved = prUrlSerialize(url, PrUrlSerialization_Whole);
            PrStringView view = prRawStringView(&resolved);
            CssValueNode& node = s.urls.values.data[span.start + at];
            node.text.start = cssValuePoolAddText(&s.urls, view);
            node.text.length = uint32_t(view.size);
            prRawStringReset(&resolved);
        }
        prUrlDestroy(url);
    }
    prUrlDestroy(base);
    return cssValuePoolRun(&s.urls, span);
}

// Returns true if `property` applies to `pseudo`, which is ::first-line or ::first-letter
// (css-pseudo-4/#first-line-styling, css-pseudo-4/#first-letter-styling). The properties that apply are the font,
// color, background, text and inline layout properties, opacity, and SVG's paints. The margins, padding, borders and
// float also apply to ::first-letter. A declaration of any other property is ignored. writing-mode, direction and
// text-orientation never apply to ::first-line, which takes them from its element. word-break and overflow-wrap apply
// because they break the lines that the pseudo-elements are found in.
bool appliesToFirst(CssPseudo pseudo, PrCssProperty property) {
    bool letter = pseudo == CssPseudo_FirstLetter;
    switch (property) {
        case PrCssProperty_WritingMode:
        case PrCssProperty_Direction:
        case PrCssProperty_TextOrientation: return letter;
        case PrCssProperty_WordBreak:
        case PrCssProperty_OverflowWrap: return false;
        case PrCssProperty_TextDecorationLine:
        case PrCssProperty_TextDecorationStyle:
        case PrCssProperty_TextDecorationColor:
        case PrCssProperty_TextDecorationThickness:
        case PrCssProperty_Opacity:
        case PrCssProperty_AlignmentBaseline:
        case PrCssProperty_BaselineShift:
        case PrCssProperty_BaselineSource: return true;
        case PrCssProperty_BorderTopLeftRadius:
        case PrCssProperty_BorderTopRightRadius:
        case PrCssProperty_BorderBottomRightRadius:
        case PrCssProperty_BorderBottomLeftRadius:
        case PrCssProperty_MarginTop:
        case PrCssProperty_MarginRight:
        case PrCssProperty_MarginBottom:
        case PrCssProperty_MarginLeft:
        case PrCssProperty_PaddingTop:
        case PrCssProperty_PaddingRight:
        case PrCssProperty_PaddingBottom:
        case PrCssProperty_PaddingLeft:
        case PrCssProperty_BorderTopWidth:
        case PrCssProperty_BorderRightWidth:
        case PrCssProperty_BorderBottomWidth:
        case PrCssProperty_BorderLeftWidth:
        case PrCssProperty_Float: return letter;
        default: break;
    }
    switch (cssStyleSlot(property).group) {
        case CssStyleGroup_Font:
        case CssStyleGroup_Color:
        case CssStyleGroup_Background:
        case CssStyleGroup_Svg:
        case CssStyleGroup_Typography: return true;
        case CssStyleGroup_Decoration: return letter;
        default: return false;
    }
}

// Computes one group of the element's style.
const CssStyleValues* buildGroup(CssStyleBuilder* builder, CssCascadeScratch& s, CssStyleGroup group,
        const CssCascadeInput& input, CssStyleContext& context, const CssCustomProperties* PR_NULLABLE custom) {
    CssStyleGroupProperties properties = cssStyleGroupProperties(group);
    bool inherited = cssStyleGroupInherited(group);
    bool any = false;
    for (uint32_t i = 0; i < properties.count && !any && (s.declaredGroups & (1u << group)) != 0; ++i)
        any = s.winners.data[properties.properties[i]].declaration != nullptr;
    bool root = input.parent == nullptr;
    bool item = !root && input.parent->childrenAreItems;
    // On ::before and ::after, `content: normal` computes to none (css-content-3/#valdef-content-normal).
    bool generates = input.pseudo == CssPseudo_Before || input.pseudo == CssPseudo_After;
    if (!any && !((root || item) && group == CssStyleGroup_Box) && !(generates && group == CssStyleGroup_Generated)) {
        const CssStyleValues* shared;
        if (inherited && input.parent != nullptr)
            shared = input.parent->groups[group];
        else
            shared = cssStyleInitial(builder, group);
        if (inherited && input.parent != nullptr) {
            cssStyleValuesRetain(shared);
            s.passThrough |= uint16_t(1u << group);
        }
        return shared;
    }
    const CssStyleValues* initial = cssStyleInitial(builder, group);
    // The root's box is blockified (css-display-4/#root). So are a float, an absolutely positioned box, and a flex or
    // grid item (css-display-4/#transformations). An absolutely positioned box's float computes to none
    // (CSS2/#dis-pos-flo). An absolutely positioned box keeps whether its display was inline-level, which determines
    // its static position.
    bool positioned = !root && group == CssStyleGroup_Box && outOfFlow(s, input, custom);
    bool blockify = root || positioned || (group == CssStyleGroup_Box && (item || floats(s, input, custom)));
    // The overflow pair is computed together, because each axis's value depends on the other's.
    PrCssKeyword overflowX = PrCssKeyword_Visible;
    PrCssKeyword overflowY = PrCssKeyword_Visible;
    if (group == CssStyleGroup_Box) {
        bool inherits = false;
        PrCssKeyword x = cascadedOverflow(s, PrCssProperty_OverflowX, input, custom, &inherits);
        PrCssKeyword y = cascadedOverflow(s, PrCssProperty_OverflowY, input, custom, &inherits);
        overflowX = computedOverflow(x, y);
        overflowY = computedOverflow(y, x);
        if (inherits)
            s.inheritsReset = true;
    }
    // Computes the property of slot `i` into the builder.
    auto add = [&](uint32_t i, PrCssProperty property) {
        const CssCascaded& cascaded = s.winners.data[property];
        if (property == PrCssProperty_OverflowX || property == PrCssProperty_OverflowY) {
            CssValueNode overflow = cssKeywordValue(property == PrCssProperty_OverflowX ? overflowX : overflowY);
            cssStyleAddValue(builder, CssValueRun{&overflow, 1, nullptr});
            return;
        }
        if (positioned && property == PrCssProperty_Float) {
            CssValueNode none = cssKeywordValue(PrCssKeyword_None);
            cssStyleAddValue(builder, CssValueRun{&none, 1, nullptr});
            return;
        }
        if (cascaded.declaration == nullptr) {
            if (blockify && property == PrCssProperty_Display) {
                s.staticInline = positioned;
                CssValueNode block = cssKeywordValue(PrCssKeyword_Block);
                cssStyleAddValue(builder, CssValueRun{&block, 1, nullptr});
                return;
            }
            addDefault(builder, group, i, inherited, input.parent, initial);
            return;
        }
        CssValueRun run = declaredValue(s, cascaded, custom);
        run = resolveUrls(s, run, cascaded.sheet);
        PrCssKeyword keyword = run.count == 0 ? PrCssKeyword_Unset : wideKeyword(run);
        if (keyword != PrCssKeyword_Count && blockify && property == PrCssProperty_Display) {
            // The root has no parent to inherit from, so every CSS-wide keyword gives the initial inline, blockified. A
            // float takes its parent's display, blockified, if it inherits, and otherwise the initial inline.
            PrCssKeyword blockLevel = PrCssKeyword_Block;
            s.staticInline = positioned;
            if (!root && keyword == PrCssKeyword_Inherit) {
                s.inheritsReset = true;
                CssValueRun parent = cssComputedValue(input.parent, PrCssProperty_Display);
                if (!blockified(parent, &blockLevel)) {
                    s.staticInline = false;
                    cssStyleAddValue(builder, parent);
                    return;
                }
            }
            CssValueNode block = cssKeywordValue(blockLevel);
            cssStyleAddValue(builder, CssValueRun{&block, 1, nullptr});
            return;
        }
        // `color: currentcolor` is `color: inherit` (css-color-4/#resolving-color-values), so the element takes its
        // parent's computed color whole, with whatever range or gamut that color resolves to where it is used.
        if (property == PrCssProperty_Color && run.count == 1 &&
                cssValueIsKeyword(run.values, PrCssKeyword_Currentcolor))
            keyword = PrCssKeyword_Inherit;
        if (keyword != PrCssKeyword_Count) {
            bool inherit = keyword == PrCssKeyword_Inherit || (keyword != PrCssKeyword_Initial && inherited);
            if (inherit && !inherited)
                s.inheritsReset = true;
            addDefault(builder, group, i, inherit, input.parent, initial);
            return;
        }
        PrCssKeyword blockLevel = PrCssKeyword_Unknown;
        // Contents computes to block on the root, because it would give the root no box (css-display-4/#root).
        if (root && property == PrCssProperty_Display && run.count == 1 &&
                cssValueIsKeyword(run.values, PrCssKeyword_Contents)) {
            CssValueNode block = cssKeywordValue(PrCssKeyword_Block);
            cssStyleAddValue(builder, CssValueRun{&block, 1, nullptr});
            return;
        }
        if (blockify && property == PrCssProperty_Display && blockified(run, &blockLevel)) {
            s.staticInline = positioned;
            CssValueNode block = cssKeywordValue(blockLevel);
            cssStyleAddValue(builder, CssValueRun{&block, 1, nullptr});
            return;
        }
        // On anything that is not a MathML element, math and inline math compute to inline, and block math to block
        // (mathml-core/#new-display-math-value).
        if (property == PrCssProperty_Display && (input.pseudo != CssPseudo_None ||
                input.element->space != PrDomNamespace_MathMl)) {
            bool math = false;
            bool block = false;
            for (uint32_t k = 0; k < run.count; ++k) {
                math = math || cssValueIsKeyword(run.values + k, PrCssKeyword_Math);
                block = block || cssValueIsKeyword(run.values + k, PrCssKeyword_Block);
            }
            if (math) {
                CssValueNode flow = cssKeywordValue(block ? PrCssKeyword_Block : PrCssKeyword_Inline);
                CssValueRun flowRun{&flow, 1, nullptr};
                if (blockify && blockified(flowRun, &blockLevel))
                    flow = cssKeywordValue(blockLevel);
                cssStyleAddValue(builder, CssValueRun{&flow, 1, nullptr});
                return;
            }
        }
        if ((property == PrCssProperty_TextAlignAll || property == PrCssProperty_TextAlignLast) &&
                run.count == 1 && cssValueIsKeyword(run.values, PrCssKeyword_MatchParent)) {
            CssValueNode matched = matchParent(input.parent, property);
            cssStyleAddValue(builder, CssValueRun{&matched, 1, nullptr});
            return;
        }
        cssStyleAddComputed(builder, property, run, context);
    };
    cssStyleBegin(builder);
    for (uint32_t i = 0; i < properties.count; ++i) {
        PrCssProperty property = properties.properties[i];
        add(i, property);
        if (generates && property == PrCssProperty_Content) {
            CssValueRun content = cssStyleLastValue(builder);
            if (content.count == 1 && cssValueIsKeyword(content.values, PrCssKeyword_Normal)) {
                builder->pool.values.count = builder->spans.last().start;
                --builder->spans.count;
                CssValueNode none = cssKeywordValue(PrCssKeyword_None);
                cssStyleAddValue(builder, CssValueRun{&none, 1, nullptr});
            }
        }
        // The element's own font size and line height are what the later values of the font group, and the values of
        // the other groups, are relative to. Once computed, the root's font size and line height are also the bases of
        // its root-relative lengths: `rem` after `font-size`, and `rlh` after `line-height`
        // (css-values-4/#font-relative-lengths).
        if (group != CssStyleGroup_Font)
            continue;
        if (property == PrCssProperty_FontSize) {
            context.font =
                cssFontBases(cssStyleLastValue(builder).values[0].number, context.font.lh, context.font.style);
            if (root)
                context.root = cssFontBases(context.font.em, context.root.lh, context.root.style);
        } else if (property == PrCssProperty_LineHeight) {
            context.font = cssFontBases(context.font.em, cssLineHeightPx(cssStyleLastValue(builder), context.font.em),
                context.font.style);
            if (root)
                context.root = context.font;
        }
    }
    cssStyleValuesRelease(initial);
    return cssStyleEnd(builder, group);
}

// Appends the bits of each base to `key`.
void addBases(RawArray<uint64_t>& key, const CssFontBases& bases) {
    key.push(bitCast<uint64_t>(bases.em));
    key.push(bitCast<uint64_t>(bases.lh));
}

// Returns true if each declaration that won a property of the inherited `group` gives the property its initial value
// under a parent whose group is the initial one. Such a declaration is a CSS-wide keyword other than a revert, or the
// keyword that `initial`, the initial group, holds. The `font` shorthand resets the longhands that it does not set with
// such declarations.
bool declaresInitial(CssCascadeScratch& s, CssStyleGroupProperties properties, const CssStyleValues* initial,
        const CssCustomProperties* PR_NULLABLE custom) {
    for (uint32_t i = 0; i < properties.count; ++i) {
        const CssCascaded& cascaded = s.winners.data[properties.properties[i]];
        if (cascaded.declaration == nullptr)
            continue;
        CssValueRun run = declaredValue(s, cascaded, custom);
        if (run.count == 0)
            continue;
        PrCssKeyword keyword = wideKeyword(run);
        if (keyword == PrCssKeyword_Initial || keyword == PrCssKeyword_Inherit || keyword == PrCssKeyword_Unset)
            continue;
        if (keyword != PrCssKeyword_Count || run.count != 1 || run.values[0].kind != PrCssValueKind_Keyword)
            return false;
        CssValueRun value = cssStyleValue(initial, i);
        if (value.count != 1 || value.values[0].kind != PrCssValueKind_Keyword ||
                cssValueKeyword(value.values) != cssValueKeyword(run.values))
            return false;
    }
    return true;
}

// Computes one group of the element's style, or takes it from the cache of computed groups. The key holds what the
// group's values can read: the declarations that won its properties, the parent's group if the group inherits, the font
// bases, the viewport's pixel ratio, the custom properties, and what makes a box blockified or a display of math.
const CssStyleValues* computeGroup(CssStyleBuilder* builder, CssCascadeScratch& s, CssStyleGroup group,
        const CssCascadeInput& input, CssStyleContext& context, const CssCustomProperties* PR_NULLABLE custom) {
    // A group of a kind that does not inherit, with no declaration, is the initial one, unless the element's own
    // position decides one of its values. Most elements declare nothing of most such groups.
    bool special = group == CssStyleGroup_Box || group == CssStyleGroup_Generated;
    if ((s.declaredGroups & (1u << group)) == 0 && !cssStyleGroupInherited(group) && !special)
        return cssStyleInitial(builder, group);
    CssStyleGroupProperties properties = cssStyleGroupProperties(group);
    bool inherited = cssStyleGroupInherited(group);
    bool any = false;
    for (uint32_t i = 0; i < properties.count && !any && (s.declaredGroups & (1u << group)) != 0; ++i)
        any = s.winners.data[properties.properties[i]].declaration != nullptr;
    // A group with no declaration is shared, or the initial one, and costs nothing to compute.
    if (!s.cacheGroups || !any)
        return buildGroup(builder, s, group, input, context, custom);
    // An inherited group whose declarations only restate the initial values under a parent with the initial group is
    // the initial group, which it takes without building or hashing.
    if (inherited && input.parent != nullptr) {
        const CssStyleValues* initial = cssStyleInitial(builder, group);
        if (input.parent->groups[group] == initial && declaresInitial(s, properties, initial, custom))
            return initial;
        cssStyleValuesRelease(initial);
    }
    bool root = input.parent == nullptr;
    RawArray<uint64_t>& key = s.groupKey;
    key.clear();
    key.push(uint64_t(group) | uint64_t(input.pseudo) << 8 | uint64_t(root) << 16 |
        uint64_t(!root && input.parent->childrenAreItems) << 17 |
        uint64_t(input.element->space == PrDomNamespace_MathMl) << 18);
    key.push(uint64_t(uintptr_t(inherited && !root ? input.parent->groups[group] : nullptr)));
    addBases(key, context.font);
    addBases(key, context.root);
    key.push(uint64_t(uintptr_t(custom)));
    key.push(s.groupSalt);
    for (uint32_t i = 0; i < properties.count; ++i) {
        const CssCascaded& winner = s.winners.data[properties.properties[i]];
        key.push(uint64_t(uintptr_t(winner.declaration)) ^ uint64_t(winner.longhand) << 48);
    }
    uint64_t hash = prHashBytes(key.data, sizeof(uint64_t) * key.count, kMatchedSeed) | 1;
    if (s.cachedGroups.count == 0)
        s.cachedGroups.resizeZeroed(kCssCachedGroups);
    CssCachedGroup& cached = s.cachedGroups.data[hash % kCssCachedGroups];
    const CssStyleValues* parentValues = root ? nullptr : input.parent->groups[group];
    bool hit = cached.hash == hash && cached.key.count == key.count &&
        prMemEquals(cached.key.data, key.data, sizeof(uint64_t) * key.count) &&
        (!cached.inheritsReset || cached.parentValues == parentValues) && (!cached.readsViewport ||
            (cached.viewport.width == input.viewport.width && cached.viewport.height == input.viewport.height)) &&
        !cached.readsMetrics;
    if (hit) {
        s.inheritsReset = s.inheritsReset || cached.inheritsReset;
        if (group == CssStyleGroup_Box)
            s.staticInline = cached.staticInline;
        if (cached.readsViewport)
            *context.readsViewport = true;
        if (cached.readsFont && context.readsFont != nullptr)
            *context.readsFont = true;
        context.font = cssFontBases(cached.font.em, cached.font.lh, context.font.style);
        context.root = cssFontBases(cached.root.em, cached.root.lh, context.root.style);
        cssStyleValuesRetain(cached.values);
        return cached.values;
    }
    // Computes the group with the cascade's flags cleared, to learn what this group sets.
    bool inheritsReset = s.inheritsReset;
    bool readsViewport = *context.readsViewport;
    bool readsFont = context.readsFont != nullptr && *context.readsFont;
    s.inheritsReset = false;
    *context.readsViewport = false;
    if (context.readsFont != nullptr)
        *context.readsFont = false;
    if (context.metrics != nullptr)
        context.metrics->readsGroup = false;
    const CssStyleValues* values = buildGroup(builder, s, group, input, context, custom);
    cssStyleValuesRelease(cached.values);
    cssStyleValuesRelease(cached.parentValues);
    cached.hash = hash;
    cached.key.clear();
    cached.key.append(key.span());
    cssStyleValuesRetain(values);
    cached.values = values;
    cached.inheritsReset = s.inheritsReset;
    cached.parentValues = s.inheritsReset ? parentValues : nullptr;
    if (cached.parentValues != nullptr)
        cssStyleValuesRetain(cached.parentValues);
    cached.viewport = input.viewport;
    cached.font = context.font;
    cached.root = context.root;
    cached.staticInline = s.staticInline;
    cached.readsViewport = *context.readsViewport;
    cached.readsFont = context.readsFont != nullptr && *context.readsFont;
    cached.readsMetrics = context.metrics != nullptr && context.metrics->readsGroup;
    s.inheritsReset = s.inheritsReset || inheritsReset;
    *context.readsViewport = *context.readsViewport || readsViewport;
    if (context.readsFont != nullptr)
        *context.readsFont = *context.readsFont || readsFont;
    return values;
}

// Records that a change to an attribute that the declarations of an entry from `first` on read through attr() restyles
// the element that carries it.
void fileAttrReads(CssRuleSet* set, uint32_t first) {
    const CssDeclarationBlock* last = nullptr;
    for (uint32_t i = first; i < set->entries.count; ++i) {
        const CssRuleEntry& entry = set->entries.data[i];
        if (!entry.attributes || entry.block == last)
            continue;
        last = entry.block;
        eachAttrRead(entry.block, [&](PrStringView name) {
            if (name.size != 0)
                invalidate(set, cssFeatureHash(CssFeature_Attribute, name), Reach{true, false, false, false, 0});
            else
                set->reads.attributes = true;
        });
    }
}

// Copies the entries of `from` into the empty `to`, with their buckets.
void copyIndex(CssRuleIndex& to, const CssRuleIndex& from) {
    to.buckets.reserve(from.buckets.count);
    for (uint32_t i = 0; i < from.buckets.count; ++i) {
        const CssRuleBucket& bucket = from.buckets.data[i];
        CssRuleBucket& made = to.buckets.emplace();
        made = CssRuleBucket{};
        made.hash = bucket.hash;
        made.entries.append(bucket.entries.span());
        to.index.insert(bucket.hash, i);
    }
    to.universal.append(from.universal.span());
}

// Copies the rule set `from` into the empty `to`, which then holds the same entries for the same document. The user
// agent's sheets that `from` files declare no layers and no keyframes.
void copyRuleSet(CssRuleSet* to, const CssRuleSet* from) {
    PR_ASSERT(from->layers.count == 0 && from->keyframes.count == 0 && from->anchors.anchors.count == 0 &&
        from->counterStyleRules.count == 0);
    to->document = from->document;
    to->entries.append(from->entries.span());
    for (uint32_t p = 0; p < CssPseudo_Count; ++p)
        copyIndex(to->filed[p], from->filed[p]);
    copyIndex(to->generating, from->generating);
    to->lists.append(from->lists.span());
    to->invalidations.reserve(from->invalidations.count);
    for (uint32_t i = 0; i < from->invalidations.count; ++i) {
        const CssInvalidation& invalidation = from->invalidations.data[i];
        CssInvalidation& made = to->invalidations.emplace();
        made = CssInvalidation{};
        made.hash = invalidation.hash;
        made.qualifier = invalidation.qualifier;
        made.next = invalidation.next;
        made.self = invalidation.self;
        made.laterSiblings = invalidation.laterSiblings;
        made.siblings = invalidation.siblings;
        made.subtree = invalidation.subtree;
        made.descendants.append(invalidation.descendants.span());
        for (uint32_t k = 0; k < invalidation.anchors.anchors.count; ++k)
            cssHasAnchorsAdd(&made.anchors, invalidation.anchors.anchors.data[k]);
        // The head of each chain comes before the rest of the chain, and the index holds the heads.
        if (invalidationAt(to, made.hash) == kHashMissing)
            to->invalidationIndex.insert(made.hash, i);
    }
    to->media.append(from->media.span());
    to->reads = from->reads;
}

} // namespace

uint32_t cssFeatureHash(CssFeature kind, PrStringView name) {
    uint32_t hash = uint32_t(nameHashLower(name.data, name.size, kFeatureSeed + kind));
    return hash != 0 ? hash : 1;
}

void cssHasAnchorsAdd(CssHasAnchors* set, const CssHasAnchor& anchor) {
    auto same = [&](const CssHasAnchor& a) {
        return a.feature == anchor.feature && a.self == anchor.self && a.laterSiblings == anchor.laterSiblings &&
            a.siblings == anchor.siblings && a.subtree == anchor.subtree && a.descendant == anchor.descendant &&
            a.previous == anchor.previous;
    };
    uint32_t first = kHashMissing;
    if (anchor.feature == 0) {
        for (uint32_t i = 0; i < set->anyElement.count; ++i) {
            if (same(set->anchors.data[set->anyElement.data[i]]))
                return;
        }
    } else {
        first = set->byFeature.find(anchor.feature,
            [&](uint32_t at) { return set->anchors.data[at].feature == anchor.feature; });
        for (uint32_t at = first; at != kHashMissing; at = set->nextOfFeature.data[at]) {
            if (same(set->anchors.data[at]))
                return;
        }
    }
    uint32_t at = set->anchors.count;
    set->anchors.push(anchor);
    // A new anchor of a feature that has some is the first of its chain, which the index finds in place of the old one.
    set->nextOfFeature.push(first);
    if (anchor.feature == 0) {
        set->anyElement.push(at);
    } else {
        if (first != kHashMissing)
            set->byFeature.erase(anchor.feature, first);
        set->byFeature.insert(anchor.feature, at);
    }
    if (anchor.previous)
        set->previous.push(at);
}

void cssHasAnchorsReset(CssHasAnchors* set) {
    set->anchors.reset();
    set->nextOfFeature.reset();
    set->byFeature.reset();
    set->anyElement.reset();
    set->previous.reset();
}

bool cssBlockReadsAttributes(const CssDeclarationBlock* block) {
    bool reads = false;
    eachAttrRead(block, [&](PrStringView) { reads = true; });
    return reads;
}

const CssRuleData* PR_NULLABLE cssRuleSetKeyframes(const CssRuleSet* set, PrStringView name) {
    for (uint32_t i = 0; i < set->keyframes.count; ++i) {
        if (prRawStringView(&set->keyframes.data[i].name) == name)
            return set->keyframes.data[i].rule;
    }
    return nullptr;
}

const CssInvalidation* PR_NULLABLE cssRuleSetInvalidation(const CssRuleSet* set, uint32_t hash) {
    uint32_t at = invalidationAt(set, hash);
    return at != kHashMissing ? &set->invalidations.data[at] : nullptr;
}

bool cssRuleSubjectFeatures(const CssRuleData* rule, RawArray<uint32_t>* out) {
    if (rule->kind == CssRuleDataKind_Style) {
        // A nested rule's selectors are relative to its parent's, which the engine does not yet resolve.
        const PrCssSelectors* selectors = rule->selectors;
        if (selectors == nullptr || rule->children.count != 0)
            return false;
        for (uint32_t i = 0; i < selectors->list.count; ++i) {
            const CssComplexSelector* c = &selectors->complex[selectors->list.first + i];
            if ((c->flags & CssComplexFlag_Invalid) != 0)
                continue;
            uint32_t feature = subjectFeature(selectors, &selectors->compounds[c->compound]);
            if (feature == 0)
                return false;
            out->push(feature);
        }
        return true;
    }
    bool holds = rule->kind == CssRuleDataKind_At && rule->block &&
        (rule->atRule == CssAtRule_Media || rule->atRule == CssAtRule_Supports || rule->atRule == CssAtRule_Layer);
    if (!holds)
        return false;
    for (uint32_t i = 0; i < rule->children.count; ++i) {
        if (!cssRuleSubjectFeatures(rule->children.data[i], out))
            return false;
    }
    return true;
}

void cssRuleSetBuildUserAgent(CssRuleSet* set, const PrDomDocument* document,
        const CssParsedUserAgentSheet* PR_NULLABLE userAgent, uint32_t userAgentCount,
        PrCssSheetConditions conditions, const CssMediaEnvironment* PR_NULLABLE media) {
    uint32_t generation = set->generation;
    cssRuleSetReset(set);
    set->generation = generation + 1;
    set->document = document;
    Filing f{.set = set, .document = document, .media = media, .origin = CssOrigin_UserAgent};
    for (uint32_t i = 0; i < userAgentCount; ++i) {
        if ((userAgent[i].conditions & ~conditions) != 0)
            continue;
        f.sheet = userAgent[i].data;
        f.appearance = userAgent[i].appearance;
        fileRules(f, userAgent[i].data->rules, kCssUnlayered, PrStringView{});
    }
    fileAttrReads(set, 0);
    prTextBufferReset(&f.name);
    f.types.reset();
}

void cssRuleSetBuild(CssRuleSet* set, const CssRuleSet* base, const CssTreeSheets* PR_NULLABLE trees,
        uint32_t treeCount, const CssMediaEnvironment* PR_NULLABLE media) {
    uint32_t generation = set->generation;
    cssRuleSetReset(set);
    copyRuleSet(set, base);
    set->generation = generation + 1;
    Filing f{
        .set = set,
        .document = base->document,
        .media = media,
        .origin = CssOrigin_Author,
        .order = base->entries.count,
    };
    set->scoped = base->document != nullptr && base->document->slots != nullptr && base->document->slots->scopes;
    for (uint32_t t = 0; t < treeCount; ++t) {
        f.scope = trees[t].tree;
        f.depth = trees[t].depth;
        for (uint32_t i = 0; i < trees[t].count; ++i) {
            const PrCssStyleSheet* sheet = trees[t].sheets[i];
            if (sheet->disabled || (media != nullptr && !matchesMedia(f, &sheet->media)))
                continue;
            f.sheet = sheet->data;
            fileRules(f, sheet->data->rules, kCssUnlayered, PrStringView{});
        }
    }
    fileAttrReads(set, base->entries.count);
    cssCounterStylesBuild(&set->counterStyles, set->counterStyleRules.data, set->counterStyleRules.count,
        media != nullptr ? media->fontSize : 16);
    prTextBufferReset(&f.name);
    f.types.reset();
}

bool cssRuleSetMediaChanged(const CssRuleSet* set, const CssMediaEnvironment* media) {
    for (uint32_t i = 0; i < set->media.count; ++i) {
        if (cssMediaListMatches(set->media.data[i].list, media) != set->media.data[i].matched)
            return true;
    }
    return false;
}

void cssRuleSetReset(CssRuleSet* set) {
    set->entries.reset();
    set->media.reset();
    auto resetIndex = [](CssRuleIndex& filed) {
        for (uint32_t i = 0; i < filed.buckets.count; ++i)
            filed.buckets.data[i].entries.reset();
        filed.buckets.reset();
        filed.index.reset();
        filed.universal.reset();
    };
    for (CssRuleIndex& filed : set->filed)
        resetIndex(filed);
    resetIndex(set->generating);
    set->slotted.reset();
    set->lists.reset();
    set->document = nullptr;
    for (uint32_t i = 0; i < set->layers.count; ++i)
        prRawStringReset(&set->layers.data[i]);
    set->layers.reset();
    for (uint32_t i = 0; i < set->keyframes.count; ++i)
        prRawStringReset(&set->keyframes.data[i].name);
    set->keyframes.reset();
    set->counterStyleRules.reset();
    cssCounterStylesReset(&set->counterStyles);
    for (uint32_t i = 0; i < set->invalidations.count; ++i) {
        set->invalidations.data[i].descendants.reset();
        cssHasAnchorsReset(&set->invalidations.data[i].anchors);
    }
    cssHasAnchorsReset(&set->anchors);
    set->invalidations.reset();
    set->invalidationIndex.reset();
    set->reads = CssRuleReads{};
}

void cssCascadeMemoClear(CssCascadeMemo* memo) {
    cssComputedStyleRelease(memo->style);
    cssComputedStyleRelease(memo->parent);
    memo->style = nullptr;
    memo->parent = nullptr;
    memo->matched.clear();
}

void cssCascadeMemoReset(CssCascadeMemo* memo) {
    cssCascadeMemoClear(memo);
    memo->matched.reset();
    *memo = CssCascadeMemo{};
}

const CssComputedStyle* cssCascadeStyle(const CssRuleSet* set, CssStyleBuilder* builder, CssCascadeScratch* scratch,
        const CssCascadeInput& input) {
    CssCascadeScratch& s = *scratch;
    follow(s, set);
    if (s.winners.count == 0) {
        s.winners.reserve(PrCssProperty_Count);
        s.winners.count = PrCssProperty_Count;
        prMemZero(s.winners.data, sizeof(CssCascaded) * PrCssProperty_Count);
    }
    s.element = input.element;
    s.inheritsReset = false;
    s.passThrough = 0;
    s.staticInline = false;

    if (input.matched != nullptr) {
        s.candidates.clear();
        s.candidates.append(input.matched->span());
    } else {
        match(set, s, input.element, set->filed[input.pseudo]);
    }
    // An element's inline style and presentational hints are not its pseudo-elements', and are rolled back with the
    // author origin.
    bool own = input.pseudo == CssPseudo_None && !input.userAgentOnly;
    const CssDeclarationBlock* inlineStyle = nullptr;
    if (own && domAttributeValue(input.element, "style").data != nullptr) {
        PR_ASSERT(input.element->style != nullptr && input.element->style->inlineStyle != nullptr);
        inlineStyle = &input.element->style->inlineStyle->block;
    }
    // Presentational hints precede every author declaration (css-cascade-5/#preshint). The caller built them. The hints
    // that another element gives are shared, so a style that applies them alone may be cached.
    const CssElementStyle* elementStyle = input.element->style;
    const CssDeclarationBlock* hints = own && elementStyle != nullptr && elementStyle->hintsBuilt ? elementStyle->hints : nullptr;
    uint64_t sharedId = 0;
    const CssDeclarationBlock* shared = own ? cssSharedHints(input.element, &sharedId) : nullptr;
    // An element's used appearance follows from its entries and its kind of widget, and a pseudo-element's from what
    // originates it.
    DomWidget widget = input.pseudo == CssPseudo_None ? domElementWidget(input.element) : DomWidget(DomWidget_None);
    CssAppearance parentAppearance = input.pseudo == CssPseudo_None ? CssAppearance_None : input.parent->appearance;

    // An element whose declarations all come from the rule set uses a style cached for the same entries.
    PrLocale locale = input.pseudo == CssPseudo_None ? localeOf(input) : input.parent->locale;
    CssMatchedStyle* slot = nullptr;
    uint32_t hash = 0;
    // A style that reads attributes through attr() depends on more than the entries. Entries for one appearance apply
    // only once the cascade has decided the element's used appearance.
    bool attributes = false;
    uint32_t conditions = 0;        // a bit for each used appearance that a candidate applies under, by CssAppearance
    for (uint32_t i = 0; i < s.candidates.count; ++i) {
        const CssRuleEntry& entry = set->entries.data[s.candidates.data[i]];
        attributes = attributes || entry.attributes;
        conditions |= entry.appearance != CssAppearance_None ? 1u << entry.appearance : 0;
    }
    bool cacheable = !input.fresh && !input.userAgentOnly && inlineStyle == nullptr && hints == nullptr && !attributes;
    CssCascadeMemo* memo = cacheable ? input.memo : nullptr;
    if (input.memo != nullptr && memo == nullptr)
        cssCascadeMemoClear(input.memo);
    if (memo != nullptr && remembers(*memo, set, s, input, sharedId, widget, locale)) {
        ++s.matchedHits;
        if (memo->readsViewport)
            s.readsViewport = true;
        cssComputedStyleRetain(memo->style);
        return memo->style;
    }
    if (cacheable) {
        uint64_t salt = input.pseudo + (uint64_t(widget) << 8) + (uint64_t(parentAppearance) << 16) +
            sharedId * 0x9e3779b97f4a7c15ull;
        hash = uint32_t(prHashBytes(s.candidates.data, sizeof(uint32_t) * s.candidates.count, kMatchedSeed + salt)) | 1;
        CssMatchedStyle* candidates = matchedSet(s, hash);
        ++s.matchedLookups;
        for (uint32_t way = 0; way < kCssMatchedWays; ++way) {
            CssMatchedStyle& cached = candidates[way];
            Fit fit = fits(cached, hash, s, input, sharedId, widget, parentAppearance);
            if (fit == Fit_None)
                continue;
            cached.used = s.matchedLookups;
            ++s.matchedHits;
            if (cached.readsViewport)
                s.readsViewport = true;
            const CssComputedStyle* style = nullptr;
            if (fit == Fit_Rebase) {
                style = rebased(cached, input.parent, locale);
            } else if (cached.style->locale != locale) {
                style = withLocale(cached.style, locale);
            } else {
                cssComputedStyleRetain(cached.style);
                style = cached.style;
            }
            if (memo != nullptr)
                remember(memo, set, s, input, style, sharedId, widget, cached.readsViewport);
            return style;
        }
        slot = &matchedVictim(candidates, hash);
    }
    // A group of the style may come from the cache of computed groups if the style may come from the matched properties
    // cache. The shared hints' number keys the groups too, since a block of hints is rebuilt in place.
    s.cacheGroups = slot != nullptr;
    s.groupSalt = sharedId;

    // The cascade sort (css-cascade-5/#cascade-sort): normal declarations of each origin, the style attribute's after
    // the author's, then important declarations in the reverse order. The hints are the whole of the hint origin. The
    // element's style attribute follows every author layer.
    CssHintBlocks blocks{shared, hints};
    CssPlace hinted{set, blocks, CssOrigin_Hint, 0, nullptr};
    CssPlace attached{set, blocks, CssOrigin_Author, uint32_t(kCssUnlayered) + 1, nullptr};
    auto cascade = [&]() {
        forgetWinners(s);
        s.applying = s.candidates.data;
        s.applyingCount = s.candidates.count;
        if (conditions != 0) {
            s.filtered.clear();
            for (uint32_t i = 0; i < s.candidates.count; ++i) {
                CssAppearance appearance = set->entries.data[s.candidates.data[i]].appearance;
                if (appearance == CssAppearance_None || appearance == s.appearance)
                    s.filtered.push(s.candidates.data[i]);
            }
            s.applying = s.filtered.data;
            s.applyingCount = s.filtered.count;
        }
        applyOrigin(s, set, blocks, CssOrigin_UserAgent, false);
        if (shared != nullptr)
            apply(s, shared, false, hinted);
        if (hints != nullptr)
            apply(s, hints, false, hinted);
        if (!input.userAgentOnly) {
            applyOrigin(s, set, blocks, CssOrigin_Author, false);
            if (inlineStyle != nullptr)
                apply(s, inlineStyle, false, attached);
            applyOrigin(s, set, blocks, CssOrigin_Author, true);
            if (inlineStyle != nullptr)
                apply(s, inlineStyle, true, attached);
        }
        applyOrigin(s, set, blocks, CssOrigin_UserAgent, true);
        if (input.pseudo == CssPseudo_FirstLine || input.pseudo == CssPseudo_FirstLetter) {
            for (uint32_t i = 0; i < s.declared.count; ++i) {
                PrCssProperty property = s.declared.data[i];
                if (!appliesToFirst(input.pseudo, property))
                    s.winners.data[property] = CssCascaded{};
            }
        }
    };
    s.appearance = parentAppearance;
    cascade();

    CssComputedStyle* style = allocZeroed<CssComputedStyle>();
    style->references.storeRelaxed(1);
    style->custom = customProperties(s, builder->table, input.parent != nullptr ? input.parent->custom : nullptr);

    // An element's used appearance comes from the declarations that apply under every appearance, which decide
    // appearance and devolution alone. The entries for its appearance then apply in their place in the cascade, which
    // takes a second pass, which an element that matched none of them skips. They declare no custom property, so the
    // custom properties of the first pass stand.
    style->appearance = parentAppearance;
    if (widget != DomWidget_None) {
        PrCssKeyword keyword = cascadedAppearance(s, input, style->custom);
        style->appearance =
            usedAppearance(keyword, input.element, widget, widget == DomWidget_Devolvable && devolves(s));
        if ((conditions & (1u << style->appearance)) != 0) {
            s.appearance = style->appearance;
            cascade();
        }
    }

    // Until the font group is computed, the font that its values measure is the parent's. The root's own values measure
    // no font until then, as the initial font has no style.
    style->locale = locale;
    CssFontMetrics metrics{};
    metrics.measure = builder->measure;
    CssStyleContext context{};
    context.metrics = &metrics;
    context.font = input.parent != nullptr ? cssStyleFontBases(input.parent) : kCssInitialFontBases;
    // Root-relative lengths in the root's own font size and line height are relative to the initial font.
    context.root = input.parent != nullptr ? input.rootFont : kCssInitialFontBases;
    context.parentWeight = input.parent != nullptr ? cssStyleFontWeight(input.parent) : 400;
    if (input.parent != nullptr)
        context.parentColor = cssStyleColor(input.parent, kCssDefaultColorEnvironment);
    else
        context.parentColor = PrColor{0, 0, 0, 1};
    context.viewport = input.viewport;
    // Tracks whether this style reads the viewport. The scratch also records this for every style since it last forgot
    // the tree.
    bool readBefore = s.readsViewport;
    s.readsViewport = false;
    context.readsViewport = &s.readsViewport;
    // The font group comes first, because the lengths of the other groups are relative to its size and line height, and
    // so are the root's own root-relative lengths after them.
    style->groups[CssStyleGroup_Font] = computeGroup(builder, s, CssStyleGroup_Font, input, context, style->custom);
    context.font.style = style;
    if (input.parent == nullptr)
        context.root.style = style;
    // The typography group follows the font group, whose longhands the font shorthand declares with it, so that a
    // shorthand that substitutes a variable is parsed once for both.
    style->groups[CssStyleGroup_Typography] =
        computeGroup(builder, s, CssStyleGroup_Typography, input, context, style->custom);
    bool readsFont = false;
    mapFlowRelative(s, style, &readsFont);
    style->important = importantOf(s);
    context.readsFont = &readsFont;
    style->groups[CssStyleGroup_Color] = computeGroup(builder, s, CssStyleGroup_Color, input, context, style->custom);
    style->groups[CssStyleGroup_Decoration] =
        computeGroup(builder, s, CssStyleGroup_Decoration, input, context, style->custom);
    style->groups[CssStyleGroup_Background] =
        computeGroup(builder, s, CssStyleGroup_Background, input, context, style->custom);
    style->groups[CssStyleGroup_Effect] = computeGroup(builder, s, CssStyleGroup_Effect, input, context, style->custom);
    style->groups[CssStyleGroup_Compositing] =
        computeGroup(builder, s, CssStyleGroup_Compositing, input, context, style->custom);
    style->groups[CssStyleGroup_Animation] =
        computeGroup(builder, s, CssStyleGroup_Animation, input, context, style->custom);
    style->groups[CssStyleGroup_Box] = computeGroup(builder, s, CssStyleGroup_Box, input, context, style->custom);
    style->groups[CssStyleGroup_Flex] = computeGroup(builder, s, CssStyleGroup_Flex, input, context, style->custom);
    style->groups[CssStyleGroup_Generated] =
        computeGroup(builder, s, CssStyleGroup_Generated, input, context, style->custom);
    style->groups[CssStyleGroup_List] = computeGroup(builder, s, CssStyleGroup_List, input, context, style->custom);
    style->groups[CssStyleGroup_Svg] = computeGroup(builder, s, CssStyleGroup_Svg, input, context, style->custom);
    style->groups[CssStyleGroup_SvgGeometry] =
        computeGroup(builder, s, CssStyleGroup_SvgGeometry, input, context, style->custom);
    style->groups[CssStyleGroup_Table] = computeGroup(builder, s, CssStyleGroup_Table, input, context, style->custom);
    style->groups[CssStyleGroup_Grid] = computeGroup(builder, s, CssStyleGroup_Grid, input, context, style->custom);
    style->inheritsReset = s.inheritsReset;
    style->staticInline = s.staticInline;
    CssValueRun display = cssComputedValue(style, PrCssProperty_Display);
    bool single = display.count == 1 && display.values[0].kind == PrCssValueKind_Keyword;
    PrCssKeyword keyword = single ? cssValueKeyword(display.values) : PrCssKeyword_Count;
    style->childrenAreItems = keyword == PrCssKeyword_Flex || keyword == PrCssKeyword_InlineFlex ||
        keyword == PrCssKeyword_Grid || keyword == PrCssKeyword_InlineGrid ||
        (keyword == PrCssKeyword_Contents && input.parent != nullptr && input.parent->childrenAreItems);
    style->font = cssFontBases(context.font.em, context.font.lh);
    style->readsFontMetrics = metrics.readsFont;
    style->readsRootMetrics = metrics.readsRoot;
    bool readsViewport = s.readsViewport;
    s.readsViewport = readBefore || readsViewport;
    // A style that measured a font depends on what the fonts held when it was computed, so no other element takes it,
    // and the element computes it again.
    bool measured = metrics.readsFont || metrics.readsRoot;
    if (slot != nullptr && !measured)
        store(*slot, hash, sharedId, widget, parentAppearance, s, input, style, readsViewport, readsFont);
    if (memo != nullptr && !measured)
        remember(memo, set, s, input, style, sharedId, widget, readsViewport);
    else if (memo != nullptr)
        cssCascadeMemoClear(memo);
    return style;
}

uint32_t cssCascadeOriginating(const CssRuleSet* set, CssCascadeScratch* scratch, const PrDomElement* element) {
    CssCascadeScratch& s = *scratch;
    const CssRuleIndex* indexes[] = {&set->filed[CssPseudo_FirstLine], &set->filed[CssPseudo_FirstLetter]};
    uint32_t originating = 0;
    const PrDomNode* tree = nullptr;
    bool followed = false;
    bool filtered = false;
    for (const CssRuleIndex* filed : indexes) {
        if (filed->universal.count == 0 && filed->buckets.count == 0)
            continue;
        if (!followed) {
            follow(s, set);
            tree = treeOf(set, element);
            followed = true;
        }
        matchEach(set, s, element, *filed, [&](uint32_t entry) {
            bool matches;
            if (tree != nullptr)
                matches = matchesEntry<true>(set, s, element, tree, entry, &filtered);
            else
                matches = matchesEntry<false>(set, s, element, nullptr, entry, &filtered);
            if (matches)
                originating |= 1u << set->entries.data[entry].pseudo;
            return !matches;
        });
    }
    return originating;
}

uint32_t cssCascadeGenerating(const CssRuleSet* set, CssCascadeScratch* scratch, const PrDomElement* element) {
    CssCascadeScratch& s = *scratch;
    const CssRuleIndex& filed = set->generating;
    if (filed.universal.count == 0 && filed.buckets.count == 0)
        return 0;
    follow(s, set);
    constexpr uint32_t kBoth = (1u << CssPseudo_Before) | (1u << CssPseudo_After);
    uint32_t generating = 0;
    bool filtered = false;
    const PrDomNode* tree = treeOf(set, element);
    matchEach(set, s, element, filed, [&](uint32_t entry) {
        uint32_t bit = 1u << set->entries.data[entry].pseudo;
        if ((generating & bit) != 0)
            return true;
        bool matches;
        if (tree != nullptr)
            matches = matchesEntry<true>(set, s, element, tree, entry, &filtered);
        else
            matches = matchesEntry<false>(set, s, element, nullptr, entry, &filtered);
        if (!matches)
            return true;
        generating |= bit;
        return generating != kBoth;
    });
    return generating;
}

void cssCascadeScratchForget(CssCascadeScratch* scratch) {
    ++scratch->epoch;
    scratch->structure.clear();
    scratch->readsViewport = false;
}

void cssCascadeScratchReset(CssCascadeScratch* scratch) {
    forgetMatchedStyles(*scratch);
    for (uint32_t i = 0; i < scratch->matchedStyles.count; ++i)
        scratch->matchedStyles.data[i].matched.reset();
    scratch->matchedStyles.reset();
    for (uint32_t i = 0; i < scratch->cachedGroups.count; ++i)
        scratch->cachedGroups.data[i].key.reset();
    scratch->cachedGroups.reset();
    scratch->groupKey.reset();
    cssValuePoolReset(&scratch->urls);
    for (uint32_t i = 0; i < scratch->matchers.count; ++i)
        cssSelectorMatcherReset(&scratch->matchers.data[i]);
    scratch->matchers.reset();
    scratch->epochs.reset();
    scratch->set = nullptr;
    scratch->candidates.reset();
    scratch->filtered.reset();
    scratch->applying = nullptr;
    scratch->featureHashes.reset();
    scratch->featuresOf = nullptr;
    scratch->structure.reset();
    scratch->winners.reset();
    scratch->declared.reset();
    scratch->custom.reset();
    scratch->customIndex.reset();
    cssValuePoolReset(&scratch->substituted);
    prTextBufferReset(&scratch->text);
    cssDeclarationBlockReset(&scratch->shorthandBlock);
    scratch->shorthand = nullptr;
    scratch->parsedShorthand = PrCssProperty_Count;
    prTextBufferReset(&scratch->parsedText);
}

void cssStylePrepare(PrDomElement* element) {
    if (domAttributeValue(element, "style").data != nullptr &&
            (element->style == nullptr || element->style->inlineStyle == nullptr))
        cssElementInlineStyle(element);
    cssHints(element);
    cssSharedHintsPrepare(element);
}

} // namespace pr_impl
