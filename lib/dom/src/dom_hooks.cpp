#include "dom/src/dom_hooks.h"

#include "pr/assert.h"
#include "pr/mem.h"

namespace pr_impl {
namespace {

static_assert(kDomDocumentHooks == DomHook_Count);
static_assert(kDomDocumentSubscribers <= 8);

// Brings the document's followers of each entry up to date with its subscribers.
void updateFollowers(PrDomDocument* document) {
    prMemZero(document->followers, sizeof(document->followers));
    for (uint32_t i = 0; i < kDomDocumentSubscribers; ++i) {
        const DomDocumentHooks* hooks = document->subscribers[i].hooks;
        if (hooks == nullptr)
            continue;
        uint8_t bit = uint8_t(1u << i);
        if (hooks->inserted != nullptr)
            document->followers[DomHook_Inserted] |= bit;
        if (hooks->removing != nullptr)
            document->followers[DomHook_Removing] |= bit;
        if (hooks->attributeChanged != nullptr)
            document->followers[DomHook_AttributeChanged] |= bit;
        if (hooks->dataChanged != nullptr)
            document->followers[DomHook_DataChanged] |= bit;
        if (hooks->slotChanged != nullptr)
            document->followers[DomHook_SlotChanged] |= bit;
        if (hooks->shadowAttached != nullptr)
            document->followers[DomHook_ShadowAttached] |= bit;
        if (hooks->shadowRootAdopted != nullptr)
            document->followers[DomHook_ShadowRootAdopted] |= bit;
        if (hooks->wheelListenersChanged != nullptr)
            document->followers[DomHook_WheelListenersChanged] |= bit;
        if (hooks->statesChanged != nullptr)
            document->followers[DomHook_StatesChanged] |= bit;
        if (hooks->languageChanged != nullptr)
            document->followers[DomHook_LanguageChanged] |= bit;
        if (hooks->metaChanged != nullptr)
            document->followers[DomHook_MetaChanged] |= bit;
        if (hooks->resourceLoaded != nullptr)
            document->followers[DomHook_ResourceLoaded] |= bit;
        if (hooks->colorValue != nullptr)
            document->followers[DomHook_ColorValue] |= bit;
        if (hooks->styleBlockUpdate != nullptr)
            document->followers[DomHook_StyleBlockUpdate] |= bit;
        if (hooks->linkUpdate != nullptr)
            document->followers[DomHook_LinkUpdate] |= bit;
        if (hooks->instructionInserted != nullptr)
            document->followers[DomHook_InstructionInserted] |= bit;
        if (hooks->ownerAttributeChanged != nullptr)
            document->followers[DomHook_OwnerAttributeChanged] |= bit;
        if (hooks->instructionRemoving != nullptr)
            document->followers[DomHook_InstructionRemoving] |= bit;
        if (hooks->documentCreated != nullptr)
            document->followers[DomHook_DocumentCreated] |= bit;
        if (hooks->elementDestroyed != nullptr)
            document->followers[DomHook_ElementDestroyed] |= bit;
        if (hooks->shadowRootDestroyed != nullptr)
            document->followers[DomHook_ShadowRootDestroyed] |= bit;
        if (hooks->documentDestroyed != nullptr)
            document->followers[DomHook_DocumentDestroyed] |= bit;
    }
}

} // namespace

bool domDocumentSubscribe(PrDomDocument* document, const DomDocumentHooks* hooks, void* PR_NULLABLE context) {
    for (DomDocumentSubscriber& subscriber : document->subscribers) {
        if (subscriber.hooks == hooks)
            return false;
        if (subscriber.hooks == nullptr) {
            subscriber = DomDocumentSubscriber{.hooks = hooks, .context = context};
            updateFollowers(document);
            return true;
        }
    }
    PR_ASSERT(false);
    return false;
}

void domDocumentUnsubscribe(PrDomDocument* document, const DomDocumentHooks* hooks) {
    uint32_t kept = 0;
    for (const DomDocumentSubscriber& subscriber : document->subscribers) {
        if (subscriber.hooks != hooks)
            document->subscribers[kept++] = subscriber;
    }
    while (kept < kDomDocumentSubscribers)
        document->subscribers[kept++] = DomDocumentSubscriber{};
    updateFollowers(document);
}

const DomDocumentSubscriber* PR_NULLABLE domDocumentSubscriber(const PrDomDocument* document, const DomDocumentHooks* hooks) {
    for (const DomDocumentSubscriber& subscriber : document->subscribers) {
        if (subscriber.hooks == hooks)
            return &subscriber;
    }
    return nullptr;
}

} // namespace pr_impl
