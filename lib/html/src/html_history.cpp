// Session history (html_history.h).

#include "html/src/html_history.h"

#include "pr/alloc.h"
#include "pr/url.h"

#include "dom/src/dom_event.h"
#include "dom/src/dom_node.h"
#include "html/src/html_interface.h"
#include "html/src/html_page.h"
#include "html/src/html_window.h"

namespace pr_impl {

namespace {

void resetEntry(HtmlHistoryEntry* entry) {
    prStringReset(&entry->url);
    prStringReset(&entry->state);
}

// Drops the entries after the current one.
void dropForward(HtmlSessionHistory* history) {
    while (history->entries.count > history->index + 1) {
        resetEntry(&history->entries.last());
        history->entries.pop();
    }
}

// Returns true if the window's document is fully active (html/#fully-active): it is its browsing context's document,
// and each ancestor's browsing context still holds an iframe of the window.
bool fullyActive(const PrHtmlWindow* window) {
    for (const PrHtmlWindow* child = window; child->parent != nullptr; child = child->parent) {
        bool held = false;
        for (uint32_t i = 0; i < child->parent->frames.count; ++i)
            held = held || child->parent->frames.data[i].window == child;
        if (!held)
            return false;
    }
    return htmlDocumentPage(window->document) != nullptr;
}

// html/#can-have-its-url-rewritten
bool canRewrite(const PrUrl* document, const PrUrl* target) {
    if (prUrlScheme(document) != prUrlScheme(target) || prUrlUsername(document) != prUrlUsername(target)
        || prUrlPassword(document) != prUrlPassword(target) || prUrlHost(document) != prUrlHost(target)
        || prUrlPort(document) != prUrlPort(target))
        return false;
    PrStringView scheme = prUrlScheme(target);
    if (scheme == "http" || scheme == "https")
        return true;
    if (prUrlPath(document) != prUrlPath(target))
        return false;
    return scheme == "file" || prUrlQuery(document) == prUrlQuery(target);
}

// A hashchange that a task of the window fires.
struct HashChange {
    PrHtmlWindow* window;
    PrHtmlHashChangeEvent* event;
};

void fireHashChange(void* data) {
    HashChange* change = static_cast<HashChange*>(data);
    domEventDispatch(change->window, change->event);
    domEventRelease(change->event);
    prMemoryFree(change);
}

void dropHashChange(void* data) {
    HashChange* change = static_cast<HashChange*>(data);
    domEventRelease(change->event);
    prMemoryFree(change);
}

// A traversal by a delta, which a task of the window runs (html/#traverse-the-history-by-a-delta).
struct Traversal {
    PrHtmlWindow* window;
    int32_t delta;
};

void traverse(void* data) {
    Traversal* traversal = static_cast<Traversal*>(data);
    PrHtmlWindow* window = traversal->window;
    int32_t delta = traversal->delta;
    prMemoryFree(traversal);
    HtmlSessionHistory* history = window->history;
    int64_t target = int64_t(history->index) + delta;
    if (target < 0 || target >= int64_t(history->entries.count))
        return;
    HtmlHistoryEntry& entry = history->entries.data[target];
    // A reload, or an entry of another document, loads its document through the host.
    if (delta == 0 || entry.document != window->historyDocument) {
        htmlPageNavigate(window, prStringView(&entry.url), HtmlHistoryCommit_Traverse, uint32_t(target));
        return;
    }
    PrUrl* url = nullptr;
    if (prUrlCreate(prStringView(&entry.url), nullptr, PrCharset_Utf8, &url) != PrStatus_Ok)
        return;
    PrDomDocument* document = window->document;
    PrString old = prUrlSerialize(document->url, PrUrlSerialization_Whole);
    prUrlDestroy(document->url);
    document->url = url;
    history->index = uint32_t(target);
    ++window->historyGeneration;
    htmlHistoryNotify(window, prStringView(&old));
    prStringReset(&old);
}

// Queues a traversal of the window's session history by `delta`.
void queueTraversal(PrHtmlWindow* window, int32_t delta) {
    Traversal* traversal = allocZeroed<Traversal>();
    *traversal = Traversal{window, delta};
    htmlWindowQueueTask(window, traverse, traversal, [](void* data) { prMemoryFree(data); });
}

} // namespace

void htmlHistoryExpect(HtmlSessionHistory* history, HtmlHistoryCommit commit, uint32_t target) {
    history->commit = commit;
    history->target = target;
}

uint64_t htmlHistoryCommit(HtmlSessionHistory* history, PrStringView url, bool initial) {
    uint64_t document = ++history->documents;
    bool current = history->index < history->entries.count;
    HtmlHistoryCommit commit = initial ? HtmlHistoryCommit_Push : history->commit;
    if (commit != HtmlHistoryCommit_Push && history->target >= history->entries.count)
        commit = HtmlHistoryCommit_Push;
    history->commit = HtmlHistoryCommit_Push;
    if (commit == HtmlHistoryCommit_Push && current && history->entries.data[history->index].initial)
        commit = HtmlHistoryCommit_Replace;
    uint32_t target = commit == HtmlHistoryCommit_Push ? history->index : history->target;
    if (commit == HtmlHistoryCommit_Push) {
        if (current)
            dropForward(history);
        history->entries.push(HtmlHistoryEntry{prStringCopy(url), prStringNull(), document,
                                               PrHtmlScrollRestoration_Auto, initial});
        history->index = history->entries.count - 1;
        return document;
    }
    HtmlHistoryEntry& entry = history->entries.data[target];
    prStringReset(&entry.url);
    entry.url = prStringCopy(url);
    entry.document = document;
    entry.initial = false;
    // A replacement starts with no state, and a traversal keeps the entry's.
    if (commit == HtmlHistoryCommit_Replace) {
        prStringReset(&entry.state);
        entry.scrollRestoration = PrHtmlScrollRestoration_Auto;
    }
    history->index = target;
    return document;
}

void htmlHistoryReset(HtmlSessionHistory* history) {
    for (uint32_t i = 0; i < history->entries.count; ++i)
        resetEntry(&history->entries.data[i]);
    history->entries.reset();
    history->index = 0;
}

void htmlHistoryNotify(PrHtmlWindow* window, PrStringView oldUrl) {
    PrHtmlPopStateEvent* popState = prHtmlPopStateEventCreate("popstate", nullptr, false);
    popState->trusted = true;
    popState->history->historyState = true;
    domEventDispatch(window, popState);
    domEventRelease(popState);
    PrDomDocument* document = window->document;
    PrUrl* old = nullptr;
    if (prUrlCreate(oldUrl, nullptr, PrCharset_Utf8, &old) != PrStatus_Ok)
        return;
    PrStringView was = prUrlFragment(old);
    PrStringView now = prUrlFragment(document->url);
    bool changed = was != now || (was.data == nullptr) != (now.data == nullptr);
    prUrlDestroy(old);
    if (!changed)
        return;
    PrString newUrl = prUrlSerialize(document->url, PrUrlSerialization_Whole);
    PrHtmlHashChangeEvent* hashChange =
        prHtmlHashChangeEventCreate("hashchange", nullptr, oldUrl, prStringView(&newUrl));
    prStringReset(&newUrl);
    hashChange->trusted = true;
    HashChange* change = allocZeroed<HashChange>();
    *change = HashChange{window, hashChange};
    htmlWindowQueueTask(window, fireHashChange, change, dropHashChange);
}

void htmlHistoryUpdate(PrHtmlWindow* window, PrString state, PrHtmlHistoryHandling handling) {
    HtmlSessionHistory* history = window->history;
    PrString url = prUrlSerialize(window->document->url, PrUrlSerialization_Whole);
    bool current = history->index < history->entries.count
                   && history->entries.data[history->index].document == window->historyDocument;
    if (!current) {
        prStringReset(&url);
        prStringReset(&state);
        return;
    }
    HtmlHistoryEntry& active = history->entries.data[history->index];
    HtmlHistoryEntry entry{url, state, window->historyDocument, active.scrollRestoration, false};
    if (handling == PrHtmlHistoryHandling_Replace || active.initial) {
        resetEntry(&active);
        active = entry;
    } else {
        dropForward(history);
        history->entries.push(entry);
        ++history->index;
    }
    ++window->historyGeneration;
}

void htmlHistoryTraverse(PrHtmlWindow* window, int32_t delta) {
    queueTraversal(window, delta);
}

} // namespace pr_impl

using namespace pr_impl;

// ---------------------------------------------------------------------------------------------------------------------
// Session history

PrHtmlSessionHistory* prHtmlSessionHistoryCreate(void) {
    return allocZeroed<PrHtmlSessionHistory>();
}

void prHtmlSessionHistoryDestroy(PrHtmlSessionHistory* PR_NULLABLE self) {
    if (self == nullptr)
        return;
    htmlHistoryReset(self);
    prMemoryFree(self);
}

uint32_t prHtmlSessionHistoryLength(const PrHtmlSessionHistory* self) {
    return self->entries.count;
}

uint32_t prHtmlSessionHistoryIndex(const PrHtmlSessionHistory* self) {
    return self->index;
}

PrStringView prHtmlSessionHistoryUrl(const PrHtmlSessionHistory* self, uint32_t index) {
    return index < self->entries.count ? prStringView(&self->entries.data[index].url) : PrStringView{};
}

// ---------------------------------------------------------------------------------------------------------------------
// History

PrStatus prHtmlHistoryLength(const PrHtmlWindow* self, uint32_t* out) {
    *out = 0;
    if (!fullyActive(self))
        return PrStatus_NotAllowed;
    // A browsing context that has yet to load a document into a host's history still shows its initial about:blank.
    *out = self->history->entries.count != 0 ? self->history->entries.count : 1;
    return PrStatus_Ok;
}

PrStatus prHtmlHistoryState(const PrHtmlWindow* self, PrStringView* out, uint32_t* generation) {
    *out = PrStringView{};
    *generation = self->historyGeneration;
    if (!fullyActive(self))
        return PrStatus_NotAllowed;
    const HtmlSessionHistory* history = self->history;
    if (history->index < history->entries.count) {
        const HtmlHistoryEntry& entry = history->entries.data[history->index];
        if (entry.document == self->historyDocument && !prStringIsNull(&entry.state))
            *out = prStringView(&entry.state);
    }
    return PrStatus_Ok;
}

PrStatus prHtmlHistoryPushState(PrHtmlWindow* self, PrStringView serialized, PrStringView url,
                                PrHtmlHistoryHandling handling) {
    if (!fullyActive(self))
        return PrStatus_NotAllowed;
    PrDomDocument* document = self->document;
    PrUrl* newUrl = nullptr;
    if (url.size != 0) {
        PrUrl* base = domDocumentBaseUrl(document);
        PrStatus status = prUrlCreate(url, base, document->charset, &newUrl);
        prUrlDestroy(base);
        if (status != PrStatus_Ok)
            return PrStatus_Syntax;
        if (!canRewrite(document->url, newUrl)) {
            prUrlDestroy(newUrl);
            return PrStatus_NotAllowed;
        }
        prUrlDestroy(document->url);
        document->url = newUrl;
    }
    htmlHistoryUpdate(self, serialized.data != nullptr ? prStringCopy(serialized) : prStringNull(), handling);
    return PrStatus_Ok;
}

PrStatus prHtmlHistoryGo(PrHtmlWindow* self, int32_t delta) {
    if (!fullyActive(self))
        return PrStatus_NotAllowed;
    queueTraversal(self, delta);
    return PrStatus_Ok;
}

PrStatus prHtmlHistoryScrollRestoration(const PrHtmlWindow* self, PrHtmlScrollRestoration* out) {
    *out = PrHtmlScrollRestoration_Auto;
    if (!fullyActive(self))
        return PrStatus_NotAllowed;
    const HtmlSessionHistory* history = self->history;
    if (history->index < history->entries.count)
        *out = history->entries.data[history->index].scrollRestoration;
    return PrStatus_Ok;
}

PrStatus prHtmlHistorySetScrollRestoration(PrHtmlWindow* self, PrHtmlScrollRestoration mode) {
    if (!fullyActive(self))
        return PrStatus_NotAllowed;
    HtmlSessionHistory* history = self->history;
    if (history->index < history->entries.count)
        history->entries.data[history->index].scrollRestoration = mode;
    return PrStatus_Ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// PopStateEvent and HashChangeEvent

PrHtmlPopStateEvent* PR_NULLABLE prHtmlPopStateEventQuery(PrDomEvent* PR_NULLABLE object) {
    return object != nullptr && object->kind == PrDomEventKind_PopState ? static_cast<PrHtmlPopStateEvent*>(object)
                                                                        : nullptr;
}

PrHtmlPopStateEvent* prHtmlPopStateEventCreate(PrStringView type, const PrDomEventInit* PR_NULLABLE init,
                                               bool hasUAVisualTransition) {
    PrHtmlPopStateEvent* event = domEventCreate<PrHtmlPopStateEvent>(
        type, init != nullptr ? *init : prDomEventInitDefault(), PrDomEventKind_PopState);
    event->history = allocZeroed<DomHistoryEventData>();
    *event->history = DomHistoryEventData{prStringNull(), prStringNull(), false, hasUAVisualTransition};
    return event;
}

bool prHtmlPopStateEventCarriesHistoryState(const PrHtmlPopStateEvent* self) {
    return self->history->historyState;
}

bool prHtmlPopStateEventHasUAVisualTransition(const PrHtmlPopStateEvent* self) {
    return self->history->hasUAVisualTransition;
}

PrHtmlHashChangeEvent* PR_NULLABLE prHtmlHashChangeEventQuery(PrDomEvent* PR_NULLABLE object) {
    return object != nullptr && object->kind == PrDomEventKind_HashChange
               ? static_cast<PrHtmlHashChangeEvent*>(object)
               : nullptr;
}

PrHtmlHashChangeEvent* prHtmlHashChangeEventCreate(PrStringView type, const PrDomEventInit* PR_NULLABLE init,
                                                   PrStringView oldUrl, PrStringView newUrl) {
    PrHtmlHashChangeEvent* event = domEventCreate<PrHtmlHashChangeEvent>(
        type, init != nullptr ? *init : prDomEventInitDefault(), PrDomEventKind_HashChange);
    event->history = allocZeroed<DomHistoryEventData>();
    *event->history = DomHistoryEventData{prStringCopy(oldUrl), prStringCopy(newUrl), false, false};
    return event;
}

PrStringView prHtmlHashChangeEventOldUrl(const PrHtmlHashChangeEvent* self) {
    return prStringView(&self->history->oldUrl);
}

PrStringView prHtmlHashChangeEventNewUrl(const PrHtmlHashChangeEvent* self) {
    return prStringView(&self->history->newUrl);
}
