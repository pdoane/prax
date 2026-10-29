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
    return value.size != 0 && value.data[0] == prefix ? stringSlice(value, 1, value.size - 1)
                                                      : value;
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

void appendPair(UrlSearchParams* self, PrStringView name, PrStringView value) {
    self->pairs.push(UrlPair{prStringCopy(name), prStringCopy(value)});
}

void erasePair(UrlSearchParams* self, uint32_t index) {
    prStringReset(&self->pairs.data[index].name);
    prStringReset(&self->pairs.data[index].value);
    self->pairs.erase(index);
}

PrStatus setHref(PrUrl* self, PrStringView value) {
    PrUrl* parsed = nullptr;
    PrStatus status = urlParse(value, nullptr, PrCharset_Utf8, &parsed);
    if (status != PrStatus_Ok)
        return status;
    Url* url = self;
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

void setProtocol(PrUrl* self, PrStringView value) {
    TextBuffer input;
    textAppend(&input, value);
    textAppendChar(&input, ':');
    urlParseFrom(self, textView(&input), UrlState_SchemeStart);
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

void setHost(PrUrl* self, PrStringView value) {
    if (!self->opaquePath)
        urlParseFrom(self, value, UrlState_Host);
}

void setHostname(PrUrl* self, PrStringView value) {
    if (!self->opaquePath)
        urlParseFrom(self, value, UrlState_Hostname);
}

void setPort(PrUrl* self, PrStringView value) {
    if (urlCannotHaveCredentials(self))
        return;
    if (value.size == 0)
        self->hasPort = false;
    else
        urlParseFrom(self, value, UrlState_Port);
}

void setPathname(PrUrl* self, PrStringView value) {
    Url* url = self;
    if (url->opaquePath)
        return;
    prStringReset(&url->path);
    urlParseFrom(url, value, UrlState_PathStart);
}

void setSearch(PrUrl* self, PrStringView value) {
    Url* url = self;
    prStringReset(&url->query);
    PrStringView input = withoutPrefix(value, '?');
    if (value.size != 0) {
        url->query = prStringEmpty();
        urlParseFrom(url, input, UrlState_Query);
    }
    if (url->searchParams != nullptr)
        urlSearchParamsParse(url->searchParams, value.size != 0 ? input : "");
}

void setHash(PrUrl* self, PrStringView value) {
    Url* url = self;
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

PrUrl* prUrlClone(const PrUrl* self) {
    return urlClone(self);
}

void prUrlDestroy(PrUrl* PR_NULLABLE self) {
    urlDestroy(self);
}

PrString prUrlSerialize(const PrUrl* self, PrUrlSerialization serialization) {
    return urlSerialize(self, serialization);
}

PrString prUrlOrigin(const PrUrl* self) {
    return urlOrigin(self);
}

bool prUrlHasSameOrigin(const PrUrl* self, const PrUrl* other) {
    PrString a = urlOrigin(self);
    PrString b = urlOrigin(other);
    bool same = prStringView(&a) == prStringView(&b) && prStringView(&a) != "null";
    prStringReset(&a);
    prStringReset(&b);
    return same;
}

PrStringView prUrlScheme(const PrUrl* self) {
    return prStringView(&self->scheme);
}

PrStringView prUrlUsername(const PrUrl* self) {
    return prStringView(&self->username);
}

PrStringView prUrlPassword(const PrUrl* self) {
    return prStringView(&self->password);
}

PrStringView prUrlHost(const PrUrl* self) {
    return prStringView(&self->host);
}

PrStringView prUrlPath(const PrUrl* self) {
    return prStringView(&self->path);
}

PrStringView prUrlQuery(const PrUrl* self) {
    return prStringView(&self->query);
}

PrStringView prUrlFragment(const PrUrl* self) {
    return prStringView(&self->fragment);
}

int32_t prUrlPort(const PrUrl* self) {
    return self->hasPort ? int32_t(self->port) : -1;
}

PrString prUrlComponent(const PrUrl* self, PrUrlComponent component) {
    const Url* url = self;
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

PrStatus prUrlSetComponent(PrUrl* self, PrUrlComponent component, PrStringView value) {
    switch (component) {
        case PrUrlComponent_Href: return setHref(self, value);
        case PrUrlComponent_Origin: return PrStatus_ReadOnly;
        case PrUrlComponent_Protocol: setProtocol(self, value); break;
        case PrUrlComponent_Username: setCredential(self, &self->username, value); break;
        case PrUrlComponent_Password: setCredential(self, &self->password, value); break;
        case PrUrlComponent_Host: setHost(self, value); break;
        case PrUrlComponent_Hostname: setHostname(self, value); break;
        case PrUrlComponent_Port: setPort(self, value); break;
        case PrUrlComponent_Pathname: setPathname(self, value); break;
        case PrUrlComponent_Search: setSearch(self, value); break;
        default: setHash(self, value); break;
    }
    return PrStatus_Ok;
}

PrUrlSearchParams* prUrlSearchParams(PrUrl* self) {
    if (self->searchParams == nullptr) {
        self->searchParams = allocZeroed<PrUrlSearchParams>();
        self->searchParams->url = self;
        urlSearchParamsParse(self->searchParams, prStringView(&self->query));
    }
    return self->searchParams;
}

PrUrlSearchParams* prUrlSearchParamsCreate(void) {
    return allocZeroed<PrUrlSearchParams>();
}

void prUrlSearchParamsDestroy(PrUrlSearchParams* PR_NULLABLE self) {
    PR_ASSERT(self == nullptr || self->url == nullptr);
    urlSearchParamsDestroy(self);
}

void prUrlSearchParamsParse(PrUrlSearchParams* self, PrStringView input) {
    urlSearchParamsParse(self, input);
    urlSearchParamsUpdate(self);
}

PrString prUrlSearchParamsToString(const PrUrlSearchParams* self) {
    return urlSearchParamsSerialize(self);
}

uint32_t prUrlSearchParamsSize(const PrUrlSearchParams* self) {
    return self->pairs.count;
}

PrStringView prUrlSearchParamsNameAt(const PrUrlSearchParams* self, uint32_t index) {
    PR_ASSERT(index < self->pairs.count);
    return prStringView(&self->pairs.data[index].name);
}

PrStringView prUrlSearchParamsValueAt(const PrUrlSearchParams* self, uint32_t index) {
    PR_ASSERT(index < self->pairs.count);
    return prStringView(&self->pairs.data[index].value);
}

PrStringView prUrlSearchParamsGet(const PrUrlSearchParams* self, PrStringView name) {
    const UrlSearchParams* params = self;
    for (uint32_t i = 0; i < params->pairs.count; ++i) {
        if (matches(&params->pairs.data[i], name, PrStringView{}))
            return prStringView(&params->pairs.data[i].value);
    }
    return PrStringView{};
}

bool prUrlSearchParamsHas(const PrUrlSearchParams* self, PrStringView name, PrStringView value) {
    const UrlSearchParams* params = self;
    for (uint32_t i = 0; i < params->pairs.count; ++i) {
        if (matches(&params->pairs.data[i], name, value))
            return true;
    }
    return false;
}

void prUrlSearchParamsAppend(PrUrlSearchParams* self, PrStringView name, PrStringView value) {
    appendPair(self, name, value);
    urlSearchParamsUpdate(self);
}

void prUrlSearchParamsDelete(PrUrlSearchParams* self, PrStringView name, PrStringView value) {
    UrlSearchParams* params = self;
    for (uint32_t i = params->pairs.count; i != 0; --i) {
        if (matches(&params->pairs.data[i - 1], name, value))
            erasePair(params, i - 1);
    }
    urlSearchParamsUpdate(params);
}

void prUrlSearchParamsSet(PrUrlSearchParams* self, PrStringView name, PrStringView value) {
    UrlSearchParams* params = self;
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

void prUrlSearchParamsSort(PrUrlSearchParams* self) {
    Array<UrlPair>* pairs = &self->pairs;
    for (uint32_t i = 1; i < pairs->count; ++i) {
        UrlPair pair = pairs->data[i];
        uint32_t j = i;
        for (;
             j != 0 && lessUtf16(prStringView(&pair.name), prStringView(&pairs->data[j - 1].name));
             --j)
            pairs->data[j] = pairs->data[j - 1];
        pairs->data[j] = pair;
    }
    urlSearchParamsUpdate(self);
}
