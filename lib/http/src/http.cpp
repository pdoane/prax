// HTTP exchanges: a request sent and its response received, over the connections of a network.

#include "pr/http.h"

#include "pr/ascii.h"
#include "pr/assert.h"

#if defined(PR_HTTP_HAS_BUILTIN)
#include "pr/http_builtin.h"
#endif

namespace pr_impl {

namespace {

constexpr PrStringView kBackendNames[PrHttpBackend_Count] = {"default", "builtin"};

// The backends that the build has, which Default creates the first of, and then Default, so that a build without
// backends creates none.
constexpr PrHttpBackend kBuilt[] = {
#if defined(PR_HTTP_HAS_BUILTIN)
    PrHttpBackend_Builtin,
#endif
    PrHttpBackend_Default,
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrHttpBackend prHttpBackendFromName(PrStringView name) {
    for (PrHttpBackend backend = 0; backend < PrHttpBackend_Count; ++backend) {
        if (asciiEqualsFold(kBackendNames[backend], name))
            return backend;
    }
    return PrHttpBackend_Count;
}

PrStringView prHttpBackendName(PrHttpBackend backend) {
    return backend < PrHttpBackend_Count ? kBackendNames[backend] : PrStringView{};
}

bool prHttpBackendIsBuilt(PrHttpBackend backend) {
    for (PrHttpBackend built : kBuilt) {
        if (built != PrHttpBackend_Default && (built == backend || backend == PrHttpBackend_Default))
            return true;
    }
    return false;
}

PrHttp* PR_NULLABLE prHttpCreate(PrHttpBackend backend, const PrHttpDesc* desc) {
    switch (backend == PrHttpBackend_Default ? kBuilt[0] : backend) {
#if defined(PR_HTTP_HAS_BUILTIN)
        case PrHttpBackend_Builtin: return prHttpBuiltinCreate(desc);
#endif
        default: return nullptr;
    }
}

void prHttpDestroy(PrHttp* PR_NULLABLE http) {
    if (http != nullptr)
        http->destroy(http);
}

PrStatus prHttpExchangeCreate(PrHttp* http, const PrHttpRequestDesc* desc, PrHttpExchange* PR_NULLABLE* PR_NONNULL out) {
    PR_ASSERT(desc->headers != nullptr || desc->headerCount == 0);
    PR_ASSERT(desc->body.data != nullptr || desc->body.size == 0);
    *out = nullptr;
    PrStatus status = http->exchangeCreate(http, desc, out);
    PR_ASSERT(status != PrStatus_Ok || ((*out) != nullptr && (*out)->http == http));
    return status;
}

void prHttpExchangeDestroy(PrHttpExchange* PR_NULLABLE exchange) {
    if (exchange != nullptr)
        exchange->http->exchangeDestroy(exchange->http, exchange);
}

uint16_t prHttpExchangeStatus(const PrHttpExchange* exchange) {
    return exchange->http->exchangeStatus(exchange->http, exchange);
}

uint32_t prHttpExchangeHeaderCount(const PrHttpExchange* exchange) {
    return exchange->http->exchangeHeaderCount(exchange->http, exchange);
}

PrHttpHeader prHttpExchangeHeader(const PrHttpExchange* exchange, uint32_t index) {
    PR_ASSERT(index < prHttpExchangeHeaderCount(exchange));
    return exchange->http->exchangeHeader(exchange->http, exchange, index);
}

size_t prHttpExchangeRead(PrHttpExchange* exchange, uint8_t* PR_NULLABLE output, size_t capacity) {
    PR_ASSERT(output != nullptr || capacity == 0);
    if (capacity == 0)
        return 0;
    return exchange->http->exchangeRead(exchange->http, exchange, output, capacity);
}

PrNetError prHttpExchangeError(const PrHttpExchange* exchange) {
    return exchange->http->exchangeError(exchange->http, exchange);
}
