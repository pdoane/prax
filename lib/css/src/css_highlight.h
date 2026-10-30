// Highlights
//
// css-highlight-api-1 - https://drafts.csswg.org/css-highlight-api-1/

#pragma once

#include "pr/array.h"
#include "pr/css.h"
#include "pr/dom.h"
#include "pr/string_view.h"
#include "pr/weak.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct DomRange;

// A Highlight (css-highlight-api-1/#highlight): ranges, in the order they were added. The highlight holds a reference
// to each range.
struct CssHighlight {
    Array<PrDomRange* PR_NONNULL> ranges;
    uint32_t references;
    uint32_t version;              // counts changes to the ranges
    int32_t priority;
    PrCssHighlightType type;
    WeakReferences weak;           // the registry entries that name the highlight
};

// An entry in a registry: a name that refers to its highlight without holding a reference to it. The entry becomes
// empty once the highlight is destroyed. Each entry is a separate allocation, because its weak reference is stored at
// its address.
struct CssHighlightEntry {
    PrString name;
    WeakReference highlight;
};

// A HighlightRegistry (css-highlight-api-1/#highlight-registry), which a document owns. Its entries are in the order in
// which their names were first set. An empty entry is removed the next time the registry is read.
struct CssHighlightRegistry {
    Array<CssHighlightEntry* PR_NONNULL> entries;
    uint32_t version;              // counts changes to the entries
};

// Creates an empty highlight of type Highlight and priority 0. The caller holds its only reference.
PrCssHighlight* cssHighlightCreate();

// Adds or releases a reference. The last release frees the highlight, and empties the entries that name it.
void cssHighlightRetain(PrCssHighlight* highlight);
void cssHighlightRelease(PrCssHighlight* PR_NULLABLE highlight);

// Adds `range` and retains it, unless the highlight already holds it.
void cssHighlightAdd(PrCssHighlight* highlight, PrDomRange* range);

// Removes `range` and releases it. Returns false for a range the highlight does not hold.
bool cssHighlightDelete(PrCssHighlight* highlight, PrDomRange* range);

// Removes and releases every range.
void cssHighlightClear(PrCssHighlight* highlight);

// Frees the registry and its entries.
void cssHighlightRegistryDestroy(PrCssHighlightRegistry* PR_NULLABLE registry);

// Removes the entries whose highlights were destroyed, and returns the registry.
PrCssHighlightRegistry* cssHighlightRegistryLive(PrCssHighlightRegistry* registry);

// Returns the index of the entry for `name`, or kArrayMissing if there is none.
uint32_t cssHighlightRegistryFind(const PrCssHighlightRegistry* registry, PrStringView name);

// Sets `name` to refer to `highlight`, replacing the highlight that the name referred to before.
void cssHighlightRegistrySet(PrCssHighlightRegistry* registry, PrStringView name, PrCssHighlight* highlight);

// Removes the entry for `name`. Returns false if there is none.
bool cssHighlightRegistryDelete(PrCssHighlightRegistry* registry, PrStringView name);

// Removes every entry.
void cssHighlightRegistryClear(PrCssHighlightRegistry* registry);

} // namespace pr_impl

// C opaque types
struct PrCssHighlight : pr_impl::CssHighlight {};
struct PrCssHighlightRegistry : pr_impl::CssHighlightRegistry {};

PR_ASSUME_NONNULL_END
