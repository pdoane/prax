#include "pr/string_ops.h"

#include "dom/src/dom_mutation.h"
#include "dom/src/dom_node.h"

#include "doctest/doctest.h"

#include <vector>

namespace pr_impl {

namespace {

// A document whose host runs microtasks when the test says, in the way that a browsing context's host would.
struct Microtasks {
    std::vector<std::pair<void (*)(void*), void*>> queued;
    DomDocumentHost host = {
        .context = this,
        .fetch = [](void*, const DomFetchRequest*, DomFetchFn done, void* data, void (*)(void*)) {
            DomFetchResult result{.status = PrStatus_NotFound};
            done(data, &result);
        },
        .sheetsChanged = [](void*) {},
        .queueTask = [](void*, void (*)(void*), void*, void (*)(void*)) {},
        .queueDelayingTask = [](void*, void (*)(void*), void*, void (*)(void*)) {},
        .queueMicrotask = [](void* context, void (*run)(void*), void* data) {
            static_cast<Microtasks*>(context)->queued.push_back({run, data});
            return true;
        },
        .compile = [](void*, PrDomEventTarget*, PrDomElement*, PrDomEventHandler, PrStringView,
            PrDomEventHandlerCallback*) { return false; },
        .embedChanged = [](void*, PrDomElement*, PrStringView) {},
    };

    void run() {
        while (!queued.empty()) {
            std::pair<void (*)(void*), void*> next = queued.front();
            queued.erase(queued.begin());
            next.first(next.second);
        }
    }
};

struct Delivered {
    int calls = 0;
    uint32_t records = 0;
};

PrDomMutationCallback callbackFor(Delivered* delivered) {
    return PrDomMutationCallback{
        [](void* context, PrDomMutationObserver* observer) {
            Delivered* seen = static_cast<Delivered*>(context);
            ++seen->calls;
            seen->records += prDomMutationObserverRecordCount(observer);
            PrDomMutationRecord* record = prDomMutationRecordCreate();
            while (prDomMutationObserverTakeRecord(observer, record)) {
            }
            prDomMutationRecordDestroy(record);
        },
        delivered,
        nullptr,
        nullptr,
        nullptr,
    };
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a mutation observer is told of the changes it observes in one microtask") {
    Microtasks microtasks;
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    document->host = &microtasks.host;
    PrDomElement* parent = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, PrStringView{});
    domNodeRetain(parent);
    Delivered delivered;
    PrDomMutationCallback callback = callbackFor(&delivered);
    PrDomMutationObserver* observer = domMutationObserverCreate(&callback);
    PrDomMutationObserverInit nothing = prDomMutationObserverInitDefault();
    CHECK(domMutationObserverObserve(observer, parent, &nothing, nullptr, 0) == PrStatus_InvalidData);
    PrDomMutationObserverInit init = prDomMutationObserverInitDefault();
    init.childList = true;
    init.attributeOldValue = true;
    REQUIRE(domMutationObserverObserve(observer, parent, &init, nullptr, 0) == PrStatus_Ok);

    domSetAttribute(parent, PrDomNamespace_None, "id", "a");
    domSetAttribute(parent, PrDomNamespace_None, "id", "b");
    REQUIRE(observer->records.count == 2);
    CHECK(prRawStringIsNull(&observer->records.data[0]->oldValue));
    CHECK(prRawStringView(&observer->records.data[1]->oldValue) == "a");
    CHECK(microtasks.queued.size() == 1);

    // Replacing every child produces one record of the nodes that it removed and added.
    domInsert(parent, domTextCreate(document, "x"), nullptr);
    domInsert(parent, domTextCreate(document, "y"), nullptr);
    domReplaceAll(parent, domTextCreate(document, "z"));
    REQUIRE(observer->records.count == 5);
    PrDomMutationRecord* all = observer->records.data[4];
    CHECK(all->type == PrDomMutationType_ChildList);
    CHECK(all->removed.count == 2);
    CHECK(all->added.count == 1);

    microtasks.run();
    CHECK(delivered.calls == 1);
    CHECK(delivered.records == 5);

    domMutationObserverDisconnect(observer);
    domSetAttribute(parent, PrDomNamespace_None, "id", "c");
    CHECK(observer->records.count == 0);
    domMutationObserverRelease(observer);
    domNodeRelease(parent);
    domNodeRelease(document);
}
