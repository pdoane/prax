// The node tree
//
// dom - https://dom.spec.whatwg.org/

#pragma once

#include "pr/dom.h"
#include "pr/string_view.h"

#include "dom/src/dom_node_leaves.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns the URL of `space` in `document`, or the null view for PrDomNamespace_None.
PrStringView domNamespaceUrl(const PrDomDocument* document, DomNamespaceId space);

// Returns the URL of a namespace that the specs name. The text lives as long as the process. Returns the null view for
// PrDomNamespace_None.
PrStringView domNamedNamespaceUrl(DomNamespaceId space);

// Writes the namespace whose URL is `url` in `document`. If the specs name no such namespace, it is added to the
// document's namespaces. The null view and the empty string are PrDomNamespace_None. Returns false if the document
// already holds as many namespaces as a DomNamespaceId can name.
bool domNamespaceFromUrl(PrDomDocument* document, PrStringView url, DomNamespaceId* out);

static_assert(sizeof(DomAttr) == 168);
static_assert(sizeof(DomElement) == 136);
static_assert(sizeof(DomText) == 128);
static_assert(sizeof(DomComment) == 128);
static_assert(sizeof(DomProcessingInstruction) == 160);
static_assert(sizeof(DomDocumentType) == 192);
static_assert(sizeof(DomDocument) == 584);

// Returns the fragment that a template element holds (html/#the-template-element), or null for any other element.
inline PrDomNode* PR_NULLABLE domElementContents(const PrDomElement* element) {
    return element->state != nullptr ? element->state->contents : nullptr;
}

// Returns the number of the element's attributes.
inline uint32_t domElementAttributeCount(const PrDomElement* element) {
    uint32_t count = 0;
    for (const PrDomNode* attribute = element->firstAttribute; attribute != nullptr;
         attribute = attribute->next)
        ++count;
    return count;
}

// ---------------------------------------------------------------------------------------------------------------------
// Making nodes
//
// A node created here is a root with no references, and belongs to its creator. The creator attaches it, or takes a
// reference to it before passing it to anything that could release or remove it. The mutation algorithms take such a
// reference.

// Creates a document of `kind` with the initial content type of that kind, and one reference that the caller holds.
PrDomDocument* domDocumentCreate(DomDocumentKind kind);

// Adds or releases a counted reference. A reference to a node keeps its tree alive. A tree other than the document's
// own tree keeps the document alive. The last release frees the tree.
void domNodeRetain(PrDomNode* node);
void domNodeRelease(PrDomNode* PR_NULLABLE node);

// Adds or releases `count` references at once. The objects of an element's style sheet hold references that count as
// references to the element (css_object.h).
void domNodeRetainBy(PrDomNode* node, uint32_t count);
void domNodeReleaseBy(PrDomNode* node, uint32_t count);

// Retains or releases each of `nodes`. For each run of nodes in tree order that one node owns, the count changes once
// at that owner, however many nodes the run holds.
void domNodeRetainEach(PrDomNode* const PR_NONNULL* PR_NULLABLE nodes, uint32_t count);
void domNodeReleaseEach(PrDomNode* const PR_NONNULL* PR_NULLABLE nodes, uint32_t count);

// Adds or releases a hold. A hold is a reference that the engine keeps for work that it has yet to do with the node,
// such as a task or a playback. The document's tree callback is notified of the first hold and of the last release.
void domNodeHold(PrDomNode* node);
void domNodeUnhold(PrDomNode* PR_NULLABLE node);

// Sets the document's tree callback. `held` of the new callback is called at once for each node that the engine holds.
// The callback that it replaces is not called again.
void domDocumentSetTreeCallback(PrDomDocument* document, const PrDomTreeCallback& callback);

// Returns the node that owns the node in its tree: its parent, an attribute's element, or a fragment's host. Returns
// null for a root.
inline PrDomNode* PR_NULLABLE domNodeOwner(const PrDomNode* node) {
    if (node->parent != nullptr)
        return node->parent;
    if (node->type != PrDomNodeType_DocumentFragment)
        return nullptr;
    return static_cast<const PrDomDocumentFragment*>(node)->host;
}

// Returns true if `ancestor` is `node`, or an ancestor of `node` or of the host of a fragment that `node` is in
// (dom/#concept-tree-host-including-inclusive-ancestor).
bool domIsHostIncludingInclusiveAncestor(const PrDomNode* ancestor, const PrDomNode* node);

// Returns the root of the node's tree. The root's `retained` counts the references to the tree.
PrDomNode* domTreeRoot(PrDomNode* node);

// Frees a root with no references to its tree, and the tree with it.
void domFreeTree(PrDomNode* root);

// Frees the contents that a template owns, which must have no references, and makes `shadow` its contents instead. The
// template of a declarative shadow root does this (html/#parsing-main-inhead). The template does not own `shadow`.
void domTemplateNameShadow(PrDomElement* element, PrDomShadowRoot* shadow);

// Frees a root that its creator is done with, and its tree, if nothing references the tree.
inline void domDropTree(PrDomNode* PR_NULLABLE root) {
    if (root != nullptr && root->retained == 0)
        domFreeTree(root);
}

// Returns the owner of the event handler `handler` of `target`. The WindowEventHandlers of a body or frameset element,
// and the handlers that the Window reflects (html/#window-reflecting-body-element-event-handler-set), belong to the
// element's Window. Their owner is null if the element has no Window. Every other handler belongs to the target.
PrDomEventTarget* PR_NULLABLE domEventHandlerTarget(PrDomEventTarget* target,
                                                    PrDomEventHandler handler);

// Calls `visit` with the node, its shadow-including descendants, and the attributes of those that are elements.
void domVisitSubtree(PrDomNode* root, void (*visit)(void* context, PrDomNode* node), void* context);

// Adds or releases a reference to the document for an object that is not a node, such as a live range.
void domDocumentRetain(PrDomDocument* document);
void domDocumentRelease(PrDomDocument* PR_NULLABLE document);

// Returns the document base URL (html/#document-base-url). The caller destroys it.
PrUrl* domDocumentBaseUrl(const PrDomDocument* document);

// Parses `text` as a URL against the document's base URL, with its query in the document's charset
// (html/#encoding-parsing-a-url). Returns the statuses of prUrlCreate.
PrStatus domDocumentParseUrl(const PrDomDocument* document, PrStringView text, PrUrl* PR_NULLABLE* PR_NONNULL out);

// Creates the element that `tag` names in `space`, or that `name` names for PrDomTag_Unknown. The element's tag comes
// from the tag table if the table holds the name in a namespace that it names elements of. Otherwise the tag is an atom
// of the document.
PrDomElement* domElementCreate(PrDomDocument* document, DomNamespaceId space, PrDomTag tag,
                               PrStringView name);

// Creates the element that `local` names in `space`, with `prefix` unless it is the null view. This is how
// dom/#concept-create-element creates an element from an already validated namespace and qualified name.
PrDomElement* domElementCreateNs(PrDomDocument* document, DomNamespaceId space, PrStringView prefix,
                                 PrStringView local);

// Create a node of `document` of the kind that each function names, from the given data or names.
PrDomText* domTextCreate(PrDomDocument* document, PrStringView data);
PrDomText* domTextCreateCdataSection(PrDomDocument* document, PrStringView data);
PrDomComment* domCommentCreate(PrDomDocument* document, PrStringView data);
PrDomProcessingInstruction* domProcessingInstructionCreate(PrDomDocument* document,
                                                           PrStringView target, PrStringView data);
PrDomDocumentType* domDocumentTypeCreate(PrDomDocument* document, PrStringView name, PrStringView publicId,
                                         PrStringView systemId);
PrDomDocumentFragment* domDocumentFragmentCreate(PrDomDocument* document);

// Attaches a shadow root to `host` (dom/#concept-attach-a-shadow-root). Returns NotSupported if `host` is not an
// element that a shadow root can be attached to, or if it already hosts one.
PrStatus domAttachShadow(PrDomElement* host, const PrDomShadowRootInit* init,
                         PrDomShadowRoot* PR_NULLABLE* PR_NONNULL out);

// Attaches the closed shadow root of a use element in the SVG namespace (SVG2/struct.html#UseShadowTree), which script
// cannot attach. The use must host no shadow root.
PrDomShadowRoot* domAttachUseShadow(PrDomElement* use);

// Returns true if the element's browsing context loads content for it. Such an element is an HTML element of embedded
// content (html/#embedded-content-category), an input, which can be an image button, or an SVG image or feImage
// element, whose href can name an image (SVG2/embedded.html#ImageElement, filter-effects-1/#feImageElement).
bool domEmbeds(const PrDomElement* element);

// Notifies the document's host of a change to an element for which domEmbeds is true. A null `attribute` means a
// connection change, and otherwise a change to that attribute. Does nothing for other elements.
void domEmbedChanged(PrDomElement* element, PrStringView attribute);

// The state that html/#script-processing-model keeps for a script element. An element that script creates has none of
// them: it has not started, it is not parser-inserted, and its force async is true.
enum {
    DomScriptFlag_Started = 1 << 0,         // html/#already-started
    DomScriptFlag_ParserInserted = 1 << 1,  // html/#parser-document is the element's document
    DomScriptFlag_NotForceAsync = 1 << 2,   // html/#script-force-async is false
};
typedef uint8_t DomScriptFlags;

// Returns true for a script element: an HTML script, or an SVG script (SVG2/interact.html#ScriptElement).
bool domIsScript(const PrDomElement* element);

// Returns or sets the DomScriptFlags of a script element.
DomScriptFlags domScriptFlags(const PrDomElement* element);
void domScriptSetFlags(PrDomElement* element, DomScriptFlags flags);

// Marks a script element that a parser created (html/#parsing-main-inhead, html/#scriptTagXML): parser-inserted, with
// force async false, or already started for a parser of a fragment, whose scripts never run. Does nothing for other
// elements.
void domScriptParserCreated(PrDomElement* element, bool fragment);

// Returns the state that only some elements have, creating it on first use.
DomElementState* domElementState(PrDomElement* element);

// Returns the shadow root that the element hosts, or null if it hosts none.
inline PrDomShadowRoot* PR_NULLABLE domShadowRootOfHost(const PrDomElement* element) {
    return element->state != nullptr ? element->state->shadowRoot : nullptr;
}

// Clones `node` into `document` (dom/#concept-node-clone). If `deep` is set, the clone includes the descendants and the
// contents of any template among them (html/#the-template-element). The clone is a detached root. Returns null for a
// document or an attribute, which are not cloned.
PrDomNode* PR_NULLABLE domClone(const PrDomNode* node, PrDomDocument* document, bool deep);

// ---------------------------------------------------------------------------------------------------------------------
// Tree queries

// Returns the text content of the node (dom/#dom-node-textcontent). This is the text of its descendants for an element
// or a fragment, the value for an attribute or character data, and the null string for any other node.
PrString domNodeTextContent(const PrDomNode* node);

// Returns true if `a` and `b` are equal (dom/#concept-node-equals).
bool domNodeEquals(const PrDomNode* a, const PrDomNode* b);

// Returns the next node in tree order that is not a descendant of `node`. This is its next sibling, or the next sibling
// of its nearest ancestor below `root` that has one. Returns null if there is none. `node` is `root` or a descendant of
// it.
inline const PrDomNode* PR_NULLABLE domNodeNextSkipping(const PrDomNode* root, const PrDomNode* node) {
    while (node != root) {
        if (node->next != nullptr)
            return node->next;
        node = node->parent;
    }
    return nullptr;
}
inline PrDomNode* PR_NULLABLE domNodeNextSkipping(const PrDomNode* root, PrDomNode* node) {
    return const_cast<PrDomNode*>(domNodeNextSkipping(root, static_cast<const PrDomNode*>(node)));
}

// Returns the next node in tree order among the descendants of `root`, or null past the last. `node` is `root` or a
// descendant of it. Attributes and template contents are not children.
inline const PrDomNode* PR_NULLABLE domNodeNext(const PrDomNode* root, const PrDomNode* node) {
    return node->first != nullptr ? node->first : domNodeNextSkipping(root, node);
}
inline PrDomNode* PR_NULLABLE domNodeNext(const PrDomNode* root, PrDomNode* node) {
    return const_cast<PrDomNode*>(domNodeNext(root, static_cast<const PrDomNode*>(node)));
}

// Returns the root of the node's tree (dom/#concept-tree-root): a document, a fragment or a detached node. An attribute
// is its own root, because its parent link refers to its element rather than a parent.
const PrDomNode* domNodeRoot(const PrDomNode* node);
inline PrDomNode* domNodeRoot(PrDomNode* node) {
    return const_cast<PrDomNode*>(domNodeRoot(static_cast<const PrDomNode*>(node)));
}

// Returns `node` as a shadow root, or null for any other node.
inline PrDomShadowRoot* PR_NULLABLE domNodeShadowRoot(const PrDomNode* PR_NULLABLE node) {
    return node != nullptr && (node->flags & DomNodeFlag_ShadowRoot) != 0
        ? static_cast<PrDomShadowRoot*>(const_cast<PrDomNode*>(node)) : nullptr;
}

// Returns the node's shadow-including root (dom/#concept-shadow-including-root). This is its root or, if the root is a
// shadow root, the shadow-including root of the host.
const PrDomNode* domShadowIncludingRoot(const PrDomNode* node);

// Returns the node after `node` among the shadow-including inclusive descendants of `root`, in shadow-including tree
// order (dom/#concept-shadow-including-tree-order), where a host's shadow root comes before the host's children.
// Returns null after the last.
PrDomNode* PR_NULLABLE domShadowIncludingNext(const PrDomNode* root, const PrDomNode* node);

// Returns true if the node's shadow-including root is a document (dom/#connected).
inline bool domIsConnected(const PrDomNode* node) {
    return (node->flags & DomNodeFlag_Connected) != 0;
}

// Returns the number of the node's ancestors, which is zero for a root. The node may not be an attribute, because the
// parent link of an attribute refers to its element.
uint32_t domNodeDepth(const PrDomNode* node);

// Returns true if `a` precedes `b` in tree order. Returns false for the same node, and true if `a` is an ancestor of
// `b`. Both nodes must be in the same tree, and neither may be an attribute.
bool domNodePrecedes(const PrDomNode* a, const PrDomNode* b);

// Returns the document element, which is the first child of `document` that is an element (dom/#document-trees).
const PrDomElement* PR_NULLABLE domDocumentElement(const PrDomDocument* document);
inline PrDomElement* PR_NULLABLE domDocumentElement(PrDomDocument* document) {
    return const_cast<PrDomElement*>(domDocumentElement(static_cast<const PrDomDocument*>(document)));
}

// Returns the head element (html/#dom-tree-accessors). This is the first HTML head element among the children of the
// document element, if the document element is an HTML html element.
PrDomElement* PR_NULLABLE domDocumentHead(PrDomDocument* document);

// Returns the body element (html/#dom-tree-accessors). This is the first HTML body or frameset element among the
// children of the document element, if the document element is an HTML html element.
PrDomElement* PR_NULLABLE domDocumentBody(PrDomDocument* document);

// One ID that connected elements of a tree have (dom/#concept-id), and the elements that have it.
struct DomIdEntry {
    PrString id;
    uint32_t hash;
    uint32_t count;                    // number of elements with the ID
    PrDomElement* PR_NULLABLE element; // the element if `count` is one, or null if it is not yet known
};

// The connected elements of the tree of a document or a shadow root, by ID. The entries are updated as elements are
// connected and disconnected, and as their IDs change.
struct DomIds {
    Array<DomIdEntry> entries;
    HashIndex index;
};

// Returns the first element in tree order among the descendants of `root` whose ID is `id`. Returns null for an empty
// `id` (dom/#interface-nonelementparentnode).
PrDomElement* PR_NULLABLE domElementById(PrDomNode* root, PrStringView id);

// Returns true if `root` is the root of a tree that keeps its elements by ID, which is a document or a connected shadow
// root.
bool domKeepsIds(const PrDomNode* root);

// Returns the number of elements with the ID `id` that the tree `root` keeps by ID. Writes the first of them in tree
// order to `first`, or null if there is none.
uint32_t domElementsById(PrDomNode* root, PrStringView id, PrDomElement* PR_NULLABLE* PR_NONNULL first);

// Finds the element for each ID of `root` that exactly one element has and whose element is not yet known. `root` must
// keep its elements by ID. Later lookups then write nothing and may run on several threads at once, as the lookups of a
// restyle do.
void domResolveIds(PrDomNode* root);

// ---------------------------------------------------------------------------------------------------------------------
// Linking nodes
//
// These functions run no insertion, removing or children changed steps.

// Runs the autofocus steps of an inserted element (html/#the-autofocus-attribute): a connected element with an
// autofocus attribute is appended to its document's autofocus candidates, and leaves its earlier place in them.
void domAutofocusInserted(PrDomElement* element);

// Links `child` into `parent` before `before`, or last if `before` is null. A child already in a tree is detached from
// it first. If `stepped` is given, the elements that the attachment connects and that have insertion steps are added to
// it, for the caller to run.
void domAttach(PrDomNode* parent, PrDomNode* child, PrDomNode* PR_NULLABLE before,
               Array<PrDomElement* PR_NONNULL>* PR_NULLABLE stepped = nullptr);

// Unlinks `child` from its parent and siblings, and keeps its descendants. The child becomes the root of its own tree,
// which takes the references to it. Neither tree is freed, whatever references it is left with. If `stepped` is given,
// the disconnected elements that have removing steps are added to it.
void domDetach(PrDomNode* child, Array<PrDomElement* PR_NONNULL>* PR_NULLABLE stepped = nullptr);

// Transfers a root's tree, including attributes and template contents, to `document` without moving nodes. The
// references to the tree and to the live ranges in it move with the tree. The root stays a root. The old document is
// destroyed if no reference to it remains.
void domAdopt(PrDomDocument* document, PrDomNode* node);

// ---------------------------------------------------------------------------------------------------------------------
// Mutation algorithms
//
// A HierarchyRequestError fails with PrStatus_NotAllowed, and a NotFoundError with PrStatus_NotFound. A null `child`
// means the end of `parent`'s children.

// Inserts `node` into `parent` before `child`, after checking that the insertion is valid
// (dom/#concept-node-pre-insert).
PrStatus domPreInsert(PrDomNode* parent, PrDomNode* node, PrDomNode* PR_NULLABLE child);

// Inserts `node` into `parent` before `child`, without checks (dom/#concept-node-insert).
void domInsert(PrDomNode* parent, PrDomNode* node, PrDomNode* PR_NULLABLE child);

// Replaces `child` of `parent` with `node` (dom/#concept-node-replace).
PrStatus domReplace(PrDomNode* parent, PrDomNode* node, PrDomNode* child);

// Replaces all the children of `parent` with `node`, or removes them all if `node` is null
// (dom/#concept-node-replace-all).
void domReplaceAll(PrDomNode* parent, PrDomNode* PR_NULLABLE node);

// Replaces all the children of `parent` with a Text node of `string`, or with nothing for the empty string
// (dom/#string-replace-all).
void domStringReplaceAll(PrDomNode* parent, PrStringView string);

// Removes `child` from `parent`, after checking that it is a child of `parent` (dom/#concept-node-pre-remove).
PrStatus domPreRemove(PrDomNode* parent, PrDomNode* child);

// Removes `node` from its parent (dom/#concept-node-remove). The node is freed with its tree if nothing references it.
void domRemove(PrDomNode* node);

// ---------------------------------------------------------------------------------------------------------------------
// ParentNode and ChildNode
//
// `nodes` holds `count` nodes and strings, which are the variadic arguments of these methods. Failures are reported in
// the same way as for the mutation algorithms.

// Inserts `nodes` before the first child of `parent` (dom/#dom-parentnode-prepend).
PrStatus domPrepend(PrDomNode* parent, const PrDomNodeOrString* PR_NULLABLE nodes, uint32_t count);

// Appends `nodes` to the children of `parent` (dom/#dom-parentnode-append).
PrStatus domAppend(PrDomNode* parent, const PrDomNodeOrString* PR_NULLABLE nodes, uint32_t count);

// Replaces the children of `parent` with `nodes` (dom/#dom-parentnode-replacechildren).
PrStatus domReplaceChildren(PrDomNode* parent, const PrDomNodeOrString* PR_NULLABLE nodes,
                            uint32_t count);

// Inserts `nodes` before `node` (dom/#dom-childnode-before).
PrStatus domBefore(PrDomNode* node, const PrDomNodeOrString* PR_NULLABLE nodes, uint32_t count);

// Inserts `nodes` after `node` (dom/#dom-childnode-after).
PrStatus domAfter(PrDomNode* node, const PrDomNodeOrString* PR_NULLABLE nodes, uint32_t count);

// Replaces `node` with `nodes` (dom/#dom-childnode-replacewith).
PrStatus domReplaceWith(PrDomNode* node, const PrDomNodeOrString* PR_NULLABLE nodes, uint32_t count);

// Removes `node` from its parent, if it has one (dom/#dom-childnode-remove).
void domRemoveFromParent(PrDomNode* node);

// ---------------------------------------------------------------------------------------------------------------------
// Text

// Sets the text content of the node (dom/#dom-node-textcontent).
void domSetTextContent(PrDomNode* node, PrStringView value);

// Returns the data of the node's Text children, in order (dom/#concept-child-text-content).
PrString domChildTextContent(const PrDomNode* node);

// Replaces the whole of the node's data with `value` (dom/#concept-cd-replace). The null view sets the empty string.
void domSetData(PrDomCharacterData* node, PrStringView value);

// Appends `data` to the node's data, which replaces no data at its end (dom/#concept-cd-replace). The cost is that of
// the appended data, not of the data that the node holds.
void domAppendData(PrDomCharacterData* node, PrStringView data);

// ---------------------------------------------------------------------------------------------------------------------
// Attributes

// Returns the element's attribute in `space` whose local name is `name`, or null if it has none.
const PrDomAttr* PR_NULLABLE domAttribute(const PrDomElement* element, DomNamespaceId space,
                                          PrStringView name);

// Returns the attribute that domAttribute finds, given `hash`, which is domAttributeHash(name).
const PrDomAttr* PR_NULLABLE domAttributeHashed(const PrDomElement* element, DomNamespaceId space,
                                                PrStringView name, uint32_t hash);

// Returns the value of the element's attribute `name` in no namespace, or the null view if it has none.
PrStringView domAttributeValue(const PrDomElement* element, PrStringView name);

// Returns true if the element has the attribute `name` in no namespace.
inline bool domHasAttribute(const PrDomElement* element, PrStringView name) {
    return domAttribute(element, PrDomNamespace_None, name) != nullptr;
}

// Adds the attribute in `space` whose local name is `name`, or replaces the value of the existing one.
void domSetAttribute(PrDomElement* element, DomNamespaceId space, PrStringView name,
                     PrStringView value);

// Sets an attribute's value in the same way as domSetAttribute (dom/#concept-element-attributes-set-value). An
// attribute that it adds has `prefix` unless `prefix` is the null view.
void domSetAttributeNs(PrDomElement* element, DomNamespaceId space, PrStringView prefix,
                       PrStringView name, PrStringView value);

// Creates an attribute of `document` on no element, as a detached root, with `prefix` unless `prefix` is the null view.
PrDomAttr* domAttrCreate(PrDomDocument* document, DomNamespaceId space, PrStringView prefix,
                         PrStringView name, PrStringView value);

// Appends `attribute`, which is on no element, to the element's attributes (dom/#concept-element-attributes-append).
// The attribute is first adopted into the element's document.
void domAppendAttribute(PrDomElement* element, PrDomAttr* attribute);

// Puts `attribute`, which is on no element, in the place of `old`, which becomes a detached root
// (dom/#concept-element-attributes-replace).
void domReplaceAttribute(PrDomElement* element, PrDomAttr* old, PrDomAttr* attribute);

// Sets the attribute's value (dom/#set-an-existing-attribute-value).
void domSetAttributeValue(PrDomAttr* attribute, PrStringView value);

// Returns the attribute's namespace prefix, or the null view if it has none.
PrStringView domAttributePrefix(const PrDomAttr* attribute);

// Returns the element's first attribute whose qualified name is `qualifiedName`
// (dom/#concept-element-attributes-get-by-name). The name must be in ASCII lowercase for an HTML element in an HTML
// document.
PrDomAttr* PR_NULLABLE domAttributeByName(const PrDomElement* element, PrStringView qualifiedName);

// Removes the attribute that domAttribute finds, if any (dom/#concept-element-attributes-remove-by-name). The attribute
// becomes a detached root.
void domRemoveAttribute(PrDomElement* element, DomNamespaceId space, PrStringView name);

// Removes `attribute` from the element (dom/#concept-element-attributes-remove). The attribute becomes a detached root.
void domRemoveAttributeNode(PrDomElement* element, PrDomAttr* attribute);

// Returns true if `name` is a valid element local name (dom/#valid-element-local-name).
bool domIsValidElementLocalName(PrStringView name);

// Returns true if `name` is a valid attribute local name (dom/#valid-attribute-local-name).
bool domIsValidAttributeLocalName(PrStringView name);

// Returns the value of the element's id attribute, or the null view if it has none (dom/#concept-id).
PrStringView domElementId(const PrDomElement* element);

// Returns the element's qualified name (dom/#concept-element-qualified-name). This is its local name, preceded by its
// prefix and a colon if it has a prefix.
PrString domElementQualifiedName(const PrDomElement* element);

// Returns true if the element is an HTML element in an HTML document. The names that a script or a selector gives for
// such an element are lowercased before they are compared or written.
inline bool domIsHtmlInHtmlDocument(const PrDomElement* element) {
    return element->space == PrDomNamespace_Html && element->document->kind == DomDocumentKind_Html;
}

// Returns the element's HTML-uppercased qualified name (dom/#element-html-uppercased-qualified-name).
PrString domElementTagName(const PrDomElement* element);

// Returns the length of the node's data in UTF-16 code units (dom/#interface-characterdata).
uint32_t domCharacterDataLength(const PrDomCharacterData* node);

// The data are UTF-8, and offsets and counts into them are in UTF-16 code units, which are the units that script uses.
// A code unit range that splits a code point outside the BMP keeps each half that it holds as U+FFFD, because UTF-8 has
// no lone surrogate.

// Writes `count` code units of the data from `offset`, or as many as there are (dom/#concept-cd-substring). Returns
// OutOfRange if `offset` is past the data's length.
PrStatus domSubstringData(const PrDomCharacterData* node, uint32_t offset, uint32_t count,
                          PrString* out);

// Replaces `count` code units of the data from `offset` with `data` (dom/#concept-cd-replace). Returns OutOfRange if
// `offset` is past the data's length.
PrStatus domReplaceData(PrDomCharacterData* node, uint32_t offset, uint32_t count, PrStringView data);

// Splits the node at `offset` (dom/#concept-text-split), and returns the new text node. The new node follows the node
// if the node has a parent. Returns null if `offset` is past the data's length.
PrDomText* PR_NULLABLE domSplitText(PrDomText* node, uint32_t offset);

// Removes the empty Text nodes in the node's subtree, and merges each run of adjacent Text nodes into the first node of
// the run (dom/#dom-node-normalize).
void domNormalize(PrDomNode* node);

// Checks that `node` can be inserted into `parent` before `child` (dom/#concept-node-ensure-pre-insertion-validity).
PrStatus domValidateInsertion(const PrDomNode* parent, const PrDomNode* node,
                              const PrDomNode* PR_NULLABLE child);

// Returns true if an element's tag is an atom of its document's local names rather than a PrDomTag.
inline bool domTagIsAtom(DomTagId tag) {
    return tag >= PrDomTag_Count;
}

// Returns the element's local name as a tag, or PrDomTag_Unknown if no tag names it.
inline PrDomTag domElementTag(const PrDomElement* element) {
    return domTagIsAtom(element->tag) ? PrDomTag_Unknown : PrDomTag(element->tag);
}

// Returns the element's local name.
inline PrStringView domElementName(const PrDomElement* element) {
    if (!domTagIsAtom(element->tag))
        return domTagName(PrDomTag(element->tag));
    return atomTableText(element->document->localNames, element->tag - PrDomTag_Count);
}

// Returns `target` as a node, or null if `target` is not a node.
inline PrDomNode* PR_NULLABLE domNodeFromTarget(PrDomEventTarget* PR_NULLABLE target) {
    bool node = target != nullptr && target->type >= PrDomNodeType_Element
                && target->type <= PrDomNodeType_Notation;
    return node ? static_cast<PrDomNode*>(target) : nullptr;
}

// Returns the node's parent (dom/#concept-tree-parent). Returns null for an attribute and for a detached root.
inline PrDomNode* PR_NULLABLE domNodeParent(PrDomNode* node) {
    return node->type != PrDomNodeType_Attribute ? node->parent : nullptr;
}
inline const PrDomNode* PR_NULLABLE domNodeParent(const PrDomNode* node) {
    return node->type != PrDomNodeType_Attribute ? node->parent : nullptr;
}

// Returns true if the node is a Text node (dom/#text). A CDATA section is a Text node.
inline bool domIsText(const PrDomNode* node) {
    return node->type == PrDomNodeType_Text || node->type == PrDomNodeType_CdataSection;
}

// Returns `node` as an element, or null for any other kind of node.
inline const PrDomElement* PR_NULLABLE domElementFromNode(const PrDomNode* PR_NULLABLE node) {
    return node != nullptr && node->type == PrDomNodeType_Element
        ? static_cast<const PrDomElement*>(node) : nullptr;
}
inline PrDomElement* PR_NULLABLE domElementFromNode(PrDomNode* PR_NULLABLE node) {
    return const_cast<PrDomElement*>(domElementFromNode(static_cast<const PrDomNode*>(node)));
}

// Returns the node's parent if it is an element, or null.
inline const PrDomElement* PR_NULLABLE domNodeParentElement(const PrDomNode* node) {
    return domElementFromNode(node->parent);
}

// Returns true if the element is the HTML element `tag`.
inline bool domIsHtml(const PrDomElement* PR_NULLABLE element, PrDomTag tag) {
    return element != nullptr && element->space == PrDomNamespace_Html && element->tag == tag;
}

// Returns the node's parent, or its host if it is a shadow root (dom/#concept-shadow-including-descendant).
inline const PrDomNode* PR_NULLABLE domShadowIncludingParent(const PrDomNode* node) {
    if (const PrDomShadowRoot* shadow = domNodeShadowRoot(node))
        return shadow->host;
    return domNodeParent(node);
}
inline PrDomNode* PR_NULLABLE domShadowIncludingParent(PrDomNode* node) {
    return const_cast<PrDomNode*>(domShadowIncludingParent(static_cast<const PrDomNode*>(node)));
}

// Returns true if `ancestor` is `node` or a shadow-including ancestor of it
// (dom/#concept-shadow-including-inclusive-ancestor).
inline bool domShadowIncludingContains(const PrDomNode* ancestor, const PrDomNode* node) {
    for (const PrDomNode* at = node; at != nullptr; at = domShadowIncludingParent(at)) {
        if (at == ancestor)
            return true;
    }
    return false;
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
