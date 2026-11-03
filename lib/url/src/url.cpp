// URL and URLSearchParams.
//
// url - https://url.spec.whatwg.org/

#include "pr/url.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/utf8.h"

#include "url/src/url_percent.h"
#include "url/src/url_record.h"

namespace pr_impl {

namespace {

PrStringView withoutPrefix(PrStringView value, char prefix) {
    return value.size != 0 && value.data[0] == prefix ? stringSlice(value, 1, value.size - 1) : value;
}

bool matches(const UrlPair* pair, PrStringView name, PrStringView value) {
    return prStringView(&pair->name) == name && (value.data == nullptr || prStringView(&pair->value) == value);
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
    params->pairs.push(UrlPair{prStringCopy(name), prStringCopy(value)});
}

void erasePair(UrlSearchParams* params, uint32_t index) {
    prStringReset(&params->pairs.data[index].name);
    prStringReset(&params->pairs.data[index].value);
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
        urlSearchParamsParse(params, prStringView(&url->query));
    return PrStatus_Ok;
}

void setProtocol(PrUrl* url, PrStringView value) {
    TextBuffer input;
    textAppend(&input, value);
    textAppendChar(&input, ':');
    urlParseFrom(url, textView(&input), UrlState_SchemeStart);
    textReset(&input);
}

// Sets the username or the password. A URL that cannot have credentials is left unchanged.
void setCredential(Url* url, PrString* credential, PrStringView value) {
    if (urlCannotHaveCredentials(url))
        return;
    TextBuffer encoded;
    urlAppendPercentEncoded(&encoded, value, PrUrlPercentEncodeSet_Userinfo);
    prStringReset(credential);
    *credential = textTake(&encoded);
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
    prStringReset(&url->path);
    urlParseFrom(url, value, UrlState_PathStart);
}

void setSearch(PrUrl* url, PrStringView value) {
    prStringReset(&url->query);
    PrStringView input = withoutPrefix(value, '?');
    if (value.size != 0) {
        url->query = prStringEmpty();
        urlParseFrom(url, input, UrlState_Query);
    }
    if (url->searchParams != nullptr)
        urlSearchParamsParse(url->searchParams, value.size != 0 ? input : "");
}

void setHash(PrUrl* url, PrStringView value) {
    prStringReset(&url->fragment);
    if (value.size == 0)
        return;
    url->fragment = prStringEmpty();
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

PrString prUrlSerialize(const PrUrl* url, PrUrlSerialization serialization) {
    return urlSerialize(url, serialization);
}

PrString prUrlOrigin(const PrUrl* url) {
    return urlOrigin(url);
}

bool prUrlHasSameOrigin(const PrUrl* url, const PrUrl* other) {
    PrString a = urlOrigin(url);
    PrString b = urlOrigin(other);
    bool same = prStringView(&a) == prStringView(&b) && prStringView(&a) != "null";
    prStringReset(&a);
    prStringReset(&b);
    return same;
}

PrStringView prUrlScheme(const PrUrl* url) {
    return prStringView(&url->scheme);
}

PrStringView prUrlUsername(const PrUrl* url) {
    return prStringView(&url->username);
}

PrStringView prUrlPassword(const PrUrl* url) {
    return prStringView(&url->password);
}

PrStringView prUrlHost(const PrUrl* url) {
    return prStringView(&url->host);
}

PrStringView prUrlPath(const PrUrl* url) {
    return prStringView(&url->path);
}

PrStringView prUrlQuery(const PrUrl* url) {
    return prStringView(&url->query);
}

PrStringView prUrlFragment(const PrUrl* url) {
    return prStringView(&url->fragment);
}

int32_t prUrlPort(const PrUrl* url) {
    return url->hasPort ? int32_t(url->port) : -1;
}

PrString prUrlComponent(const PrUrl* url, PrUrlComponent component) {
    TextBuffer text;
    PrStringView host = prStringView(&url->host);
    switch (component) {
        case PrUrlComponent_Href: return urlSerialize(url, PrUrlSerialization_Whole);
        case PrUrlComponent_Origin: return urlOrigin(url);
        case PrUrlComponent_Protocol:
            textAppend(&text, prStringView(&url->scheme));
            textAppendChar(&text, ':');
            break;
        case PrUrlComponent_Username:
            textAppend(&text, prStringView(&url->username));
            break;
        case PrUrlComponent_Password:
            textAppend(&text, prStringView(&url->password));
            break;
        case PrUrlComponent_Host:
            textAppend(&text, host);
            if (host.data != nullptr && url->hasPort) {
                textAppendChar(&text, ':');
                textAppendU64(&text, url->port);
            }
            break;
        case PrUrlComponent_Hostname:
            textAppend(&text, host);
            break;
        case PrUrlComponent_Port:
            if (url->hasPort)
                textAppendU64(&text, url->port);
            break;
        case PrUrlComponent_Pathname:
            textAppend(&text, prStringView(&url->path));
            break;
        case PrUrlComponent_Search:
        case PrUrlComponent_Hash: {
            const PrString* part = component == PrUrlComponent_Search ? &url->query : &url->fragment;
            if (!prStringIsEmpty(part)) {
                textAppendChar(&text, component == PrUrlComponent_Search ? '?' : '#');
                textAppend(&text, prStringView(part));
            }
            break;
        }
        default:
            break;
    }
    return textTake(&text);
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
        urlSearchParamsParse(url->searchParams, prStringView(&url->query));
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

PrString prUrlSearchParamsToString(const PrUrlSearchParams* params) {
    return urlSearchParamsSerialize(params);
}

uint32_t prUrlSearchParamsSize(const PrUrlSearchParams* params) {
    return params->pairs.count;
}

PrStringView prUrlSearchParamsNameAt(const PrUrlSearchParams* params, uint32_t index) {
    PR_ASSERT(index < params->pairs.count);
    return prStringView(&params->pairs.data[index].name);
}

PrStringView prUrlSearchParamsValueAt(const PrUrlSearchParams* params, uint32_t index) {
    PR_ASSERT(index < params->pairs.count);
    return prStringView(&params->pairs.data[index].value);
}

PrStringView prUrlSearchParamsGet(const PrUrlSearchParams* params, PrStringView name) {
    for (uint32_t i = 0; i < params->pairs.count; ++i) {
        if (matches(&params->pairs.data[i], name, PrStringView{}))
            return prStringView(&params->pairs.data[i].value);
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
            prStringReplace(&pair->value, value);
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
    Array<UrlPair>* pairs = &params->pairs;
    for (uint32_t i = 1; i < pairs->count; ++i) {
        UrlPair pair = pairs->data[i];
        uint32_t j = i;
        for (; j != 0 && lessUtf16(prStringView(&pair.name), prStringView(&pairs->data[j - 1].name)); --j)
            pairs->data[j] = pairs->data[j - 1];
        pairs->data[j] = pair;
    }
    urlSearchParamsUpdate(params);
}
