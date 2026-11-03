// Resource documents: the other documents that a document's references name (SVG2/linking.html#processingURL), such as
// a use element's href or a fill's url() into another file.
//
// A document fetches each such document once, by its URL without a fragment, and keeps it. A fetched document is parsed
// in secure static mode (SVG2/linking.html#processingURL-parsing). It has no browsing context, so it fetches nothing
// and runs no script. Nothing changes it once it is parsed. A document with a browsing context fetches through its
// host, and only from its own origin, since no reference that reaches here is made with CORS. A document without a
// browsing context takes the documents that a loader gives, such as when a harness draws a document from files.

#pragma once

#include "pr/string_ops.h"
#include "pr/string_view.h"

#include "dom/src/dom_node.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns the fragment of the URL `url`, without its `#`, or the null view if it has none.
inline PrStringView domUrlFragment(PrStringView url) {
    size_t hash = prStringFindByte(url, '#');
    return hash < url.size ? prStringSlice(url, hash + 1, url.size - hash - 1) : PrStringView{};
}

struct DomResources;

// Loads the document that `url` names, for a document without a browsing context. `url` is a reference in `from`
// without its fragment. It is absolute, or as the reference writes it if it does not resolve against the base URL of
// `from`. Returns null if nothing loads. The returned document must stay alive until the call returns, and the
// resources take a reference to it.
typedef PrDomDocument* PR_NULLABLE (*DomResourceLoadFn)(void* PR_NULLABLE context, PrDomDocument* from, PrStringView url);

// A reference split into the document that it names and the fragment that names an element there.
struct DomReference {
    bool external;              // the URL names a document other than the one that holds the reference
    PrString url;               // External: the other document's URL without its fragment, absolute if it resolves
    PrStringView fragment;      // the fragment without its `#`, a view into the reference, or the null view if none
};

// Splits `reference`, a URL as written in `document`, into the document that it names and its fragment
// (SVG2/linking.html#processingURL-absolute). A reference of a fragment alone, or whose URL equals the document's base
// URL but for the fragment, names the document itself. A reference that does not parse against the base URL, such as a
// relative reference against about:blank, names another document by the URL as written. Returns false if the reference
// is empty. The caller releases `out->url` with prStringReset.
bool domReferenceSplit(const PrDomDocument* document, PrStringView reference, DomReference* out);

// Returns the resource document that `url`, an absolute URL without a fragment, names for `document`. Returns null
// until the document has loaded, and if it fails to load. The first call for a URL starts its load: a fetch through the
// document's host, or the loader that domResourcesSetLoader gave. Once a fetch completes, the document's uses resolve
// their references again, and its presentation hears that what IDs name may have changed.
PrDomDocument* PR_NULLABLE domResourceDocument(PrDomDocument* document, PrStringView url);

// Returns the element that a reference split by domReferenceSplit names in `document`: the element of the fragment's ID
// in the document that the reference names, or that document's root element without a fragment
// (SVG2/linking.html#processingURL-target). Returns null if no such element exists or the resource document has not
// loaded.
const PrDomElement* PR_NULLABLE domReferenceTarget(PrDomDocument* document, const DomReference& reference);

// Sets the loader that a document without a browsing context takes resource documents from, until another call replaces
// it. A null `load` loads nothing. The resource documents that it loaded stay.
void domResourcesSetLoader(PrDomDocument* document, DomResourceLoadFn PR_NULLABLE load, void* PR_NULLABLE context);

// Frees the resource documents that a document keeps.
void domResourcesDestroy(DomResources* PR_NULLABLE resources);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
