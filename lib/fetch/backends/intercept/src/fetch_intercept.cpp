// The fetcher whose host answers its requests (fetch_intercept.h).

#include "pr/fetch_intercept.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/fetch_backend.h"
#include "pr/fetch_blob.h"
#include "pr/mem.h"
#include "pr/raw_array.h"
#include "pr/time.h"
#include "pr/url.h"

namespace pr_impl {

namespace {

struct InterceptFetcher : PrFetchNetwork {
    PrNet* net;                    // the network whose loop runs the fetches' callbacks
    PrFetchBlobStore* blobStore;   // the blob URLs that the fetcher answers
    PrFetchInterceptRequestFn request;
    void* PR_NULLABLE context;
    PrFetchNetwork* PR_NULLABLE next;
    PrRawString userAgent;         // the default User-Agent value
};

enum {
    InterceptState_Starting,    // waiting for the task that asks the host
    InterceptState_Asked,       // waiting for the host's answer
    InterceptState_Body,        // answered with a response
    InterceptState_Failed,      // answered with a network error
    InterceptState_Passed,      // passed on to the next fetcher, whose fetch answers it
};
typedef uint8_t InterceptState;

// Header names and values, which a fetch owns, and views of them.
struct InterceptHeaders {
    RawArray<PrRawString> names;
    RawArray<PrRawString> values;
    RawArray<PrFetchHeader> views;
};

void headersCopy(InterceptHeaders* list, const PrFetchHeader* PR_NULLABLE headers, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        list->names.push(prRawStringCopy(headers[i].name));
        list->values.push(prRawStringCopy(headers[i].value));
    }
    for (uint32_t i = 0; i < count; ++i)
        list->views.push(PrFetchHeader{prRawStringView(&list->names.data[i]), prRawStringView(&list->values.data[i])});
}

void headersReset(InterceptHeaders* list) {
    for (uint32_t i = 0; i < list->names.count; ++i) {
        prRawStringReset(&list->names.data[i]);
        prRawStringReset(&list->values.data[i]);
    }
    list->names.reset();
    list->values.reset();
    list->views.reset();
}

PrRawString copyOrNull(PrStringView view) {
    return view.data != nullptr ? prRawStringCopy(view) : prRawStringNull();
}

// A fetch, which is also the request that the host answers. The memory outlives the fetch's destruction while `holds`
// is not zero: while the task that asks the host waits, while the host has not answered, and while a callback runs.
struct InterceptFetch : PrFetch, PrFetchInterceptRequest {
    InterceptFetcher* fetcher;         // the fetcher that made the fetch
    uint32_t holds;
    bool dead;
    PrFetchFn PR_NULLABLE event;
    void* PR_NULLABLE context;
    InterceptState state;

    // The copies of the request that the views of its description are of.
    PrRawString url;
    PrRawString method;
    PrRawString origin;
    PrRawString referrer;
    InterceptHeaders requestHeaders;
    RawArray<uint8_t> requestBody;

    // The host's response, and its body from `contentStart`, or the next fetcher's fetch that answers instead.
    PrRawString responseUrl;       // the null string for the request's URL
    uint16_t status;
    PrFetchResponseType type;
    InterceptHeaders response;
    RawArray<uint8_t> content;
    uint32_t contentStart;
    PrNetError error;
    PrFetch* PR_NULLABLE passed;
    PrFetchTiming timing;       // the measurements of a request that the host answers

    // The blob that a blob URL named when the fetch was created, or null. The fetcher answers such a URL itself.
    const PrFetchBlob* PR_NULLABLE blob;
};

InterceptFetcher* fetcherOf(InterceptFetch* fetch) {
    return static_cast<InterceptFetcher*>(fetch->fetcher);
}

void freeFetch(InterceptFetch* fetch) {
    prFetchBlobRelease(fetch->blob);
    prRawStringReset(&fetch->url);
    prRawStringReset(&fetch->responseUrl);
    prRawStringReset(&fetch->method);
    prRawStringReset(&fetch->origin);
    prRawStringReset(&fetch->referrer);
    headersReset(&fetch->requestHeaders);
    fetch->requestBody.reset();
    headersReset(&fetch->response);
    fetch->content.reset();
    prMemoryFree(fetch);
}

void release(InterceptFetch* fetch) {
    PR_ASSERT(fetch->holds != 0);
    if (--fetch->holds == 0 && fetch->dead)
        freeFetch(fetch);
}

void deliver(InterceptFetch* fetch, PrFetchEvent event) {
    if (!fetch->dead && fetch->event != nullptr)
        fetch->event(fetch->context, fetch, event);
}

// Answers a request for a blob URL from the fetcher's own store.
void answerBlob(InterceptFetch* fetch) {
    const PrFetchBlob* blob = fetch->blob;
    if (blob == nullptr || (fetch->desc.method.data != nullptr && fetch->desc.method != "GET")) {
        prFetchInterceptRequestFail(fetch, PrNetError_Protocol);
        return;
    }
    PrFetchHeader header{"Content-Type", prFetchBlobType(blob)};
    PrFetchInterceptResponse response{
        .status = 200,
        .headers = &header,
        .headerCount = 1,
        .body = prFetchBlobBytes(blob),
    };
    prFetchInterceptRequestRespond(fetch, &response);
}

// Asks the host for the response, from a task, so that the fetch's events come after the call that made it.
void ask(void* PR_NULLABLE context) {
    InterceptFetch* fetch = static_cast<InterceptFetch*>(context);
    if (!fetch->dead) {
        InterceptFetcher* fetcher = fetcherOf(fetch);
        fetch->state = InterceptState_Asked;
        // The host's hold, which its answer releases.
        ++fetch->holds;
        // A blob URL names what the fetcher holds, so no host answers for it.
        if (prFetchBlobIsUrl(fetch->desc.url))
            answerBlob(fetch);
        else
            fetcher->request(fetcher->context, fetch);
    }
    release(fetch);
}

// Passes the next fetcher's events on to the client.
void passedEvent(void* PR_NULLABLE context, PrFetch*, PrFetchEvent event) {
    InterceptFetch* fetch = static_cast<InterceptFetch*>(context);
    ++fetch->holds;
    deliver(fetch, event);
    release(fetch);
}

// Fetcher functions

PrNet* fetcherNet(const PrFetchNetwork* base) {
    return static_cast<const InterceptFetcher*>(base)->net;
}

PrFetchBlobStore* fetcherBlobStore(const PrFetchNetwork* base) {
    return static_cast<const InterceptFetcher*>(base)->blobStore;
}

PrStringView fetcherUserAgent(const PrFetchNetwork* base) {
    return prRawStringView(&static_cast<const InterceptFetcher*>(base)->userAgent);
}

void destroyFetcher(PrFetchNetwork* base) {
    InterceptFetcher* fetcher = static_cast<InterceptFetcher*>(base);
    prRawStringReset(&fetcher->userAgent);
    if (fetcher->next == nullptr)
        prFetchBlobStoreDestroy(fetcher->blobStore);
    prMemoryFree(fetcher);
}

void destroyFetch(PrFetch* handle) {
    InterceptFetch* fetch = static_cast<InterceptFetch*>(handle);
    PR_ASSERT(!fetch->dead);
    fetch->dead = true;
    prFetchDestroy(fetch->passed);
    fetch->passed = nullptr;
    if (fetch->holds == 0)
        freeFetch(fetch);
}

bool hasResponse(const InterceptFetch* fetch) {
    return fetch->state == InterceptState_Body;
}

uint16_t fetchStatus(const PrFetch* handle) {
    const InterceptFetch* fetch = static_cast<const InterceptFetch*>(handle);
    if (fetch->passed != nullptr)
        return prFetchStatus(fetch->passed);
    return hasResponse(fetch) ? fetch->status : 0;
}

PrStringView fetchUrl(const PrFetch* handle) {
    const InterceptFetch* fetch = static_cast<const InterceptFetch*>(handle);
    if (fetch->passed != nullptr)
        return prFetchUrl(fetch->passed);
    return prRawStringView(prRawStringIsNull(&fetch->responseUrl) ? &fetch->url : &fetch->responseUrl);
}

PrFetchResponseType fetchResponseType(const PrFetch* handle) {
    const InterceptFetch* fetch = static_cast<const InterceptFetch*>(handle);
    if (fetch->passed != nullptr)
        return prFetchResponseType(fetch->passed);
    return hasResponse(fetch) ? fetch->type : PrFetchResponseType_Basic;
}

uint32_t fetchHeaderCount(const PrFetch* handle) {
    const InterceptFetch* fetch = static_cast<const InterceptFetch*>(handle);
    if (fetch->passed != nullptr)
        return prFetchHeaderCount(fetch->passed);
    return hasResponse(fetch) ? fetch->response.views.count : 0;
}

PrFetchHeader fetchHeader(const PrFetch* handle, uint32_t index) {
    const InterceptFetch* fetch = static_cast<const InterceptFetch*>(handle);
    if (fetch->passed != nullptr)
        return prFetchHeader(fetch->passed, index);
    return fetch->response.views.data[index];
}

size_t fetchRead(PrFetch* handle, uint8_t* output, size_t capacity) {
    InterceptFetch* fetch = static_cast<InterceptFetch*>(handle);
    if (fetch->passed != nullptr)
        return prFetchRead(fetch->passed, output, capacity);
    uint32_t available = fetch->content.count - fetch->contentStart;
    uint32_t taken = capacity < available ? uint32_t(capacity) : available;
    prMemCopy(output, fetch->content.data + fetch->contentStart, taken);
    fetch->contentStart += taken;
    return taken;
}

PrStringView fetchSentReferrer(const PrFetch* handle) {
    const InterceptFetch* fetch = static_cast<const InterceptFetch*>(handle);
    return fetch->passed != nullptr ? prFetchSentReferrer(fetch->passed) : fetch->desc.referrer;
}

PrFetchTiming fetchTiming(const PrFetch* handle) {
    const InterceptFetch* fetch = static_cast<const InterceptFetch*>(handle);
    return fetch->passed != nullptr ? prFetchTiming(fetch->passed) : fetch->timing;
}

PrNetError fetchError(const PrFetch* handle) {
    const InterceptFetch* fetch = static_cast<const InterceptFetch*>(handle);
    if (fetch->passed != nullptr)
        return prFetchError(fetch->passed);
    return fetch->error;
}

PrStatus createFetch(PrFetchNetwork* base, const PrFetchRequestDesc* desc, PrFetchFn PR_NULLABLE event,
        void* PR_NULLABLE context, PrFetch* PR_NULLABLE* PR_NONNULL out) {
    PrUrl* url = nullptr;
    if (prUrlCreate(desc->url, nullptr, PrCharset_Utf8, &url) != PrStatus_Ok)
        return PrStatus_InvalidData;
    PrUrl* referrer = nullptr;
    if (desc->referrer.data != nullptr &&
            prUrlCreate(desc->referrer, nullptr, PrCharset_Utf8, &referrer) != PrStatus_Ok) {
        prUrlDestroy(url);
        return PrStatus_InvalidData;
    }
    prUrlDestroy(referrer);
    InterceptFetch* fetch = allocZeroed<InterceptFetch>();
    fetch->fetcher = static_cast<InterceptFetcher*>(base);
    fetch->destroyFn = destroyFetch;
    fetch->statusFn = fetchStatus;
    fetch->urlFn = fetchUrl;
    fetch->responseTypeFn = fetchResponseType;
    fetch->headerCountFn = fetchHeaderCount;
    fetch->headerFn = fetchHeader;
    fetch->readFn = fetchRead;
    fetch->errorFn = fetchError;
    fetch->timingFn = fetchTiming;
    fetch->sentReferrerFn = fetchSentReferrer;
    fetch->event = event;
    fetch->context = context;
    fetch->url = prUrlSerialize(url, PrUrlSerialization_ExcludeFragment);
    prUrlDestroy(url);
    fetch->method = copyOrNull(desc->method);
    fetch->origin = copyOrNull(desc->origin);
    fetch->referrer = copyOrNull(desc->referrer);
    headersCopy(&fetch->requestHeaders, desc->headers, desc->headerCount);
    if (desc->body.size != 0)
        fetch->requestBody.append(desc->body.data, uint32_t(desc->body.size));
    fetch->desc = *desc;
    fetch->desc.url = prRawStringView(&fetch->url);
    fetch->desc.method = prRawStringView(&fetch->method);
    fetch->desc.origin = prRawStringView(&fetch->origin);
    fetch->desc.referrer = prRawStringView(&fetch->referrer);
    fetch->desc.headers = fetch->requestHeaders.views.data;
    fetch->desc.body = PrByteView{fetch->requestBody.data, fetch->requestBody.count};
    fetch->timing.startTime = prTimeMonotonicMilliseconds();
    fetch->timing.postRedirectStart = fetch->timing.startTime;
    if (prFetchBlobIsUrl(fetch->desc.url))
        fetch->blob = prFetchBlobStoreResolve(fetch->fetcher->blobStore, fetch->desc.url);
    // The hold of the task that asks the host.
    fetch->holds = 1;
    prNetPost(fetch->fetcher->net, ask, fetch);
    *out = fetch;
    return PrStatus_Ok;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrFetchNetwork* prFetchInterceptCreate(const PrFetchInterceptDesc* desc) {
    PR_ASSERT(desc->request != nullptr);
    InterceptFetcher* fetcher = allocZeroed<InterceptFetcher>();
    fetcher->destroyFn = destroyFetcher;
    fetcher->fetchCreateFn = createFetch;
    fetcher->netFn = fetcherNet;
    fetcher->blobStoreFn = fetcherBlobStore;
    fetcher->userAgentFn = fetcherUserAgent;
    fetcher->net = desc->net;
    fetcher->blobStore = desc->next != nullptr ? desc->next->blobStoreFn(desc->next) : prFetchBlobStoreCreate();
    fetcher->request = desc->request;
    fetcher->context = desc->context;
    fetcher->next = desc->next;
    PrStringView userAgent = desc->userAgent;
    if (userAgent.data == nullptr && desc->next != nullptr)
        userAgent = prFetchNetworkUserAgent(desc->next);
    else if (userAgent.data == nullptr)
        userAgent = kPrFetchDefaultUserAgent;
    fetcher->userAgent = prRawStringCopy(userAgent);
    PR_ASSERT(desc->next == nullptr || prFetchNetworkNet(desc->next) == desc->net);
    return fetcher;
}

void prFetchInterceptRequestRespond(PrFetchInterceptRequest* request, const PrFetchInterceptResponse* response) {
    InterceptFetch* fetch = static_cast<InterceptFetch*>(request);
    PR_ASSERT(fetch->state == InterceptState_Asked);
    PR_ASSERT(response->headers != nullptr || response->headerCount == 0);
    PR_ASSERT(response->body.data != nullptr || response->body.size == 0);
    fetch->state = InterceptState_Body;
    fetch->status = response->status;
    fetch->type = response->type;
    if (response->url.data != nullptr)
        fetch->responseUrl = prRawStringCopy(response->url);
    headersCopy(&fetch->response, response->headers, response->headerCount);
    if (response->body.size != 0)
        fetch->content.append(response->body.data, uint32_t(response->body.size));
    fetch->timing.requestStart = prTimeMonotonicMilliseconds();
    fetch->timing.responseStart = fetch->timing.requestStart;
    fetch->timing.encodedBodySize = response->body.size;
    fetch->timing.decodedBodySize = response->body.size;
    fetch->timing.timingAllowPassed = response->type == PrFetchResponseType_Basic;
    deliver(fetch, PrFetchEvent_Response);
    if (fetch->content.count != 0)
        deliver(fetch, PrFetchEvent_Readable);
    fetch->timing.endTime = prTimeMonotonicMilliseconds();
    deliver(fetch, PrFetchEvent_End);
    release(fetch);
}

void prFetchInterceptRequestFail(PrFetchInterceptRequest* request, PrNetError error) {
    InterceptFetch* fetch = static_cast<InterceptFetch*>(request);
    PR_ASSERT(fetch->state == InterceptState_Asked && error != PrNetError_None);
    fetch->state = InterceptState_Failed;
    fetch->error = error;
    fetch->timing.endTime = prTimeMonotonicMilliseconds();
    deliver(fetch, PrFetchEvent_Fail);
    release(fetch);
}

void prFetchInterceptRequestPass(PrFetchInterceptRequest* request) {
    InterceptFetch* fetch = static_cast<InterceptFetch*>(request);
    PR_ASSERT(fetch->state == InterceptState_Asked);
    InterceptFetcher* fetcher = fetcherOf(fetch);
    PR_ASSERT(fetcher->next != nullptr);
    fetch->state = InterceptState_Passed;
    if (!fetch->dead) {
        if (prFetchCreate(fetcher->next, &fetch->desc, passedEvent, fetch, &fetch->passed) != PrStatus_Ok) {
            fetch->state = InterceptState_Failed;
            fetch->error = PrNetError_Protocol;
            deliver(fetch, PrFetchEvent_Fail);
        }
    }
    release(fetch);
}
