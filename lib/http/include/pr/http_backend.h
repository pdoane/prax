// The interface that HTTP backends implement. Callers use the functions of pr/http.h, which call these. Only backends
// include this header.

#pragma once

#include "pr/base.h"
#include "pr/http.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The functions of an exchange's backend, at the start of its state. Each implements the function of pr/http.h that it
// is named for.
struct PrHttpExchange {
    void (*destroyFn)(PrHttpExchange* exchange);
    uint16_t (*statusFn)(const PrHttpExchange* exchange);
    uint32_t (*headerCountFn)(const PrHttpExchange* exchange);
    PrHttpHeader (*headerFn)(const PrHttpExchange* exchange, uint32_t index);
    size_t (*readFn)(PrHttpExchange* exchange, uint8_t* output, size_t capacity);
    PrNetError (*errorFn)(const PrHttpExchange* exchange);
    PrStringView (*protocolFn)(const PrHttpExchange* exchange);
};

// The functions of a source's backend, at the start of its state. Each implements the function of pr/http.h that it is
// named for.
struct PrHttp {
    void (*destroyFn)(PrHttp* http);
    PrStatus (*exchangeCreateFn)(PrHttp* http, const PrHttpRequestDesc* desc, PrHttpExchange* PR_NULLABLE* PR_NONNULL out);
};

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
