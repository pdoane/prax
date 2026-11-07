// The interface that fetcher backends implement. Callers use the functions of pr/fetch.h, which call these. Only
// backends include this header.

#pragma once

#include "pr/base.h"
#include "pr/fetch.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The functions of a fetch's backend, at the start of its state. Each implements the function of pr/fetch.h that it is
// named for. A null `responseTypeFn` means that the response is Basic, a null `timingFn` that the fetcher measures
// nothing, and a null `sentReferrerFn` that the fetch sent no referrer.
struct PrFetch {
    void (*destroyFn)(PrFetch* fetch);
    uint16_t (*statusFn)(const PrFetch* fetch);
    PrStringView (*urlFn)(const PrFetch* fetch);
    PrFetchResponseType (*PR_NULLABLE responseTypeFn)(const PrFetch* fetch);
    uint32_t (*headerCountFn)(const PrFetch* fetch);
    PrFetchHeader (*headerFn)(const PrFetch* fetch, uint32_t index);
    size_t (*readFn)(PrFetch* fetch, uint8_t* output, size_t capacity);
    PrNetError (*errorFn)(const PrFetch* fetch);
    PrFetchTiming (*PR_NULLABLE timingFn)(const PrFetch* fetch);
    PrStringView (*PR_NULLABLE sentReferrerFn)(const PrFetch* fetch);
};

// The functions of a fetcher's backend, at the start of its state. Each implements the function of pr/fetch.h that it
// is named for. `blobStoreFn` returns the blob URLs that the fetcher answers, which fetchers that pass requests on
// share.
struct PrFetchNetwork {
    void (*destroyFn)(PrFetchNetwork* fetcher);
    PrStatus (*fetchCreateFn)(PrFetchNetwork* fetcher, const PrFetchRequestDesc* desc, PrFetchFn PR_NULLABLE event,
        void* PR_NULLABLE context, PrFetch* PR_NULLABLE* PR_NONNULL out);
    PrNet* PR_NONNULL (*PR_NONNULL netFn)(const PrFetchNetwork* fetcher);
    PrFetchBlobStore* PR_NONNULL (*PR_NONNULL blobStoreFn)(const PrFetchNetwork* fetcher);
    PrStringView (*userAgentFn)(const PrFetchNetwork* fetcher);
};

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
