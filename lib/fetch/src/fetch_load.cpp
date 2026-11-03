// Fetch loads: whole responses fetched on the network's loop for another thread.
//
// A load is counted by its owner, by the loop while the load's fetch may run, and by each task that the owner posts to
// the loop after the first. The owner and the loop meet only under the load's mutex, through `dropped`. The loop posts
// the response to the owner only if the owner has not dropped the load. The owner's task that receives the response
// delivers it only if the owner has not dropped the load since.

#include "pr/fetch_load.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/memory.h"
#include "pr/mutex.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"

namespace pr_impl {

namespace {

// A header that the load holds a copy of.
struct LoadHeader {
    PrRawString name;
    PrRawString value;
};

struct FetchLoad {
    Atomic<uint32_t> references;
    PrFetchNetwork* fetcher;
    PrFetchLoadDesc desc;

    // The request, whose views are of the copies that follow it.
    PrFetchRequestDesc request;
    PrRawString url;
    PrRawString method;
    PrRawString origin;
    PrRawString referrer;
    RawArray<LoadHeader> requestHeaders;
    RawArray<PrFetchHeader> requestViews;
    RawArray<uint8_t> requestBody;

    // The loop's state. `ended` is true once the loop is done with the fetch, which then is null.
    PrFetch* PR_NULLABLE fetch;
    bool ended;

    // The response, which the loop writes before it posts the load to the owner.
    PrStatus status;
    PrFetchResponseType type;
    uint16_t responseStatus;
    PrRawString responseUrl;
    RawArray<LoadHeader> responseHeaders;
    RawArray<uint8_t> body;

    // `dropped` is true once the owner destroys the load, under the mutex. `delivered` is the owner's, true once the
    // load has called `done`.
    PrMutex mutex;
    bool dropped;
    bool delivered;
};

// Returns a copy of `text` that keeps the null view distinct from the empty one.
PrRawString copyOrNull(PrStringView text) {
    return text.data != nullptr ? prRawStringCopy(text) : prRawStringNull();
}

void copyHeaders(RawArray<LoadHeader>* out, const PrFetchHeader* PR_NULLABLE headers, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i)
        out->push(LoadHeader{prRawStringCopy(headers[i].name), prRawStringCopy(headers[i].value)});
}

void resetHeaders(RawArray<LoadHeader>* headers) {
    for (uint32_t i = 0; i < headers->count; ++i) {
        prRawStringReset(&headers->data[i].name);
        prRawStringReset(&headers->data[i].value);
    }
    headers->reset();
}

void release(FetchLoad* load) {
    if (load->references.fetchSub(1) != 1)
        return;
    PR_ASSERT(load->fetch == nullptr);
    prRawStringReset(&load->url);
    prRawStringReset(&load->method);
    prRawStringReset(&load->origin);
    prRawStringReset(&load->referrer);
    resetHeaders(&load->requestHeaders);
    load->requestViews.reset();
    load->requestBody.reset();
    prRawStringReset(&load->responseUrl);
    resetHeaders(&load->responseHeaders);
    load->body.reset();
    prMutexReset(&load->mutex);
    prMemoryFree(load);
}

// Calls `done` with the response on the owner's thread, unless the owner dropped the load after the loop posted it.
// Releases the loop's reference.
void arrive(void* PR_NULLABLE context) {
    FetchLoad* load = static_cast<FetchLoad*>(context);
    prMutexLock(&load->mutex);
    bool dropped = load->dropped;
    prMutexUnlock(&load->mutex);
    if (!dropped) {
        load->delivered = true;
        RawArray<PrFetchHeader> headers{};
        for (uint32_t i = 0; i < load->responseHeaders.count; ++i) {
            const LoadHeader& header = load->responseHeaders.data[i];
            headers.push(PrFetchHeader{prRawStringView(&header.name), prRawStringView(&header.value)});
        }
        PrFetchResponse response{
            .status = load->status,
            .type = load->type,
            .responseStatus = load->responseStatus,
            .url = prRawStringView(&load->responseUrl),
            .headers = headers.data,
            .headerCount = headers.count,
            .body = PrByteView{load->body.data, load->body.count},
        };
        // `done` may destroy the load, which the loop's reference keeps alive until the call returns.
        load->desc.done(load->desc.context, &response);
        headers.reset();
    }
    release(load);
}

// Ends the fetch on the loop with `status`, and posts the load to its owner unless the owner dropped it. The post is
// under the mutex, so that once the owner has dropped the load, the loop posts nothing more to its queue.
void finish(FetchLoad* load, PrStatus status) {
    prFetchDestroy(load->fetch);
    load->fetch = nullptr;
    load->ended = true;
    load->status = status;
    prMutexLock(&load->mutex);
    bool dropped = load->dropped;
    if (!dropped)
        load->desc.post(load->desc.queue, arrive, load);
    prMutexUnlock(&load->mutex);
    if (dropped)
        release(load);
}

// Reads the body that arrived. Returns false if it is larger than the load accepts.
bool readBody(FetchLoad* load, PrFetch* fetch) {
    uint8_t chunk[16 * 1024];
    for (size_t got = prFetchRead(fetch, chunk, sizeof chunk); got != 0; got = prFetchRead(fetch, chunk, sizeof chunk)) {
        uint64_t limit = load->desc.maxBodySize;
        if (limit != 0 && load->body.count + got > limit)
            return false;
        load->body.append(chunk, uint32_t(got));
    }
    return true;
}

void fetchEvent(void* PR_NULLABLE context, PrFetch* fetch, PrFetchEvent event) {
    FetchLoad* load = static_cast<FetchLoad*>(context);
    switch (event) {
        case PrFetchEvent_Response: {
            load->type = prFetchResponseType(fetch);
            load->responseStatus = prFetchStatus(fetch);
            load->responseUrl = prRawStringCopy(prFetchUrl(fetch));
            uint32_t count = prFetchHeaderCount(fetch);
            for (uint32_t i = 0; i < count; ++i) {
                PrFetchHeader header = prFetchHeader(fetch, i);
                load->responseHeaders.push(LoadHeader{prRawStringCopy(header.name), prRawStringCopy(header.value)});
            }
            break;
        }
        case PrFetchEvent_Readable:
            if (!readBody(load, fetch))
                finish(load, PrStatus_Limit);
            break;
        case PrFetchEvent_End:
            finish(load, readBody(load, fetch) ? PrStatus_Ok : PrStatus_Limit);
            break;
        case PrFetchEvent_Fail:
            finish(load, PrStatus_Io);
            break;
    }
}

// Starts the fetch on the loop, unless the owner dropped the load first.
void begin(void* PR_NULLABLE context) {
    FetchLoad* load = static_cast<FetchLoad*>(context);
    prMutexLock(&load->mutex);
    bool dropped = load->dropped;
    prMutexUnlock(&load->mutex);
    if (dropped) {
        load->ended = true;
        release(load);
        return;
    }
    if (prFetchCreate(load->fetcher, &load->request, fetchEvent, load, &load->fetch) != PrStatus_Ok)
        finish(load, PrStatus_InvalidData);
}

// Stops the fetch of a dropped load on the loop, if it still runs, and releases the loop's reference with it. Then
// releases this task's reference.
void cancel(void* PR_NULLABLE context) {
    FetchLoad* load = static_cast<FetchLoad*>(context);
    if (!load->ended) {
        prFetchDestroy(load->fetch);
        load->fetch = nullptr;
        load->ended = true;
        release(load);
    }
    release(load);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrFetchLoad : FetchLoad {};

PrFetchLoad* prFetchLoadCreate(PrFetchNetwork* fetcher, const PrFetchRequestDesc* request, const PrFetchLoadDesc* desc) {
    PR_ASSERT(desc->post != nullptr && desc->done != nullptr);
    PR_ASSERT(request->headers != nullptr || request->headerCount == 0);
    PrFetchLoad* load = allocZeroed<PrFetchLoad>();
    // The owner's reference and the loop's.
    load->references.storeRelaxed(2);
    load->fetcher = fetcher;
    load->desc = *desc;
    load->url = prRawStringCopy(request->url);
    load->method = copyOrNull(request->method);
    load->origin = copyOrNull(request->origin);
    load->referrer = copyOrNull(request->referrer);
    copyHeaders(&load->requestHeaders, request->headers, request->headerCount);
    for (uint32_t i = 0; i < load->requestHeaders.count; ++i) {
        const LoadHeader& header = load->requestHeaders.data[i];
        load->requestViews.push(PrFetchHeader{prRawStringView(&header.name), prRawStringView(&header.value)});
    }
    if (request->body.size != 0)
        load->requestBody.append(request->body.data, uint32_t(request->body.size));
    load->request = *request;
    load->request.url = prRawStringView(&load->url);
    load->request.method = prRawStringView(&load->method);
    load->request.origin = prRawStringView(&load->origin);
    load->request.referrer = prRawStringView(&load->referrer);
    load->request.headers = load->requestViews.data;
    load->request.body = PrByteView{load->requestBody.data, load->requestBody.count};
    prMutexInit(&load->mutex);
    prNetPost(fetcher->net, begin, load);
    return load;
}

void prFetchLoadDestroy(PrFetchLoad* PR_NULLABLE load) {
    if (load == nullptr)
        return;
    prMutexLock(&load->mutex);
    load->dropped = true;
    prMutexUnlock(&load->mutex);
    // A load that has not delivered may still be fetching, which the loop stops.
    if (!load->delivered) {
        load->references.fetchAdd(1);
        prNetPost(load->fetcher->net, cancel, load);
    }
    release(load);
}
