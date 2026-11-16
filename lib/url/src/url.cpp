// URL and URLSearchParams.
//
// url - https://url.spec.whatwg.org/

#include "pr/url.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/utf8.h"

#include "url/src/url_percent.h"
#include "url/src/url_public_suffix.h"
#include "url/src/url_record.h"

namespace pr_impl {

namespace {

PrStringView withoutPrefix(PrStringView value, char prefix) {
    return value.size != 0 && value.data[0] == prefix ? prStringFrom(value, 1) : value;
}

bool matches(const UrlPair* pair, PrStringView name, PrStringView value) {
    return prRawStringView(&pair->name) == name && (value.data == nullptr || prRawStringView(&pair->value) == value);
}

// Maps code points to the order of their UTF-16 code units. The BMP above the surrogates sorts above every
// supplementary code point.
uint32_t utf16Order(uint32_t codePoint) {
    return codePoint >= 0xe000 && codePoint <= 0xffff ? codePoint + 0x110000 : codePoint;
}

bool lessUtf16(PrStringView a, PrStringView b) {
    size_t i = 0;
    size_t j = 0;
    while (i < a.size && j < b.size) {
        uint32_t x;
        uint32_t y;
        i += prUtf8Decode(a, i, &x);
        j += prUtf8Decode(b, j, &y);
        if (x != y)
            return utf16Order(x) < utf16Order(y);
    }
    return i == a.size && j != b.size;
}

void appendPair(UrlSearchParams* params, PrStringView name, PrStringView value) {
    params->pairs.push(UrlPair{prRawStringCopy(name), prRawStringCopy(value)});
}

void erasePair(UrlSearchParams* params, uint32_t index) {
    prRawStringReset(&params->pairs.data[index].name);
    prRawStringReset(&params->pairs.data[index].value);
    params->pairs.erase(index);
}

PrStatus setHref(PrUrl* url, PrStringView value) {
    PrUrl* parsed = nullptr;
    PrStatus status = urlParse(value, nullptr, PrCharset_Utf8, &parsed);
    if (status != PrStatus_Ok)
        return status;
    PrUrlSearchParams* params = url->searchParams;
    url->searchParams = nullptr;
    Url previous = *url;
    *url = *parsed;
    *parsed = PrUrl{previous};
    urlDestroy(parsed);
    url->searchParams = params;
    if (params != nullptr)
        urlSearchParamsParse(params, prRawStringView(&url->query));
    return PrStatus_Ok;
}

void setProtocol(PrUrl* url, PrStringView value) {
    PrTextBuffer input = {};
    prTextBufferAppend(&input, value);
    prTextBufferAppendChar(&input, ':');
    urlParseFrom(url, prTextBufferView(&input), UrlState_SchemeStart);
    prTextBufferReset(&input);
}

// Sets the username or the password. A URL that cannot have credentials is left unchanged.
void setCredential(Url* url, PrRawString* credential, PrStringView value) {
    if (urlCannotHaveCredentials(url))
        return;
    PrTextBuffer encoded = {};
    urlAppendPercentEncoded(&encoded, value, PrUrlPercentEncodeSet_Userinfo);
    prRawStringReset(credential);
    *credential = prTextBufferTake(&encoded);
}

void setHost(PrUrl* url, PrStringView value) {
    if (!url->opaquePath)
        urlParseFrom(url, value, UrlState_Host);
}

void setHostname(PrUrl* url, PrStringView value) {
    if (!url->opaquePath)
        urlParseFrom(url, value, UrlState_Hostname);
}

void setPort(PrUrl* url, PrStringView value) {
    if (urlCannotHaveCredentials(url))
        return;
    if (value.size == 0)
        url->hasPort = false;
    else
        urlParseFrom(url, value, UrlState_Port);
}

void setPathname(PrUrl* url, PrStringView value) {
    if (url->opaquePath)
        return;
    prRawStringReset(&url->path);
    urlParseFrom(url, value, UrlState_PathStart);
}

void setSearch(PrUrl* url, PrStringView value) {
    prRawStringReset(&url->query);
    PrStringView input = withoutPrefix(value, '?');
    if (value.size != 0) {
        url->query = prRawStringEmpty();
        urlParseFrom(url, input, UrlState_Query);
    }
    if (url->searchParams != nullptr)
        urlSearchParamsParse(url->searchParams, value.size != 0 ? input : "");
}

void setHash(PrUrl* url, PrStringView value) {
    prRawStringReset(&url->fragment);
    if (value.size == 0)
        return;
    url->fragment = prRawStringEmpty();
    urlParseFrom(url, withoutPrefix(value, '#'), UrlState_Fragment);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrStatus prUrlCreate(PrStringView input, const PrUrl* PR_NULLABLE base, PrCharset charset,
        PrUrl* PR_NULLABLE* PR_NONNULL out) {
    return urlParse(input, base, prCharsetOutput(charset), out);
}

PrUrl* prUrlClone(const PrUrl* url) {
    return urlClone(url);
}

void prUrlDestroy(PrUrl* PR_NULLABLE url) {
    urlDestroy(url);
}

PrRawString prUrlSerialize(const PrUrl* url, PrUrlSerialization serialization) {
    return urlSerialize(url, serialization);
}

PrRawString prUrlOrigin(const PrUrl* url) {
    return urlOrigin(url);
}

bool prUrlHasSameOrigin(const PrUrl* url, const PrUrl* other) {
    PrRawString a = urlOrigin(url);
    PrRawString b = urlOrigin(other);
    bool same = prRawStringView(&a) == prRawStringView(&b) && prRawStringView(&a) != "null";
    prRawStringReset(&a);
    prRawStringReset(&b);
    return same;
}

PrStatus prUrlHostParse(PrStringView input, PrRawString* out) {
    return urlHostParse(input, out);
}

PrStringView prUrlHostPublicSuffix(PrStringView host) {
    // A serialized IPv6 address is bracketed, and an IPv4 address ends in a number (url/#ends-in-a-number-checker),
    // which no domain does. An opaque host has no rules to match.
    if (host.size == 0 || host.data[0] == '[')
        return PrStringView{};
    PrStringView name = host;
    if (name.data[name.size - 1] == '.')
        name.size -= 1;
    size_t lastDot = name.size;
    while (lastDot != 0 && name.data[lastDot - 1] != '.')
        --lastDot;
    PrStringView last = prStringFrom(name, lastDot);
    bool numeric = last.size != 0;
    for (size_t i = 0; i < last.size && numeric; ++i)
        numeric = prAsciiIsDigit(last.data[i]);
    if (numeric || last.size == 0)
        return PrStringView{};
    // publicsuffix.org/list/: the prevailing rule is an exception if one matches, and otherwise the matching rule with
    // the most labels, or the implicit `*` rule. An exception's suffix is its name without its first label.
    size_t suffix = lastDot;
    for (size_t at = name.size;;) {
        size_t start = at;
        // `start` is where the candidate of one more label begins, or 0 for the whole name.
        while (start != 0 && name.data[start - 1] != '.')
            --start;
        PrStringView candidate = prStringFrom(name, start);
        UrlSuffixRuleFlags rules = urlSuffixRules(candidate);
        if ((rules & UrlSuffixRuleFlag_Exception) != 0) {
            suffix = at == name.size ? start : at + 1;
            break;
        }
        if ((rules & UrlSuffixRuleFlag_Exact) != 0)
            suffix = start;
        // A wildcard rule for the candidate makes the candidate and one more label a suffix.
        if ((rules & UrlSuffixRuleFlag_Wildcard) != 0 && start != 0) {
            size_t more = start - 1;
            while (more != 0 && name.data[more - 1] != '.')
                --more;
            PrStringView wider = prStringFrom(name, more);
            if ((urlSuffixRules(wider) & UrlSuffixRuleFlag_Exception) == 0)
                suffix = more;
        }
        if (start == 0)
            break;
        at = start - 1;
    }
    return prStringFrom(host, suffix);
}

PrStringView prUrlHostRegistrableDomain(PrStringView host) {
    PrStringView suffix = prUrlHostPublicSuffix(host);
    if (suffix.data == nullptr || suffix.size == host.size)
        return PrStringView{};
    size_t start = host.size - suffix.size - 1;
    while (start != 0 && host.data[start - 1] != '.')
        --start;
    return prStringFrom(host, start);
}

PrStringView prUrlScheme(const PrUrl* url) {
    return prRawStringView(&url->scheme);
}

PrStringView prUrlUsername(const PrUrl* url) {
    return prRawStringView(&url->username);
}

PrStringView prUrlPassword(const PrUrl* url) {
    return prRawStringView(&url->password);
}

PrStringView prUrlHost(const PrUrl* url) {
    return prRawStringView(&url->host);
}

PrStringView prUrlPath(const PrUrl* url) {
    return prRawStringView(&url->path);
}

PrStringView prUrlQuery(const PrUrl* url) {
    return prRawStringView(&url->query);
}

PrStringView prUrlFragment(const PrUrl* url) {
    return prRawStringView(&url->fragment);
}

int32_t prUrlPort(const PrUrl* url) {
    return url->hasPort ? int32_t(url->port) : -1;
}

PrRawString prUrlComponent(const PrUrl* url, PrUrlComponent component) {
    PrTextBuffer text = {};
    PrStringView host = prRawStringView(&url->host);
    switch (component) {
        case PrUrlComponent_Href: return urlSerialize(url, PrUrlSerialization_Whole);
        case PrUrlComponent_Origin: return urlOrigin(url);
        case PrUrlComponent_Protocol:
            prTextBufferAppend(&text, prRawStringView(&url->scheme));
            prTextBufferAppendChar(&text, ':');
            break;
        case PrUrlComponent_Username:
            prTextBufferAppend(&text, prRawStringView(&url->username));
            break;
        case PrUrlComponent_Password:
            prTextBufferAppend(&text, prRawStringView(&url->password));
            break;
        case PrUrlComponent_Host:
            prTextBufferAppend(&text, host);
            if (host.data != nullptr && url->hasPort) {
                prTextBufferAppendChar(&text, ':');
                prTextBufferAppendU64(&text, url->port, 10);
            }
            break;
        case PrUrlComponent_Hostname:
            prTextBufferAppend(&text, host);
            break;
        case PrUrlComponent_Port:
            if (url->hasPort)
                prTextBufferAppendU64(&text, url->port, 10);
            break;
        case PrUrlComponent_Pathname:
            prTextBufferAppend(&text, prRawStringView(&url->path));
            break;
        case PrUrlComponent_Search:
        case PrUrlComponent_Hash: {
            const PrRawString* part = component == PrUrlComponent_Search ? &url->query : &url->fragment;
            if (!prRawStringIsEmpty(part)) {
                prTextBufferAppendChar(&text, component == PrUrlComponent_Search ? '?' : '#');
                prTextBufferAppend(&text, prRawStringView(part));
            }
            break;
        }
        default:
            break;
    }
    return prTextBufferTake(&text);
}

PrStatus prUrlSetComponent(PrUrl* url, PrUrlComponent component, PrStringView value) {
    switch (component) {
        case PrUrlComponent_Href: return setHref(url, value);
        case PrUrlComponent_Origin: return PrStatus_ReadOnly;
        case PrUrlComponent_Protocol: setProtocol(url, value); break;
        case PrUrlComponent_Username: setCredential(url, &url->username, value); break;
        case PrUrlComponent_Password: setCredential(url, &url->password, value); break;
        case PrUrlComponent_Host: setHost(url, value); break;
        case PrUrlComponent_Hostname: setHostname(url, value); break;
        case PrUrlComponent_Port: setPort(url, value); break;
        case PrUrlComponent_Pathname: setPathname(url, value); break;
        case PrUrlComponent_Search: setSearch(url, value); break;
        default: setHash(url, value); break;
    }
    return PrStatus_Ok;
}

PrUrlSearchParams* prUrlSearchParams(PrUrl* url) {
    if (url->searchParams == nullptr) {
        url->searchParams = allocZeroed<PrUrlSearchParams>();
        url->searchParams->url = url;
        urlSearchParamsParse(url->searchParams, prRawStringView(&url->query));
    }
    return url->searchParams;
}

PrUrlSearchParams* prUrlSearchParamsCreate(void) {
    return allocZeroed<PrUrlSearchParams>();
}

void prUrlSearchParamsDestroy(PrUrlSearchParams* PR_NULLABLE params) {
    PR_ASSERT(params == nullptr || params->url == nullptr);
    urlSearchParamsDestroy(params);
}

void prUrlSearchParamsParse(PrUrlSearchParams* params, PrStringView input) {
    urlSearchParamsParse(params, input);
    urlSearchParamsUpdate(params);
}

PrRawString prUrlSearchParamsToString(const PrUrlSearchParams* params) {
    return urlSearchParamsSerialize(params);
}

uint32_t prUrlSearchParamsSize(const PrUrlSearchParams* params) {
    return params->pairs.count;
}

PrStringView prUrlSearchParamsNameAt(const PrUrlSearchParams* params, uint32_t index) {
    PR_ASSERT(index < params->pairs.count);
    return prRawStringView(&params->pairs.data[index].name);
}

PrStringView prUrlSearchParamsValueAt(const PrUrlSearchParams* params, uint32_t index) {
    PR_ASSERT(index < params->pairs.count);
    return prRawStringView(&params->pairs.data[index].value);
}

PrStringView prUrlSearchParamsGet(const PrUrlSearchParams* params, PrStringView name) {
    for (uint32_t i = 0; i < params->pairs.count; ++i) {
        if (matches(&params->pairs.data[i], name, PrStringView{}))
            return prRawStringView(&params->pairs.data[i].value);
    }
    return PrStringView{};
}

bool prUrlSearchParamsHas(const PrUrlSearchParams* params, PrStringView name, PrStringView value) {
    for (uint32_t i = 0; i < params->pairs.count; ++i) {
        if (matches(&params->pairs.data[i], name, value))
            return true;
    }
    return false;
}

void prUrlSearchParamsAppend(PrUrlSearchParams* params, PrStringView name, PrStringView value) {
    appendPair(params, name, value);
    urlSearchParamsUpdate(params);
}

void prUrlSearchParamsDelete(PrUrlSearchParams* params, PrStringView name, PrStringView value) {
    for (uint32_t i = params->pairs.count; i != 0; --i) {
        if (matches(&params->pairs.data[i - 1], name, value))
            erasePair(params, i - 1);
    }
    urlSearchParamsUpdate(params);
}

void prUrlSearchParamsSet(PrUrlSearchParams* params, PrStringView name, PrStringView value) {
    bool found = false;
    for (uint32_t i = 0; i < params->pairs.count;) {
        UrlPair* pair = &params->pairs.data[i];
        if (!matches(pair, name, PrStringView{})) {
            ++i;
        } else if (!found) {
            prRawStringReplace(&pair->value, value);
            found = true;
            ++i;
        } else {
            erasePair(params, i);
        }
    }
    if (!found)
        appendPair(params, name, value);
    urlSearchParamsUpdate(params);
}

void prUrlSearchParamsSort(PrUrlSearchParams* params) {
    RawArray<UrlPair>* pairs = &params->pairs;
    for (uint32_t i = 1; i < pairs->count; ++i) {
        UrlPair pair = pairs->data[i];
        uint32_t j = i;
        for (; j != 0 && lessUtf16(prRawStringView(&pair.name), prRawStringView(&pairs->data[j - 1].name)); --j)
            pairs->data[j] = pairs->data[j - 1];
        pairs->data[j] = pair;
    }
    urlSearchParamsUpdate(params);
}
