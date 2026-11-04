// What elements give their boxes beyond their computed style
//
// css-display-3 - https://drafts.csswg.org/css-display-3/
//
// The markup languages decide some of what an element's box is: whether it is replaced, a line break or a form control,
// how many table columns it spans, and how its display: contents behaves. Each language that has such elements gives
// the document's CSS a provider for its namespace (cssBoxesProvide), which answers from the element alone. The
// presentation asks it as it projects each element, so the provider reads only the element, its attributes and its
// state, and is cheap.
//
// A language may also give layout what an element's markup and surroundings decide beyond its style, such as the layout
// algorithm of its box and the values of its attributes. The provider then refines the layout record that the element's
// style projects to. It also names the elements whose records a change to the document reaches, so that the
// presentation projects them again.

#pragma once

#include "pr/bitset.h"
#include "pr/css_presentation.h"
#include "pr/dom.h"
#include "pr/dom_tag.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/string_view.h"
#include "pr/text_transform.h"

#include "css/src/css_root.h"
#include "css/src/css_style.h"
#include "dom/src/dom_node.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The kinds of box that an element generates beyond what its display says.
enum {
    CssBoxKind_Normal,

    // The box's content is the element's resource, not its children's boxes (html/#replaced-elements).
    CssBoxKind_Replaced,

    CssBoxKind_LineBreak,       // a line break, as a br is (html/#phrasing-content-3)
    CssBoxKind_Button,          // laid out as a button is, semi-replaced (html/#button-layout)

    // A one-line text control, semi-replaced (html/#the-input-element-as-a-text-entry-widget).
    CssBoxKind_TextField,
};
typedef uint8_t CssBoxKind;

// What an element's box does beyond its kind.
enum {
    // display: contents makes the element generate no box at all, as display: none does (css-display-3/#unbox-html).
    CssBoxTraitFlag_Unboxes = 1u << 0,

    // The element's children are fallback content, which is not shown and takes no input while the element is
    // (html/#fallback-content).
    CssBoxTraitFlag_HoldsFallback = 1u << 1,

    // The element's children are a control's value, which a text box shows and the rendered text steps do not read
    // (html/#rendered-text-collection-steps).
    CssBoxTraitFlag_ChildrenAreValue = 1u << 2,

    // Of the element's children only `shownChild`, or none if it is null, generates boxes.
    CssBoxTraitFlag_ShowsOneChild = 1u << 3,

    // The element's direction is auto unless a dir attribute says otherwise (html/#the-directionality).
    CssBoxTraitFlag_AutoDirection = 1u << 4,

    // `columnSpan` and `rowSpan` apply.
    CssBoxTraitFlag_Spans = 1u << 5,

    // `natural` applies.
    CssBoxTraitFlag_Sized = 1u << 6,
};
typedef uint8_t CssBoxTraitFlags;

// What an element gives its box beyond its computed style.
struct CssBoxTraits {
    CssBoxKind kind;
    CssBoxTraitFlags flags;

    // The columns and rows that the element spans as a table cell, column or column group.
    uint16_t columnSpan;
    uint16_t rowSpan;

    // The inline size of a text field in characters, or 0.
    uint16_t characters;

    const PrDomNode* PR_NULLABLE shownChild;

    // The natural dimensions of a replaced element that the element decides, as a canvas's attributes and a video's
    // resource do. Otherwise a replaced element's are its image's.
    PrLayoutNaturalSize natural;
};

// The context in which a provider refines the layout record of an element's box. The record may read the styles of
// other elements from `presentation` (cssPresentationNodeStyle), is interned in `tree`, and resolves lengths against
// `measure` and `viewport`.
struct CssBoxRecordContext {
    const PrCssPresentation* presentation;
    PrLayoutTree* tree;
    const CssFontMeasure* PR_NULLABLE measure;
    CssViewport viewport;
};

// What changed at an element that may change the layout records of elements beyond those that the change restyles.
enum {
    CssBoxChange_Children,      // a child was inserted or removed, its text changed, or its style changed in a way that
                                // CssBoxProvider::childStyleReaches reports
    CssBoxChange_Attributes,    // an attribute in no namespace changed
    CssBoxChange_Style,         // its style changed in a way that CssBoxProvider::styleReaches reports
};
typedef uint8_t CssBoxChange;

// Called with each element whose layout record a change reaches.
typedef void (*CssBoxReachFn)(void* PR_NULLABLE context, PrDomElement* element);

// What a namespace's elements give their boxes.
struct CssBoxProvider {
    // The tags of the elements that may give their boxes anything, a set that gen_dom.py builds (dom_tag.h). The
    // provider is asked about those alone, so that the elements of every other tag, which are most, cost a projection
    // no call.
    const uint64_t* PR_NONNULL tags;

    // Writes what the element gives its box to `out`, which is zeroed.
    void (*PR_NONNULL traits)(const PrDomElement* element, CssBoxTraits* out);

    // Returns true if a change to the element's attribute `name`, in no namespace, may change its traits.
    bool (*PR_NONNULL reads)(const PrDomElement* element, PrStringView name);

    // Returns the text of a replaced element, given as its context, which its box does not hold
    // (PrLayoutContent::text). Null for a namespace whose replaced elements have none.
    PrRawString (*PR_NULLABLE replacedText)(void* PR_NULLABLE element);

    // The tags of the elements that two line breaks set apart in their rendered text, whatever their boxes
    // (html/#rendered-text-collection-steps), a set that gen_dom.py builds. Null for a namespace with none.
    const uint64_t* PR_NULLABLE paragraphs;

    // Returns the color schemes that the document's page supports, as the namespace's elements give them, such as
    // HTML's meta elements (css-color-adjust-1/#color-scheme-page). Null for a namespace whose elements give none.
    CssColorSchemeFlags (*PR_NULLABLE pageSchemes)(PrDomDocument* document);

    // Returns `record`, the layout record that the element's computed `style` projects to, refined with what the
    // element's markup and surroundings give layout beyond its style. The presentation calls it for every element of
    // the namespace that has a box, whatever its tag. Null for a namespace whose records come from their styles alone.
    PrLayoutStyleIndex (*PR_NULLABLE record)(const CssBoxRecordContext& context, const PrDomElement* element,
        const CssComputedStyle* style, PrLayoutStyleIndex record);

    // Calls `fn` with each element whose record a change of kind `change` at `element` may change, among the elements
    // whose records `record` refines. Null if `record` is.
    void (*PR_NULLABLE reach)(PrDomElement* element, CssBoxChange change, CssBoxReachFn fn, void* PR_NULLABLE context);

    // Returns true if a child's change of computed style from `old` to `style` can change the records that
    // CssBoxChange_Children reaches. Null if `record` is.
    bool (*PR_NULLABLE childStyleReaches)(const CssComputedStyle* old, const CssComputedStyle* style);

    // Returns true if the element's change of computed style from `old` to `style` can change the records of other
    // elements, which CssBoxChange_Style reaches, as an operator's font gives the spaces of the operators around it.
    // Null if `record` is.
    bool (*PR_NULLABLE styleReaches)(const PrDomElement* element, const CssComputedStyle* old, const CssComputedStyle* style);
};

// Gives the document's CSS the provider of the elements of the namespace `space`.
void cssBoxesProvide(PrDomDocument* document, DomNamespaceId space, const CssBoxProvider* provider);

// Returns the color schemes that the document's page supports, as the first provider that gives them says, or none.
CssColorSchemeFlags cssPageSchemes(PrDomDocument* document);

// Returns the provider of the element's namespace, or null if it has none.
inline const CssBoxProvider* PR_NULLABLE cssBoxProviderOfNamespace(const PrDomElement* element) {
    const CssRoot* root = element->document->style;
    return root != nullptr && element->space < kCssProviderNamespaces ? root->boxProviders[element->space] : nullptr;
}

// Returns the provider of the element's namespace if the element's tag is one that it answers for, or else null.
inline const CssBoxProvider* PR_NULLABLE cssBoxProviderOf(const PrDomElement* element) {
    const CssBoxProvider* provider = cssBoxProviderOfNamespace(element);
    return provider != nullptr && element->tag < PrDomTag_Count && bitsetTest(provider->tags, element->tag) ? provider : nullptr;
}

// Returns what the element gives its box, as its namespace's provider says. An element of a namespace without one gives
// nothing.
inline CssBoxTraits cssBoxTraits(const PrDomElement* element) {
    CssBoxTraits traits{};
    if (const CssBoxProvider* provider = cssBoxProviderOf(element))
        provider->traits(element, &traits);
    return traits;
}

// Returns true if the element is a paragraph, as its namespace's provider says.
inline bool cssBoxIsParagraph(const PrDomElement* element) {
    const CssBoxProvider* provider = cssBoxProviderOfNamespace(element);
    return provider != nullptr && provider->paragraphs != nullptr && element->tag < PrDomTag_Count &&
        bitsetTest(provider->paragraphs, element->tag);
}

// Returns true if a change to the element's attribute `name`, in no namespace, may change what it gives its box.
inline bool cssBoxReads(const PrDomElement* element, PrStringView name) {
    const CssBoxProvider* provider = cssBoxProviderOf(element);
    return provider != nullptr && provider->reads(element, name);
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
