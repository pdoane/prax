// Document hooks
//
// The libraries above the DOM keep state that follows a document's tree: CSS keeps the sheets that their owners give
// the tree and the style data of nodes, and the presentation of the document keeps what it derives from them. Each
// subscribes to the document with a table of the changes that it follows, and the DOM reports each change to its
// subscribers in the order in which they subscribed, and each freeing in the reverse order. A document with no
// subscribers runs the DOM alone. A document that the DOM creates for another document, such as by DOMImplementation,
// is reported to the creator's subscribers, which subscribe to it if they follow it too.

#pragma once

#include "pr/dom.h"
#include "pr/integer.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"
#include "pr/text_buffer.h"

#include "dom/src/dom_node.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// What inserted a style or link element. This decides whether its sheet blocks the document's scripts and rendering
// while it is fetched (html/#contributes-a-script-blocking-style-sheet, html/#potentially-render-blocking).
enum {
    DomInsertedBy_Parser,
    DomInsertedBy_Script,
};
typedef uint8_t DomInsertedBy;

// The changes that a subscriber of a document follows. Each function takes the subscriber's context, and is called on
// the document's agent thread. A function is null for a change that the subscriber does not follow.
struct DomDocumentHooks {
    // A node is linked into its parent, or is about to be unlinked from it.
    void (*PR_NULLABLE inserted)(void* PR_NULLABLE context, PrDomNode* node);
    void (*PR_NULLABLE removing)(void* PR_NULLABLE context, PrDomNode* node);

    // An attribute of the element changed, with its namespace and local name, the old value and the new value. Each
    // value is the null view for none. The subscriber runs the steps that it takes for the change, such as those of the
    // style attribute (cssom-1/#the-elementcssinlinestyle-mixin) and of the attributes of a sheet's owner.
    void (*PR_NULLABLE attributeChanged)(void* PR_NULLABLE context, PrDomElement* element, DomNamespaceId space,
        PrStringView name, PrStringView oldValue, PrStringView value);

    // The node's data changed.
    void (*PR_NULLABLE dataChanged)(void* PR_NULLABLE context, PrDomNode* node);

    // The flat tree changed (css-shadow-1/#flattening): the slot's assigned nodes changed from `before`, or the element
    // became a shadow host, whose shadow tree replaces its children.
    void (*PR_NULLABLE slotChanged)(void* PR_NULLABLE context, PrDomElement* slot,
        PrDomNode* const PR_NONNULL* PR_NULLABLE before, uint32_t beforeCount);
    void (*PR_NULLABLE shadowAttached)(void* PR_NULLABLE context, PrDomElement* host);

    // The element gained or lost a wheel listener that is not passive (domHasBlockingWheelListener).
    void (*PR_NULLABLE wheelListenersChanged)(void* PR_NULLABLE context, PrDomElement* element);

    // A state of the element that no attribute decides changed, such as whether it is a defined custom element, its
    // custom states, or the state of a form control.
    void (*PR_NULLABLE statesChanged)(void* PR_NULLABLE context, PrDomElement* element);

    // The document's default language changed (html/#pragma-set-default-language), or its meta elements changed in a
    // way that can change the page's supported color schemes.
    void (*PR_NULLABLE languageChanged)(void* PR_NULLABLE context, PrDomDocument* document);
    void (*PR_NULLABLE metaChanged)(void* PR_NULLABLE context, PrDomDocument* document);

    // A resource of the document loaded (dom_resource.h).
    void (*PR_NULLABLE resourceLoaded)(void* PR_NULLABLE context, PrDomDocument* document);

    // Appends a color input's value after the steps of html/#update-a-color-well-control-color, given `text`, the value
    // or value attribute that the steps start from. The first subscriber that has it gives the value.
    void (*PR_NULLABLE colorValue)(void* PR_NULLABLE context, const PrDomElement* input, PrStringView text,
        PrTextBuffer* out);

    // The update steps of the owners of style sheets: an HTML or SVG style element's block
    // (html/#update-a-style-block), an HTML link element (html/#link-type-stylesheet), and an xml-stylesheet processing
    // instruction that the parser inserted. The DOM reports them for those elements and for processing instructions
    // alone.
    void (*PR_NULLABLE styleBlockUpdate)(void* PR_NULLABLE context, PrDomElement* element, DomInsertedBy by);
    void (*PR_NULLABLE linkUpdate)(void* PR_NULLABLE context, PrDomElement* element, DomInsertedBy by);
    void (*PR_NULLABLE instructionInserted)(void* PR_NULLABLE context, PrDomNode* node);

    // An attribute in no namespace of an owner of a sheet changed: of an HTML or SVG style element, or of an HTML link
    // element. `value` is the null view for a removal. It follows attributeChanged.
    void (*PR_NULLABLE ownerAttributeChanged)(void* PR_NULLABLE context, PrDomElement* element, PrStringView name,
        PrStringView value);

    // A processing instruction is about to be unlinked from its parent, after which `removing` follows. An
    // xml-stylesheet instruction's sheet leaves with it.
    void (*PR_NULLABLE instructionRemoving)(void* PR_NULLABLE context, PrDomNode* node);

    // The DOM created `created` for the subscriber's document, such as by DOMImplementation.
    void (*PR_NULLABLE documentCreated)(void* PR_NULLABLE context, PrDomDocument* created);

    // The element, shadow root or document is being freed. Each frees the state that the subscriber keeps for it. An
    // element or shadow root is reported only if it has style data (DomElement::style, DomShadowRoot::style), the only
    // state that subscribers keep for one.
    void (*PR_NULLABLE elementDestroyed)(void* PR_NULLABLE context, PrDomElement* element);
    void (*PR_NULLABLE shadowRootDestroyed)(void* PR_NULLABLE context, PrDomShadowRoot* shadow);
    void (*PR_NULLABLE documentDestroyed)(void* PR_NULLABLE context, PrDomDocument* document);
};

// The entries of DomDocumentHooks, by index. A document keeps, for each, the subscribers that have it
// (DomDocument::followers), so that a change that no subscriber follows costs its report a test of one byte, and a
// change that some follow reaches those alone.
enum : uint8_t {
    DomHook_Inserted,
    DomHook_Removing,
    DomHook_AttributeChanged,
    DomHook_DataChanged,
    DomHook_SlotChanged,
    DomHook_ShadowAttached,
    DomHook_WheelListenersChanged,
    DomHook_StatesChanged,
    DomHook_LanguageChanged,
    DomHook_MetaChanged,
    DomHook_ResourceLoaded,
    DomHook_ColorValue,
    DomHook_StyleBlockUpdate,
    DomHook_LinkUpdate,
    DomHook_InstructionInserted,
    DomHook_OwnerAttributeChanged,
    DomHook_InstructionRemoving,
    DomHook_DocumentCreated,
    DomHook_ElementDestroyed,
    DomHook_ShadowRootDestroyed,
    DomHook_DocumentDestroyed,
    DomHook_Count,
};
typedef uint8_t DomHook;

// Adds a subscriber to the document, after its others, and returns true. Returns false, and changes nothing, if the
// document has a subscriber with `hooks`. The document has room for kDomDocumentSubscribers.
bool domDocumentSubscribe(PrDomDocument* document, const DomDocumentHooks* hooks, void* PR_NULLABLE context);

// Removes the subscriber with `hooks` from the document, keeping the order of the others.
void domDocumentUnsubscribe(PrDomDocument* document, const DomDocumentHooks* hooks);

// Returns the document's subscriber with `hooks`, or null if it has none.
const DomDocumentSubscriber* PR_NULLABLE domDocumentSubscriber(const PrDomDocument* document, const DomDocumentHooks* hooks);

// Call `fn` with the hooks and the context of each of the document's subscribers that follow `hook`: in the order in
// which they subscribed, or in the reverse order.
template <typename Fn>
inline void domEachFollower(const PrDomDocument* document, DomHook hook, Fn fn) {
    for (uint64_t followers = document->followers[hook]; followers != 0; followers &= followers - 1) {
        const DomDocumentSubscriber& subscriber = document->subscribers[prU64LowestBit(followers)];
        fn(*subscriber.hooks, subscriber.context);
    }
}
template <typename Fn>
inline void domEachFollowerReversed(const PrDomDocument* document, DomHook hook, Fn fn) {
    uint32_t followers = document->followers[hook];
    for (uint32_t i = kDomDocumentSubscribers; i-- != 0;) {
        if ((followers & (1u << i)) != 0)
            fn(*document->subscribers[i].hooks, document->subscribers[i].context);
    }
}

// Each reports a change to the subscribers that follow it, of the document of the node, element or document that
// changed.

// Reports that the node was linked into its parent.
inline void domNotifyInserted(PrDomNode* node) {
    if (node->document->followers[DomHook_Inserted] == 0)
        return;
    domEachFollower(node->document, DomHook_Inserted, [&](const DomDocumentHooks& hooks, void* PR_NULLABLE context) {
        hooks.inserted(context, node);
    });
}

// Reports that the node is about to be unlinked from its parent, to the subscribers that follow a processing
// instruction's removal first.
inline void domNotifyRemoving(PrDomNode* node) {
    PrDomDocument* document = node->document;
    if (node->type == PrDomNodeType_ProcessingInstruction && document->followers[DomHook_InstructionRemoving] != 0) {
        domEachFollower(document, DomHook_InstructionRemoving, [&](const DomDocumentHooks& hooks, void* PR_NULLABLE context) {
            hooks.instructionRemoving(context, node);
        });
    }
    if (document->followers[DomHook_Removing] == 0)
        return;
    domEachFollower(document, DomHook_Removing, [&](const DomDocumentHooks& hooks, void* PR_NULLABLE context) {
        hooks.removing(context, node);
    });
}

// Reports that an attribute of the element changed.
inline void domNotifyAttributeChanged(PrDomElement* element, DomNamespaceId space, PrStringView name,
        PrStringView oldValue, PrStringView value) {
    if (element->document->followers[DomHook_AttributeChanged] == 0)
        return;
    domEachFollower(element->document, DomHook_AttributeChanged, [&](const DomDocumentHooks& hooks, void* PR_NULLABLE context) {
        hooks.attributeChanged(context, element, space, name, oldValue, value);
    });
}

// Reports that the node's data changed.
inline void domNotifyDataChanged(PrDomNode* node) {
    if (node->document->followers[DomHook_DataChanged] == 0)
        return;
    domEachFollower(node->document, DomHook_DataChanged, [&](const DomDocumentHooks& hooks, void* PR_NULLABLE context) {
        hooks.dataChanged(context, node);
    });
}

// Reports that the slot's assigned nodes changed from `before`.
inline void domNotifySlotChanged(PrDomElement* slot, PrDomNode* const PR_NONNULL* PR_NULLABLE before,
        uint32_t beforeCount) {
    if (slot->document->followers[DomHook_SlotChanged] == 0)
        return;
    domEachFollower(slot->document, DomHook_SlotChanged, [&](const DomDocumentHooks& hooks, void* PR_NULLABLE context) {
        hooks.slotChanged(context, slot, before, beforeCount);
    });
}

// Reports that the element became a shadow host.
inline void domNotifyShadowAttached(PrDomElement* host) {
    if (host->document->followers[DomHook_ShadowAttached] == 0)
        return;
    domEachFollower(host->document, DomHook_ShadowAttached, [&](const DomDocumentHooks& hooks, void* PR_NULLABLE context) {
        hooks.shadowAttached(context, host);
    });
}

// Reports that the element gained or lost a wheel listener that is not passive.
inline void domNotifyWheelListenersChanged(PrDomElement* element) {
    if (element->document->followers[DomHook_WheelListenersChanged] == 0)
        return;
    domEachFollower(element->document, DomHook_WheelListenersChanged,
        [&](const DomDocumentHooks& hooks, void* PR_NULLABLE context) {
            hooks.wheelListenersChanged(context, element);
        });
}

// Reports that a state of the element that no attribute decides changed.
inline void domNotifyStatesChanged(PrDomElement* element) {
    if (element->document->followers[DomHook_StatesChanged] == 0)
        return;
    domEachFollower(element->document, DomHook_StatesChanged, [&](const DomDocumentHooks& hooks, void* PR_NULLABLE context) {
        hooks.statesChanged(context, element);
    });
}

// Reports that the document's default language changed.
inline void domNotifyLanguageChanged(PrDomDocument* document) {
    if (document->followers[DomHook_LanguageChanged] == 0)
        return;
    domEachFollower(document, DomHook_LanguageChanged, [&](const DomDocumentHooks& hooks, void* PR_NULLABLE context) {
        hooks.languageChanged(context, document);
    });
}

// Reports that the document's meta elements changed.
inline void domNotifyMetaChanged(PrDomDocument* document) {
    if (document->followers[DomHook_MetaChanged] == 0)
        return;
    domEachFollower(document, DomHook_MetaChanged, [&](const DomDocumentHooks& hooks, void* PR_NULLABLE context) {
        hooks.metaChanged(context, document);
    });
}

// Reports that a resource of the document loaded.
inline void domNotifyResourceLoaded(PrDomDocument* document) {
    if (document->followers[DomHook_ResourceLoaded] == 0)
        return;
    domEachFollower(document, DomHook_ResourceLoaded, [&](const DomDocumentHooks& hooks, void* PR_NULLABLE context) {
        hooks.resourceLoaded(context, document);
    });
}

// Appends a color input's value: the one that the first subscriber that gives one gives. A document whose subscribers
// give none has no colors to parse, and a color input keeps its value as given.
inline void domNotifyColorValue(const PrDomElement* input, PrStringView text, PrTextBuffer* out) {
    uint32_t followers = input->document->followers[DomHook_ColorValue];
    if (followers == 0) {
        prTextBufferAppend(out, text);
        return;
    }
    const DomDocumentSubscriber& subscriber = input->document->subscribers[prU64LowestBit(followers)];
    subscriber.hooks->colorValue(subscriber.context, input, text, out);
}

// Reports the update of an HTML or SVG style element's block.
inline void domNotifyStyleBlockUpdate(PrDomElement* element, DomInsertedBy by = DomInsertedBy_Script) {
    if (element->document->followers[DomHook_StyleBlockUpdate] == 0 || element->tag != PrDomTag_Style ||
            (element->space != PrDomNamespace_Html && element->space != PrDomNamespace_Svg))
        return;
    domEachFollower(element->document, DomHook_StyleBlockUpdate, [&](const DomDocumentHooks& hooks, void* PR_NULLABLE context) {
        hooks.styleBlockUpdate(context, element, by);
    });
}

// Reports the update of an HTML link element.
inline void domNotifyLinkUpdate(PrDomElement* element, DomInsertedBy by = DomInsertedBy_Script) {
    if (element->document->followers[DomHook_LinkUpdate] == 0 || !domIsHtml(element, PrDomTag_Link))
        return;
    domEachFollower(element->document, DomHook_LinkUpdate, [&](const DomDocumentHooks& hooks, void* PR_NULLABLE context) {
        hooks.linkUpdate(context, element, by);
    });
}

// Reports a change to an attribute in no namespace of an owner of a sheet, to the owners alone.
inline void domNotifyOwnerAttributeChanged(PrDomElement* element, DomNamespaceId space, PrStringView name, PrStringView value) {
    if (element->document->followers[DomHook_OwnerAttributeChanged] == 0 || space != PrDomNamespace_None ||
            (element->tag != PrDomTag_Style && element->tag != PrDomTag_Link) || (element->space != PrDomNamespace_Html &&
                (element->space != PrDomNamespace_Svg || element->tag != PrDomTag_Style)))
        return;
    domEachFollower(element->document, DomHook_OwnerAttributeChanged,
        [&](const DomDocumentHooks& hooks, void* PR_NULLABLE context) {
            hooks.ownerAttributeChanged(context, element, name, value);
        });
}

// Reports that the parser inserted a processing instruction.
inline void domNotifyInstructionInserted(PrDomNode* node) {
    if (node->document->followers[DomHook_InstructionInserted] == 0 ||
            node->type != PrDomNodeType_ProcessingInstruction)
        return;
    domEachFollower(node->document, DomHook_InstructionInserted, [&](const DomDocumentHooks& hooks, void* PR_NULLABLE context) {
        hooks.instructionInserted(context, node);
    });
}

// Reports that the DOM created `created` for `creator`.
inline void domNotifyDocumentCreated(PrDomDocument* creator, PrDomDocument* created) {
    if (creator->followers[DomHook_DocumentCreated] == 0)
        return;
    domEachFollower(creator, DomHook_DocumentCreated, [&](const DomDocumentHooks& hooks, void* PR_NULLABLE context) {
        hooks.documentCreated(context, created);
    });
}

// Reports that the element is being freed, if it has style data.
inline void domNotifyElementDestroyed(PrDomElement* element) {
    if (element->document->followers[DomHook_ElementDestroyed] == 0 || element->style == nullptr)
        return;
    domEachFollowerReversed(element->document, DomHook_ElementDestroyed,
        [&](const DomDocumentHooks& hooks, void* PR_NULLABLE context) {
            hooks.elementDestroyed(context, element);
        });
}

// Reports that the shadow root is being freed, if it has style data.
inline void domNotifyShadowRootDestroyed(PrDomShadowRoot* shadow) {
    if (shadow->document->followers[DomHook_ShadowRootDestroyed] == 0 || shadow->style == nullptr)
        return;
    domEachFollowerReversed(shadow->document, DomHook_ShadowRootDestroyed,
        [&](const DomDocumentHooks& hooks, void* PR_NULLABLE context) {
            hooks.shadowRootDestroyed(context, shadow);
        });
}

// Reports that the document is being freed.
inline void domNotifyDocumentDestroyed(PrDomDocument* document) {
    if (document->followers[DomHook_DocumentDestroyed] == 0)
        return;
    domEachFollowerReversed(document, DomHook_DocumentDestroyed, [&](const DomDocumentHooks& hooks, void* PR_NULLABLE context) {
        hooks.documentDestroyed(context, document);
    });
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
