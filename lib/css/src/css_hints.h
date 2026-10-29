// Presentational hints: the hints that elements' attributes map to, as the cascade applies them
//
// css-cascade-5 - https://drafts.csswg.org/css-cascade-5/
//
// The cascade applies hints as author declarations of zero specificity, before every other author declaration
// (css-cascade-5/#preshint). The language of an element's namespace maps its attributes to hints, through the provider
// that it gives the document's CSS: HTML (html/#rendering), and SVG's presentation attributes. Most hints come from the
// element's own attributes. Some come from another element's attributes, as a table's attributes style its parts. The
// element that gives such hints builds one block for every kind of element that takes them, which keeps the shared
// hints cheap to match and lets those elements share computed styles.
//
// Each block is built on first use, on the element's agent thread, and kept until an attribute that it reads changes.

#pragma once

#include "pr/dom.h"
#include "pr/string_view.h"
#include "pr/text_buffer.h"

#include "css/src/css_rule.h"
#include "dom/src/dom_node.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct CssElementStyle;

// The elements that a change to an attribute restyles through its hints.
enum {
    CssHintReach_None,              // the attribute maps to no hint
    CssHintReach_Self,              // the element alone
    CssHintReach_Subtree,           // the element and its descendants, as for a table's border
    CssHintReach_Document,          // every element of the document, as for a body's link colors
};
typedef uint8_t CssHintReach;

// The most kinds of element that take hints from one element.
constexpr uint32_t kCssHintTakers = 4;

// What maps the attributes of a namespace's elements to hints.
struct CssHintProvider {
    // Returns true if the element may have hints of its own: an attribute of it maps to one, or it has one that no
    // attribute gives. Most elements have none, which this tells without building them.
    bool (*hasOwn)(const PrDomElement* element);

    // Appends the declarations of the hints that the element's own attributes map to.
    void (*own)(TextBuffer* out, const PrDomElement* element);

    // Returns the elements that a change to the attribute `name` of the element restyles through hints.
    CssHintReach (*mapped)(const PrDomElement* element, PrStringView name);

    // Returns the element that gives `element` hints, or null if none does, and writes the kind of element that it is,
    // below kCssHintTakers, to `taker`. Null for a namespace whose elements give none.
    const PrDomElement* PR_NULLABLE (*PR_NULLABLE giver)(const PrDomElement* element, uint32_t* taker);

    // Appends the declarations of the hints that `giver` gives the elements of the kind `taker`.
    void (*PR_NULLABLE given)(TextBuffer* out, const PrDomElement* giver, uint32_t taker);
};

// Gives the document's CSS the provider of the hints of the elements of the namespace `space`.
void cssHintsProvide(PrDomDocument* document, DomNamespaceId space, const CssHintProvider* provider);

// Append a declaration of `property` with `value`, or with a length of `pixels`, for a provider.
void cssHintAdd(TextBuffer* out, PrStringView property, PrStringView value);
void cssHintAddPixels(TextBuffer* out, PrStringView property, double pixels);

// Returns the elements that a change to the attribute named `name` of `element` restyles through hints.
CssHintReach cssHintsMapped(const PrDomElement* element, PrStringView name);

// Returns the hints that the element's own attributes give it, or null if it has none. Rebuilds the hints if an
// attribute that they read has changed since they were built. Called on the element's agent thread. Restyle threads
// then read the block until the element changes.
const CssDeclarationBlock* PR_NULLABLE cssHints(PrDomElement* element);

// Builds the hints that another element gives `element`, if they are not built. Called on the element's agent thread,
// before a restyle reads them with cssSharedHints.
void cssSharedHintsPrepare(PrDomElement* element);

// Returns the hints that another element gives `element`, or null if there are none. Writes to `id` a number that
// identifies the block. A block that is rebuilt gets a new number, so a cache may key on it. Safe on restyle threads
// once cssSharedHintsPrepare has run for the element.
const CssDeclarationBlock* PR_NULLABLE cssSharedHints(const PrDomElement* element, uint64_t* id);

// Discards the element's hints, and those that it gives other elements, after an attribute that they read has changed.
void cssHintsChanged(PrDomElement* element);

// Frees the hints that an element's style state holds.
void cssHintsDestroy(CssElementStyle* style);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
