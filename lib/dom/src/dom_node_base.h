// The node tree: the base of every node.
//
// dom - https://dom.spec.whatwg.org/

#pragma once

#include "pr/dom.h"
#include "pr/draw_image.h"
#include "pr/hash_index.h"
#include "pr/image.h"
#include "pr/layout_tree.h"
#include "pr/name_table.h"
#include "pr/raw_array.h"
#include "pr/url.h"

#include "dom/src/dom_class.h"
#include "dom/src/dom_event.h"
#include "dom/src/dom_tag.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct CssElementStyle;
struct CssMediaEnvironment;
struct CssRoot;
struct CssSelectorCache;
struct DomControl;
struct DomCustomElement;
struct DomDocument;
struct DomDocumentHooks;
struct DomForm;
struct DomIds;
struct DomMutationObservers;
struct DomNode;
struct DomRange;
struct DomResources;
struct DomSelection;
struct DomShadowRoot;
struct DomSlot;
struct DomSlots;
struct DomUses;

// The playback state of an HTML media element, which the pseudo-classes of that state read (selectors-4/#video-state,
// selectors-4/#sound-state). lib/html updates it as the element plays.
enum {
    // Set if the other flags are tracked. An element whose flags are not tracked is paused, and is muted if its muted
    // attribute is present.
    DomMediaFlag_Tracked = 1 << 0,

    DomMediaFlag_Playing = 1 << 1,
    DomMediaFlag_Paused = 1 << 2,
    DomMediaFlag_Seeking = 1 << 3,
    DomMediaFlag_Buffering = 1 << 4,
    DomMediaFlag_Stalled = 1 << 5,
    DomMediaFlag_Muted = 1 << 6,
    DomMediaFlag_VolumeLocked = 1 << 7,
};
typedef uint8_t DomMediaFlags;

// State that only some elements have, created on first use.
struct DomElementState {
    PrDomShadowRoot* PR_NULLABLE shadowRoot; // shadow root that the element hosts (dom/#concept-element-shadow-root)
    PrDomNode* PR_NULLABLE contents;         // fragment that a template element holds (html/#the-template-element)
    PrRawString prefix;                      // dom/#concept-element-namespace-prefix, or the null string for none
    PrRawString returnValue;                 // html/#dom-dialog-returnvalue
    bool modal;                              // html/#is-modal
    bool clicking;                           // html/#click-in-progress-flag

    // For an element of embedded content, the number of updates to the content that its browsing context loads. The
    // events of an update that a later update superseded are dropped.
    uint32_t generation;

    PrDomImageState image;                // state of an image's current request (html/#current-request)
    PrRawString imageUrl;                 // URL of the current request, or the null string for none
    DomSlot* PR_NULLABLE slot;            // state of a slot (dom/#concept-slot), created on first assignment
    DomCustomElement* PR_NULLABLE custom; // custom element data, created on first use
    DomControl* PR_NULLABLE control;      // state of a form-associated element (dom_control.h)
    DomForm* PR_NULLABLE form;            // state of a form element

    // The natural size of the current request, as its orientation displays it and corrected for its pixel density
    // (html/#density-corrected-intrinsic-width-and-height).
    uint32_t naturalWidth;
    uint32_t naturalHeight;

    double density;                       // html/#current-pixel-density of the current request, or 0 for 1

    // The image of the current request, if the host decodes images. `picture` is arranged as the image's orientation
    // displays it, and `orientation` is how it was arranged from the stored pixels. `storedPicture` is the stored
    // arrangement, made the first time that image-orientation asks for it.
    const PrDrawImage* PR_NULLABLE picture;
    PrImageOrientation orientation;
    const PrDrawImage* PR_NULLABLE storedPicture;

    // What draws the element's content at the size shown, in place of `picture`: an SVG image of the current request,
    // or a canvas's bitmap; and the revision of what it draws (PrLayoutContent::revision).
    PrLayoutPainter vector;
    uint64_t vectorRevision;
    PrLayoutNaturalSize vectorNatural;                  // the SVG image's natural dimensions
    uint64_t embed;                                     // embedded content that an iframe shows
                                                        // (PrLayoutContentKind_Embed), or zero
    DomMediaFlags media;                                // playback state of an HTML media element
    void* PR_NULLABLE embedded;                         // data that the module running the content keeps for it
    void (*PR_NULLABLE forgetEmbedded)(void* embedded); // called when the element is freed
    uint8_t script;                                     // the DomScriptFlags of a script element
    uint8_t linkEvent;                                  // the event that a link has yet to fire (html_link.h)
};

// ---------------------------------------------------------------------------------------------------------------------
// Names

// The namespace of an element or an attribute. A value below PrDomNamespace_Other is a PrDomNamespace that the specs
// name. A value from PrDomNamespace_Other on is a namespace that a script names, and the document holds its URL
// (DomDocument::namespaces).
typedef uint8_t DomNamespaceId;

// ---------------------------------------------------------------------------------------------------------------------
// Nodes

// The states that a node records as flags.
enum {
    DomNodeFlag_ShadowRoot = 1 << 0,     // a document fragment that is a DomShadowRoot
    DomNodeFlag_Connected = 1 << 1,      // dom/#connected
    DomNodeFlag_Id = 1 << 2,             // an element whose ID (dom/#concept-id) is not empty
    DomNodeFlag_Counting = 1 << 3,       // an owner at which domNodeRetainEach or domNodeReleaseEach is counting
    DomNodeFlag_OptionSelected = 1 << 4, // selectedness of an option (html/#concept-option-selectedness)
    DomNodeFlag_OptionDirty = 1 << 5,    // dirtiness of an option (html/#concept-option-dirtiness)

    // The disabled state of a selectedcontent element (html/#the-selectedcontent-element).
    DomNodeFlag_SelectedContentDisabled = 1 << 6,

    // A slottable that a slot's assigned nodes include (dom/#slotable-assigned-slot), which DomSlots::slotted finds.
    DomNodeFlag_Slotted = 1 << 7,
};
typedef uint8_t DomNodeFlags;

// The work that the presentation of a connected node has to redo for the node at its next update.
enum {
    DomStyleFlag_Self = 1 << 0,       // an element's style, or a text node's data
    DomStyleFlag_Descendant = 1 << 1, // something for a descendant
    DomStyleFlag_Subtree = 1 << 2,    // the style of every descendant
    DomStyleFlag_Children = 1 << 3,   // the boxes of the children, which were added or removed

    // The style of each descendant that carries a feature that the presentation's pending invalidations name for the
    // node.
    DomStyleFlag_Features = 1 << 4,

    DomStyleFlag_Animate = 1 << 5,    // an element's animations, sampled at the presentation's time over the base style
    DomStyleFlag_Content = 1 << 6,    // what a replaced element shows: its natural size and content
};
typedef uint8_t DomStyleFlags;

// What styles read about a node's children. This decides which elements a child that is added or removed restyles.
enum {
    DomStructureFlag_Empty = 1 << 0,      // whether there are any children (:empty), which restyles the node

    // The element right before a child, which :first-child or a lone `+` reads. Restyles the element after a change.
    DomStructureFlag_Next = 1 << 1,

    // The element right after a child, which :last-child reads. Restyles the element before a change.
    DomStructureFlag_Previous = 1 << 2,

    DomStructureFlag_Forward = 1 << 3,    // the elements before a child, which restyles every element after a change
    DomStructureFlag_Backward = 1 << 4,   // the elements after a child, which restyles every element before a change

    // A read made on behalf of an element other than the child whose siblings were read, and which a selector reaches
    // from that child. The children that a change restyles then restyle their subtrees too.
    DomStructureFlag_Descendants = 1 << 5,
};
typedef uint8_t DomStructureFlags;

// The base type of every node. Nodes are allocated one by one and keep their addresses while their tree lives. A node
// owns its children. An element also owns its attributes, the shadow root that it hosts, and, for a template, its
// contents. The fragment of a template's contents refers to the template as its host. domNodeOwner returns the owner. A
// node with no owner is the root of its own tree, which its document lists. A root's parent is null, and its `previous`
// and `next` link that list instead of siblings.
//
// An attribute's parent is its element, and its `previous` and `next` link the element's attributes. Attributes are not
// children, so code that transfers ownership visits them separately.
struct DomNode : PrDomEventTarget {
    DomNamespaceId space;               // namespace of an element or an attribute, or PrDomNamespace_None
    DomNodeFlags flags;

    // The custom element state of an element (dom/#concept-element-custom-element-state).
    PrDomCustomElementState customState;

    DomStyleFlags pending;               // work that the document's presentation has to redo for the node
    uint32_t holds;                      // the engine's holds on the node (domNodeHold), each also a reference
    PrDomDocument* document;
    PrDomNode* PR_NULLABLE parent;
    PrDomNode* PR_NULLABLE first;
    PrDomNode* PR_NULLABLE last;
    PrDomNode* PR_NULLABLE previous;
    PrDomNode* PR_NULLABLE next;
    uint32_t references;              // counted references to the node, which keep its tree alive

    // 1 + the node's entry in its document's presentation (CssPresentation::entries), or 0 for none. A presentation
    // checks that the entry refers to the node before using it. A value left by a presentation that has since been
    // destroyed, or by an entry that has since been reused, therefore refers to nothing.
    mutable uint32_t presentationEntry;

    // The counted references to what the node owns: itself, its descendants, its attributes and the fragments that it
    // owns. A root's count is the count of its tree. A document's count also includes the trees that refer to it.
    uint32_t retained;
};

} // namespace pr_impl

// C opaque types
struct PrDomNode : pr_impl::DomNode {};

PR_ASSUME_NONNULL_END
