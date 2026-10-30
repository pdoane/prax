#include "dom/src/dom_custom.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/string_array.h"
#include "pr/string_ops.h"
#include "pr/utf8.h"

#include "dom/src/dom_attribute.h"
#include "dom/src/dom_hooks.h"

namespace pr_impl {

namespace {

// Returns a string as a view, or the null view for the null string.
PrStringView viewOf(const PrString* text) {
    return prStringIsNull(text) ? PrStringView{} : prStringView(text);
}

void resetReaction(DomCustomReactionEntry* reaction) {
    prStringReset(&reaction->name);
    prStringReset(&reaction->oldValue);
    prStringReset(&reaction->value);
    prStringReset(&reaction->space);
    domDocumentRelease(reaction->oldDocument);
    domDocumentRelease(reaction->newDocument);
}

// Drops the element's reactions without running them.
void clearReactions(DomCustomElement* custom) {
    for (uint32_t i = 0; i < custom->reactions.count; ++i)
        resetReaction(&custom->reactions.data[i]);
    custom->reactions.clear();
}

void upgrade(PrDomElement* element, void* definition, const PrDomCustomElementRegistry* registry);

// Runs the element's reactions in order, until none are left or one destroys the element's custom element state.
void runReactions(PrDomElement* element) {
    if (element->state == nullptr || element->state->custom == nullptr)
        return;
    DomCustomElement* custom = element->state->custom;
    while (custom->reactions.count != 0) {
        DomCustomReactionEntry reaction = custom->reactions.data[0];
        custom->reactions.erase(0);
        if (reaction.upgrade != nullptr) {
            const PrDomCustomElementRegistry* registry =
                custom->registry != nullptr ? custom->registry : element->document->customElements;
            upgrade(element, reaction.upgrade, registry);
        } else if (custom->definition != nullptr && custom->registry != nullptr) {
            PrDomCustomReaction arguments = {
                reaction.callback, viewOf(&reaction.name),
                viewOf(&reaction.oldValue), viewOf(&reaction.value),
                viewOf(&reaction.space),
                reaction.oldDocument,
                reaction.newDocument,
            };
            custom->registry->invoke(custom->registry->context, element, custom->definition, &arguments);
        }
        resetReaction(&reaction);
        // A reaction may have destroyed the element's custom element state.
        if (element->state == nullptr || element->state->custom != custom)
            break;
    }
}

// html/#invoke-custom-element-reactions: runs each element's reactions in order. The queue may grow while they run.
// Releases each element once its reactions are done.
void invokeReactions(Array<PrDomElement* PR_NONNULL>* queue) {
    for (uint32_t i = 0; i < queue->count; ++i) {
        PrDomElement* element = queue->data[i];
        runReactions(element);
        domNodeUnhold(element);
    }
}

void processBackup(void* data) {
    PrDomAgent* agent = static_cast<PrDomAgent*>(data);
    invokeReactions(&agent->backup);
    agent->backup.clear();
    agent->processingBackup = false;
}

// html/#enqueue-an-element-on-the-appropriate-element-queue, of the registry's agent.
void enqueueElement(PrDomElement* element, const PrDomCustomElementRegistry* registry) {
    PrDomAgent* agent = registry->agent;
    domNodeHold(element);
    if (agent->queues.count != 0) {
        agent->queues.last().push(element);
        return;
    }
    agent->backup.push(element);
    if (agent->processingBackup)
        return;
    agent->processingBackup = true;
    registry->queueMicrotask(registry->context, processBackup, agent);
}

// html/#enqueue-a-custom-element-callback-reaction, for a custom element whose definition has the callback. For
// attributeChanged, enqueues the reaction only if the definition observes the attribute.
void enqueueCallback(PrDomElement* element, PrDomCustomCallback callback, PrStringView name = {},
        PrStringView oldValue = {}, PrStringView value = {}, PrStringView space = {},
        PrDomDocument* PR_NULLABLE oldDocument = nullptr, PrDomDocument* PR_NULLABLE newDocument = nullptr) {
    DomCustomElement* custom = domCustomElementFromElement(element);
    const PrDomCustomElementRegistry* registry = custom->registry;
    if (custom->definition == nullptr || registry == nullptr ||
            !registry->observes(registry->context, custom->definition, callback, name))
        return;
    if (oldDocument != nullptr)
        domDocumentRetain(oldDocument);
    if (newDocument != nullptr)
        domDocumentRetain(newDocument);
    custom->reactions.push(DomCustomReactionEntry{
        nullptr, callback, prStringCopy(name),
        prStringCopy(oldValue), prStringCopy(value),
        prStringCopy(space), oldDocument, newDocument,
    });
    enqueueElement(element, registry);
}

// html/#enqueue-a-custom-element-upgrade-reaction
void enqueueUpgrade(PrDomElement* element, void* definition, const PrDomCustomElementRegistry* registry) {
    DomCustomElement* custom = domCustomElementFromElement(element);
    DomCustomReactionEntry reaction = {};
    reaction.upgrade = definition;
    custom->reactions.push(reaction);
    if (custom->registry == nullptr)
        custom->registry = registry;
    enqueueElement(element, registry);
}

// html/#concept-upgrade-an-element: the element takes the definition, its attributes and its connection enqueue their
// callbacks, and the definition's constructor runs over the element.
void upgrade(PrDomElement* element, void* definition, const PrDomCustomElementRegistry* PR_NULLABLE registry) {
    if (registry == nullptr || (element->customState != PrDomCustomElementState_Undefined &&
            element->customState != PrDomCustomElementState_Uncustomized))
        return;
    DomCustomElement* custom = domCustomElementFromElement(element);
    custom->definition = definition;
    custom->registry = registry;
    element->customState = PrDomCustomElementState_Failed;
    for (PrDomAttr* attribute = element->firstAttribute; attribute != nullptr;
            attribute = static_cast<PrDomAttr*>(attribute->next)) {
        enqueueCallback(element, PrDomCustomCallback_AttributeChanged,
            prStringView(&attribute->name), {}, prStringView(&attribute->value),
            domNamespaceUrl(element->document, attribute->space));
    }
    if (domIsConnected(element))
        enqueueCallback(element, PrDomCustomCallback_Connected);
    element->customState = PrDomCustomElementState_Precustomized;
    if (!registry->upgrade(registry->context, element, definition)) {
        element->customState = PrDomCustomElementState_Failed;
        custom->definition = nullptr;
        clearReactions(custom);
        return;
    }
    element->customState = PrDomCustomElementState_Custom;
    // The element now matches :defined.
    domNotifyStatesChanged(element);
}

// Returns true if the code point is a PCENChar (html/#valid-custom-element-name).
bool isPcenChar(uint32_t c) {
    return c == '-' || c == '.' || (c >= '0' && c <= '9') || c == '_' || (c >= 'a' && c <= 'z') ||
        c == 0xb7 || (c >= 0xc0 && c <= 0xd6) || (c >= 0xd8 && c <= 0xf6) || (c >= 0xf8 && c <= 0x37d) ||
        (c >= 0x37f && c <= 0x1fff) ||
        (c >= 0x200c && c <= 0x200d) || (c >= 0x203f && c <= 0x2040) || (c >= 0x2070 && c <= 0x218f) ||
        (c >= 0x2c00 && c <= 0x2fef) ||
        (c >= 0x3001 && c <= 0xd7ff) || (c >= 0xf900 && c <= 0xfdcf) || (c >= 0xfdf0 && c <= 0xfffd) ||
        (c >= 0x10000 && c <= 0xeffff);
}

// Visits the shadow-including descendants of `root` that are elements, in shadow-including tree order, including root
// itself if `inclusive` is set.
template <typename Visit>
void eachShadowIncluding(PrDomNode* root, bool inclusive, Visit visit) {
    for (PrDomNode* node = root; node != nullptr; node = domNodeNext(root, node)) {
        PrDomElement* element = domElementFromNode(node);
        if (element == nullptr || (node == root && !inclusive))
            continue;
        visit(element);
        if (PrDomShadowRoot* shadow = domShadowRootOfHost(element))
            eachShadowIncluding(shadow, false, visit);
    }
}

} // namespace

PrDomElement* domElementCreateHtml(PrDomDocument* document, PrDomTag tag, PrStringView name,
        PrStringView is, bool synchronous, bool* held) {
    *held = false;
    const PrDomCustomElementRegistry* registry = document->customElements;
    void* definition = registry != nullptr ? registry->lookup(registry->context, name, is) : nullptr;
    if (definition != nullptr && synchronous) {
        // The element that the constructor creates is held from then on, which keeps its script object and the state
        // that the constructor gave it. If this function returns the element, it stays held for the caller.
        PrDomAgent* agent = registry->agent;
        uint32_t mark = agent->constructed.count;
        ++agent->constructing;
        PrDomElement* made = registry->construct(registry->context, definition);
        --agent->constructing;
        if (made != nullptr)
            domNodeHold(made);
        while (agent->constructed.count > mark)
            domNodeUnhold(agent->constructed.data[--agent->constructed.count]);
        if (made != nullptr) {
            // The element must be a new HTML element in the document with the definition's name. No script may have
            // already placed it anywhere or given it attributes or children.
            domNodeRelease(made);
            if (made->space != PrDomNamespace_Html) {
                registry->report(registry->context, "TypeError");
            } else if (made->firstAttribute != nullptr || made->first != nullptr ||
                    made->parent != nullptr || made->document != document || domElementName(made) != name) {
                registry->report(registry->context, "NotSupportedError");
            } else {
                *held = true;
                return made;
            }
            domNodeUnhold(made);
        }
        // An element whose constructor threw becomes a failed HTMLUnknownElement.
        PrDomElement* failed = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Unknown, name);
        failed->customState = PrDomCustomElementState_Failed;
        return failed;
    }
    PrDomElement* element = domElementCreate(document, PrDomNamespace_Html, tag, name);
    if (is.data != nullptr)
        domCustomElementFromElement(element)->is = prStringCopy(is);
    // No element that a spec defines has a valid custom element name.
    if (definition != nullptr || is.data != nullptr || (tag == PrDomTag_Unknown && domIsValidCustomElementName(name)))
        element->customState = PrDomCustomElementState_Undefined;
    if (definition != nullptr)
        enqueueUpgrade(element, definition, registry);
    return element;
}

bool domIsValidCustomElementName(PrStringView name) {
    constexpr PrStringView kReserved[] = {
        "annotation-xml", "color-profile", "font-face",
        "font-face-src",  "font-face-uri", "font-face-format",
        "font-face-name", "missing-glyph",
    };
    if (name.size == 0 || !(name.data[0] >= 'a' && name.data[0] <= 'z') || !stringContains(name, "-"))
        return false;
    for (PrStringView reserved : kReserved) {
        if (name == reserved)
            return false;
    }
    for (size_t at = 0; at < name.size;) {
        uint32_t c = 0;
        uint32_t length = prUtf8Decode(name, at, &c);
        if (length == 0 || !isPcenChar(c))
            return false;
        at += length;
    }
    return true;
}

void domCustomConnectionChanged(PrDomElement* element) {
    if (!domIsConnected(element)) {
        if (element->customState == PrDomCustomElementState_Custom)
            enqueueCallback(element, PrDomCustomCallback_Disconnected);
    } else if (element->customState == PrDomCustomElementState_Custom) {
        enqueueCallback(element, PrDomCustomCallback_Connected);
    } else {
        domCustomTryUpgrade(element);
    }
}

void domCustomAdopted(PrDomElement* element, PrDomDocument* from) {
    if (element->customState == PrDomCustomElementState_Custom)
        enqueueCallback(element, PrDomCustomCallback_Adopted, {}, {}, {}, {}, from, element->document);
}

void domCustomAttributeChanged(PrDomElement* element, DomNamespaceId space, PrStringView name,
        PrStringView oldValue, PrStringView value) {
    if (element->customState == PrDomCustomElementState_Custom)
        enqueueCallback(element, PrDomCustomCallback_AttributeChanged, name, oldValue, value,
            domNamespaceUrl(element->document, space));
}

void domCustomTryUpgrade(PrDomElement* element) {
    const PrDomCustomElementRegistry* registry = element->document->customElements;
    if (registry == nullptr || element->space != PrDomNamespace_Html ||
            element->customState != PrDomCustomElementState_Undefined)
        return;
    const DomCustomElement* custom = element->state != nullptr ? element->state->custom : nullptr;
    PrStringView is = custom != nullptr ? viewOf(&custom->is) : PrStringView{};
    void* definition = registry->lookup(registry->context, domElementName(element), is);
    if (definition != nullptr)
        enqueueUpgrade(element, definition, registry);
}

void domCustomUpgradeCandidates(PrDomDocument* document, PrStringView name, void* definition) {
    const PrDomCustomElementRegistry* registry = document->customElements;
    if (registry == nullptr)
        return;
    eachShadowIncluding(document, false, [&](PrDomElement* element) {
        if (element->space == PrDomNamespace_Html && domElementName(element) == name &&
                element->customState == PrDomCustomElementState_Undefined)
            enqueueUpgrade(element, definition, registry);
    });
}

void domCustomUpgradeTree(PrDomNode* root) {
    eachShadowIncluding(root, true, [](PrDomElement* element) { domCustomTryUpgrade(element); });
}

DomCustomElement* domCustomElementFromElement(PrDomElement* element) {
    DomElementState* state = domElementState(element);
    if (state->custom == nullptr) {
        state->custom = allocZeroed<DomCustomElement>();
        state->custom->is = prStringNull();
    }
    return state->custom;
}

bool domCustomHasState(const PrDomElement* element, PrStringView state) {
    const DomCustomElement* custom = element->state != nullptr ? element->state->custom : nullptr;
    for (uint32_t i = 0; custom != nullptr && i < custom->states.count; ++i) {
        if (prStringView(&custom->states.data[i]) == state)
            return true;
    }
    return false;
}

void domCustomDestroy(DomCustomElement* PR_NULLABLE custom) {
    if (custom == nullptr)
        return;
    clearReactions(custom);
    custom->reactions.reset();
    stringsReset(&custom->states);
    prStringReset(&custom->is);
    prMemoryFree(custom);
}

void domCustomConstructed(PrDomAgent* agent, PrDomElement* element) {
    if (agent->constructing == 0)
        return;
    domNodeHold(element);
    agent->constructed.push(element);
}

void domCustomReactionsPush(PrDomAgent* agent) {
    agent->queues.push(Array<PrDomElement* PR_NONNULL>{});
}

void domCustomReactionsPop(PrDomAgent* agent) {
    Array<PrDomElement* PR_NONNULL> queue = agent->queues.data[--agent->queues.count];
    if (queue.data == nullptr)
        return;
    invokeReactions(&queue);
    queue.reset();
}

} // namespace pr_impl
