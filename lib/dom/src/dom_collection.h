// Collections and node lists
//
// dom - https://dom.spec.whatwg.org/#concept-collection

#pragma once

#include "pr/dom.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/status.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The filter that selects the elements a collection holds.
enum {
    DomCollectionFilter_QualifiedName,   // dom/#concept-getelementsbytagname
    DomCollectionFilter_ClassNames,      // dom/#concept-getelementsbyclassname
    DomCollectionFilter_Children,        // the element children (dom/#dom-parentnode-children)

    // The collections of a document's elements that HTML names (html/#dom-document-images and the members after it).
    DomCollectionFilter_Images,          // img elements
    DomCollectionFilter_Embeds,          // embed elements
    DomCollectionFilter_Links,           // a and area elements with an href attribute
    DomCollectionFilter_Forms,           // form elements
    DomCollectionFilter_Scripts,         // script elements
    DomCollectionFilter_Anchors,         // a elements with a name attribute

    // The listed elements whose form owner is the form at the root of the collection, other than image buttons, in the
    // form's tree (html/#dom-form-elements).
    DomCollectionFilter_FormControls,

    // The listed elements under the fieldset at the root of the collection (html/#dom-fieldset-elements).
    DomCollectionFilter_FieldSetElements,

    // The area elements under the map at the root of the collection (html/#dom-map-areas).
    DomCollectionFilter_MapAreas,

    // The options under the root of the collection: the list of options of a select (html/#concept-select-option-list),
    // those of its options that are selected, and the option elements under a datalist (html/#dom-datalist-options).
    DomCollectionFilter_SelectOptions,
    DomCollectionFilter_SelectedOptions,
    DomCollectionFilter_DataListOptions,

    // The parts of the table at the root of the collection (html/#dom-table-tbodies, html/#dom-table-rows). Its rows
    // are not in tree order: those of its thead children come first, and those of its tfoot children last.
    DomCollectionFilter_TableBodies,
    DomCollectionFilter_TableRows,

    // The tr children of a table section, and the td and th children of a row (html/#dom-tbody-rows,
    // html/#dom-tr-cells).
    DomCollectionFilter_ChildRows,
    DomCollectionFilter_RowCells,
};
typedef uint8_t DomCollectionFilter;

// The length of a collection that does not know its length.
constexpr uint32_t kDomCollectionLengthNone = ~0u;

// A cache of what a live collection or list learned about its nodes: its length, if known, and the last node that it
// reached by index. The cache is valid while the document's tree version is the version at which the cache was filled.
// Iterating by index walks the tree once.
struct DomCollectionCache {
    const PrDomDocument* PR_NULLABLE document;
    uint64_t version;
    uint32_t length;                // kDomCollectionLengthNone if not known
    uint32_t index;                 // the index of `node`
    PrDomNode* PR_NULLABLE node;    // null if no node is known
};

// A collection of the descendants of `root` that the filter accepts, in tree order, or of the children of `root` for
// DomCollectionFilter_Children. A collection of a form's controls holds the form as its root, and reads the form's
// tree. The collection holds a reference to its root.
struct DomHtmlCollection {
    PrDomNode* root;
    PrRawString text;                  // the qualified name, or the class names
    DomCollectionFilter filter;
    mutable DomCollectionCache cache;
};

// Creates a collection of the elements under `root` that `filter` accepts, with `text` as the filter's argument.
PrDomHtmlCollection* domHtmlCollectionCreate(PrDomNode* root, DomCollectionFilter filter, PrStringView text);

// Return the number of elements in the collection, and the element at `index` or null past the end.
uint32_t domHtmlCollectionLength(const PrDomHtmlCollection* collection);
PrDomElement* PR_NULLABLE domHtmlCollectionItem(const PrDomHtmlCollection* collection, uint32_t index);

// Returns the first element whose ID is `name` or, for an HTML element, whose name attribute is `name`. Returns null
// for the empty string (dom/#dom-htmlcollection-nameditem-key).
PrDomElement* PR_NULLABLE domHtmlCollectionNamedItem(const PrDomHtmlCollection* collection, PrStringView name);

// Appends the collection's supported property names (dom/#ref-for-dfn-supported-property-names) to `out`: for each
// element in order, its ID, then its name attribute if it is an HTML element, each name once and none empty. The views
// are valid until the elements' attributes change.
void domHtmlCollectionNames(const PrDomHtmlCollection* collection, RawArray<PrStringView>* out);

// Frees the collection, and releases its reference to its root.
void domHtmlCollectionDestroy(PrDomHtmlCollection* PR_NULLABLE collection);

// The nodes that a live node list holds.
enum {
    DomNodeListFilter_Children,         // the children of the list's node (dom/#dom-node-childnodes)
    DomNodeListFilter_Labels,           // the labels whose labeled control is the list's node (html/#dom-lfe-labels)

    // The controls of the form that is the list's node with the name or ID `name`, other than image buttons
    // (html/#radionodelist), and the img elements of the form with that name or ID (html/#dom-form-nameditem).
    DomNodeListFilter_Radio,
    DomNodeListFilter_FormImages,
};
typedef uint8_t DomNodeListFilter;

// A node list (dom/#interface-nodelist). If `parent` is not null, the list is live, and holds the nodes that its filter
// finds from `parent`. Otherwise it is a snapshot, and `nodes` holds its nodes. The list holds a reference to its
// parent, or to each of its nodes.
struct DomNodeList {
    PrDomNode* PR_NULLABLE parent;
    RawArray<PrDomNode* PR_NONNULL> nodes;
    PrRawString name;                          // the name of a radio node list
    DomNodeListFilter filter;
    mutable DomCollectionCache cache;       // cache for a live list
};

// Creates the live list of the children of `parent`, or an empty snapshot list if `parent` is null.
PrDomNodeList* domNodeListCreate(PrDomNode* PR_NULLABLE parent);

// Creates the live list of the nodes that `filter` finds from `node`, with `name` as the filter's argument.
PrDomNodeList* domNodeListCreateFiltered(PrDomNode* node, DomNodeListFilter filter, PrStringView name);

// Makes the list a snapshot of `nodes`, in place of what it held before.
void domNodeListAssign(PrDomNodeList* list, PrDomNode* const PR_NONNULL* PR_NULLABLE nodes, uint32_t count);

// Return the number of nodes in the list, and the node at `index` or null past the end.
uint32_t domNodeListLength(const PrDomNodeList* list);
PrDomNode* PR_NULLABLE domNodeListItem(const PrDomNodeList* list, uint32_t index);

// Frees the list, and releases its references.
void domNodeListDestroy(PrDomNodeList* PR_NULLABLE list);

// A DOMTokenList (dom/#interface-domtokenlist): the token set of an element's attribute `name`. Each call reads the set
// from the attribute, and each change writes it back. The list holds a reference to the element.
//
// A call that changes the set returns Syntax for an empty token, and InvalidData for a token that contains ASCII
// whitespace. It then changes nothing.
struct DomTokenList {
    PrDomElement* element;
    PrRawString name;

    // The attribute's supported tokens (dom/#concept-supported-tokens) in ASCII lowercase, which outlive the list. Null
    // if the attribute defines none.
    const PrStringView* PR_NULLABLE supported;
    uint32_t supportedCount;
};

// Create the token list of the attribute `name` of `element`, and free a list.
PrDomTokenList* domTokenListCreate(PrDomElement* element, PrStringView name);
void domTokenListDestroy(PrDomTokenList* PR_NULLABLE list);

// Returns the number of tokens.
uint32_t domTokenListLength(const PrDomTokenList* list);

// Returns the token at `index`, or the null view past the last token.
PrStringView domTokenListItem(const PrDomTokenList* list, uint32_t index);

// Returns true if the set holds `token`.
bool domTokenListContains(const PrDomTokenList* list, PrStringView token);

// Add each of `tokens` that the set lacks to its end, or remove each of `tokens` from the set.
PrStatus domTokenListAdd(PrDomTokenList* list, const PrStringView* PR_NULLABLE tokens, uint32_t count);
PrStatus domTokenListRemove(PrDomTokenList* list, const PrStringView* PR_NULLABLE tokens, uint32_t count);

// Adds `token` if the set lacks it, and removes it otherwise (dom/#dom-domtokenlist-toggle). A non-null `force` adds
// the token if true and removes it if false. Writes true if the token is present afterward.
PrStatus domTokenListToggle(PrDomTokenList* list, PrStringView token, const bool* PR_NULLABLE force, bool* out);

// Replaces `token` with `newToken` (dom/#dom-domtokenlist-replace). Writes true if `token` was present.
PrStatus domTokenListReplace(PrDomTokenList* list, PrStringView token, PrStringView newToken, bool* out);

// Returns the attribute's value, or the empty string if the element has no such attribute.
PrStringView domTokenListValue(const PrDomTokenList* list);

// Sets the attribute to `value`.
void domTokenListSetValue(PrDomTokenList* list, PrStringView value);

// Writes the tokens of `text`, split on ASCII whitespace, without repeats after the first occurrence
// (dom/#concept-ordered-set-parser). The views point into `text`.
void domOrderedSetParse(PrStringView text, RawArray<PrStringView>* out);

} // namespace pr_impl

// C opaque types
struct PrDomHtmlCollection : pr_impl::DomHtmlCollection {};
struct PrDomNodeList : pr_impl::DomNodeList {};
struct PrDomTokenList : pr_impl::DomTokenList {};

PR_ASSUME_NONNULL_END
