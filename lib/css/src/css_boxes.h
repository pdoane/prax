// What elements give their boxes beyond their computed style
//
// css-display-3 - https://drafts.csswg.org/css-display-3/
//
// The markup languages decide some of what an element's box is: whether it is replaced, a line break or a form control,
// how many table columns it spans, and how its display: contents behaves. Each language that has such elements gives
// the document's CSS a provider for its namespace (cssBoxesProvide), which answers from the element alone. The
// presentation asks it as it projects each element, so the provider reads only the element, its attributes and its
// state, and is cheap.

#pragma once

#include "pr/bitset.h"
#include "pr/dom.h"
#include "pr/dom_tag.h"
#include "pr/layout_style.h"
#include "pr/string_view.h"

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
    PrString (*PR_NULLABLE replacedText)(void* PR_NULLABLE element);

    // The tags of the elements that two line breaks set apart in their rendered text, whatever their boxes
    // (html/#rendered-text-collection-steps), a set that gen_dom.py builds. Null for a namespace with none.
    const uint64_t* PR_NULLABLE paragraphs;

    // Returns the color schemes that the document's page supports, as the namespace's elements give them, such as
    // HTML's meta elements (css-color-adjust-1/#color-scheme-page). Null for a namespace whose elements give none.
    CssColorSchemeFlags (*PR_NULLABLE pageSchemes)(PrDomDocument* document);
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
