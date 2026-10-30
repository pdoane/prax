// JavaScript binding of Headers and Response, and of fetch()'s arguments.
//
// A body is held whole: a Response's bytes, read once by text(), json(), arrayBuffer() or bytes(). Its body attribute,
// a ReadableStream, is absent until a page needs one. A response that fetch() made with a signal keeps the signal, and
// its body is errored with the signal's reason once the signal aborts (fetch/#abort-fetch); a read of the whole body
// rejects with that reason.
//
// fetch - https://fetch.spec.whatwg.org/#fetch-api

#include "pr/fetch_quickjs.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/charset.h"
#include "pr/count_of.h"
#include "pr/dom_abort.h"
#include "pr/dom_quickjs.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/quickjs_binding.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/url.h"
#include "pr/url_quickjs.h"
#include "pr/webidl_quickjs.h"

namespace pr_impl {
namespace {

JSClassID headersClass;
JSClassID headersIteratorClass;
JSClassID responseClass;

// ---------------------------------------------------------------------------------------------------------------------
// ByteStrings (webidl/#idl-ByteString)

// Converts `input` to a ByteString: a string whose code points are each below U+0100, as one byte each. The call's
// scope holds the bytes. Throws a TypeError for a string with any other code point.
bool readByteString(JSContext* ctx, JSValueConst input, PrStringView* out) {
    PrStringView text;
    if (!jsReadString(ctx, input, &text))
        return false;
    size_t ascii = 0;
    while (ascii < text.size && uint8_t(text.data[ascii]) < 0x80)
        ++ascii;
    if (ascii == text.size) {
        *out = text;
        return true;
    }
    char* bytes = static_cast<char*>(prMemoryAlloc(text.size));
    jsHoldBytes(ctx, bytes);
    size_t size = 0;
    for (size_t i = 0; i < text.size; ++i) {
        uint8_t c = uint8_t(text.data[i]);
        if (c < 0x80) {
            bytes[size++] = char(c);
        } else if ((c == 0xc2 || c == 0xc3) && i + 1 < text.size) {
            bytes[size++] = char(((c & 0x1f) << 6) | (uint8_t(text.data[++i]) & 0x3f));
        } else {
            JS_ThrowTypeError(ctx, "A ByteString has a code point above U+00FF");
            return false;
        }
    }
    *out = PrStringView{bytes, size};
    return true;
}

// Returns the string whose code points are the bytes of `bytes` (infra/#isomorphic-decode).
JSValue newByteString(JSContext* ctx, PrStringView bytes) {
    size_t ascii = 0;
    while (ascii < bytes.size && uint8_t(bytes.data[ascii]) < 0x80)
        ++ascii;
    if (ascii == bytes.size)
        return JS_NewStringLen(ctx, bytes.data != nullptr ? bytes.data : "", bytes.size);
    TextBuffer text;
    for (size_t i = 0; i < bytes.size; ++i)
        textAppendCodePoint(&text, uint8_t(bytes.data[i]));
    JSValue result = JS_NewStringLen(ctx, textView(&text).data, textView(&text).size);
    textReset(&text);
    return result;
}

// ---------------------------------------------------------------------------------------------------------------------
// Headers (fetch/#headers-class)

// What a Headers object lets script change (fetch/#concept-headers-guard).
enum {
    Guard_None,
    Guard_Request,
    Guard_RequestNoCors,
    Guard_Response,
    Guard_Immutable,
};
typedef uint8_t Guard;

// A header list (fetch/#concept-header-list) and its guard. Names keep the case of the first header of each name.
struct JsHeaders {
    Array<PrString> names;
    Array<PrString> values;
    Guard guard;
};

JsHeaders* headersCreate(Guard guard) {
    JsHeaders* headers = allocZeroed<JsHeaders>();
    headers->guard = guard;
    return headers;
}

void headersClear(JsHeaders* headers) {
    for (uint32_t i = 0; i < headers->names.count; ++i) {
        prStringReset(&headers->names.data[i]);
        prStringReset(&headers->values.data[i]);
    }
    headers->names.clear();
    headers->values.clear();
}

void headersDestroy(JsHeaders* headers) {
    headersClear(headers);
    headers->names.reset();
    headers->values.reset();
    prMemoryFree(headers);
}

JsHeaders* PR_NULLABLE headersOf(JSContext* ctx, JSValueConst self) {
    return jsValueOf<JsHeaders>(ctx, self, headersClass, "Headers");
}

uint32_t findHeader(const JsHeaders* headers, PrStringView name, uint32_t from = 0) {
    for (uint32_t i = from; i < headers->names.count; ++i) {
        if (asciiEqualsFold(prStringView(&headers->names.data[i]), name))
            return i;
    }
    return kArrayMissing;
}

// Appends to `out` the values of the headers named `name`, separated by ", " (fetch/#concept-header-list-get). Returns
// false if there are none.
bool combinedValue(const JsHeaders* headers, PrStringView name, TextBuffer* out) {
    bool found = false;
    for (uint32_t i = findHeader(headers, name); i != kArrayMissing; i = findHeader(headers, name, i + 1)) {
        if (found)
            textAppend(out, ", ");
        textAppend(out, prStringView(&headers->values.data[i]));
        found = true;
    }
    return found;
}

void appendHeader(JsHeaders* headers, PrStringView name, PrStringView value) {
    uint32_t first = findHeader(headers, name);
    headers->names.push(prStringCopy(first != kArrayMissing ? prStringView(&headers->names.data[first]) : name));
    headers->values.push(prStringCopy(value));
}

void eraseHeaders(JsHeaders* headers, PrStringView name, uint32_t from = 0) {
    for (uint32_t i = findHeader(headers, name, from); i != kArrayMissing; i = findHeader(headers, name, i)) {
        prStringReset(&headers->names.data[i]);
        prStringReset(&headers->values.data[i]);
        headers->names.erase(i);
        headers->values.erase(i);
    }
}

// Returns true if `name` is a privileged no-CORS request-header name (fetch/#privileged-no-cors-request-header-name).
bool isPrivilegedNoCors(PrStringView name) {
    return asciiEqualsFold(name, "range");
}

// fetch/#headers-validate. Returns false, with a TypeError pending, for a name or a value that a header cannot have, or
// for headers that are immutable. Writes to `*ignored` whether the guard ignores the header.
bool validate(JSContext* ctx, const JsHeaders* headers, PrStringView name, PrStringView value, bool* ignored) {
    if (!prFetchIsHeaderName(name)) {
        JS_ThrowTypeError(ctx, "Invalid header name");
        return false;
    }
    if (!prFetchIsHeaderValue(value)) {
        JS_ThrowTypeError(ctx, "Invalid header value");
        return false;
    }
    if (headers->guard == Guard_Immutable) {
        JS_ThrowTypeError(ctx, "Headers are immutable");
        return false;
    }
    *ignored = (headers->guard == Guard_Request && prFetchIsForbiddenRequestHeader(name, value)) ||
        (headers->guard == Guard_Response && prFetchIsForbiddenResponseHeaderName(name));
    return true;
}

// fetch/#concept-headers-append
bool headersAppend(JSContext* ctx, JsHeaders* headers, PrStringView name, PrStringView value) {
    value = prFetchNormalizeHeaderValue(value);
    bool ignored = false;
    if (!validate(ctx, headers, name, value, &ignored))
        return false;
    if (ignored)
        return true;
    if (headers->guard == Guard_RequestNoCors) {
        TextBuffer temporary;
        if (combinedValue(headers, name, &temporary))
            textAppend(&temporary, ", ");
        textAppend(&temporary, value);
        bool safelisted = prFetchIsNoCorsSafelistedRequestHeader(name, textView(&temporary));
        textReset(&temporary);
        if (!safelisted)
            return true;
    }
    appendHeader(headers, name, value);
    if (headers->guard == Guard_RequestNoCors)
        eraseHeaders(headers, "range");
    return true;
}

// fetch/#concept-headers-fill, from a HeadersInit: a sequence of pairs, of which a Headers object is one, or a record.
bool headersFill(JSContext* ctx, JsHeaders* headers, JSValueConst init) {
    if (!JS_IsObject(init)) {
        JS_ThrowTypeError(ctx, "HeadersInit must be an object");
        return false;
    }
    JSValue method = jsIteratorMethod(ctx, init);
    if (JS_IsException(method))
        return false;
    if (!JS_IsNull(method) && !JS_IsUndefined(method)) {
        // Every pair is converted before any is appended.
        Array<PrStringView> pairs;
        bool ok = jsIterate(ctx, init, method, [&](JSValue item) {
            uint32_t count = 0;
            PrStringView pair[2] = {};
            bool read = jsSequence(ctx, item, [&](JSValue part) {
                PrStringView text;
                bool converted = readByteString(ctx, part, &text);
                JS_FreeValue(ctx, part);
                if (converted && count < 2)
                    pair[count] = text;
                ++count;
                return converted;
            });
            JS_FreeValue(ctx, item);
            if (read && count != 2) {
                JS_ThrowTypeError(ctx, "A header must be a pair of a name and a value");
                read = false;
            }
            if (read) {
                pairs.push(pair[0]);
                pairs.push(pair[1]);
            }
            return read;
        });
        JS_FreeValue(ctx, method);
        for (uint32_t i = 0; ok && i < pairs.count; i += 2)
            ok = headersAppend(ctx, headers, pairs.data[i], pairs.data[i + 1]);
        pairs.reset();
        return ok;
    }
    // webidl/#js-record
    JSPropertyEnum* properties = nullptr;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(ctx, &properties, &count, init, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        return false;
    bool ok = true;
    for (uint32_t i = 0; ok && i < count; ++i) {
        PrStringView name;
        PrStringView value;
        JSValue key = JS_AtomToValue(ctx, properties[i].atom);
        ok = readByteString(ctx, key, &name);
        JS_FreeValue(ctx, key);
        JSValue item = ok ? JS_GetProperty(ctx, init, properties[i].atom) : JS_UNDEFINED;
        ok = ok && !JS_IsException(item) && readByteString(ctx, item, &value);
        JS_FreeValue(ctx, item);
        ok = ok && headersAppend(ctx, headers, name, value);
    }
    JS_FreePropertyEnum(ctx, properties, count);
    return ok;
}

JSValue newHeaders(JSContext* ctx, JsHeaders* headers) {
    JSValue object = jsWrap(ctx, headersClass, JS_UNDEFINED, headers, true);
    if (JS_IsException(object))
        headersDestroy(headers);
    return object;
}

// fetch/#dom-headers
JSValue constructHeaders(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    JsHeaders* headers = headersCreate(Guard_None);
    if (jsArgGiven(argc, argv, 0) && !headersFill(ctx, headers, argv[0])) {
        headersDestroy(headers);
        return JS_EXCEPTION;
    }
    return jsConstructed(ctx, newTarget, newHeaders(ctx, headers));
}

enum {
    Method_Append,
    Method_Delete,
    Method_Get,
    Method_Has,
    Method_Set,
};
typedef uint8_t Method;

// The methods of Headers that take a name, selected by the magic.
JSValue nameMethod(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv, int magic) {
    JsHeaders* headers = headersOf(ctx, self);
    bool pair = magic == Method_Append || magic == Method_Set;
    if (headers == nullptr || !jsArgCount(ctx, argc, pair ? 2 : 1))
        return JS_EXCEPTION;
    PrStringView name;
    PrStringView value = "";
    if (!readByteString(ctx, argv[0], &name) || (pair && !readByteString(ctx, argv[1], &value)))
        return JS_EXCEPTION;
    switch (Method(magic)) {
        case Method_Append:
            return headersAppend(ctx, headers, name, value) ? JS_UNDEFINED : JS_EXCEPTION;
        case Method_Delete: {
            // fetch/#dom-headers-delete
            bool ignored = false;
            if (!validate(ctx, headers, name, "", &ignored))
                return JS_EXCEPTION;
            if (ignored)
                return JS_UNDEFINED;
            if (headers->guard == Guard_RequestNoCors && !prFetchIsNoCorsSafelistedRequestHeaderName(name) &&
                    !isPrivilegedNoCors(name))
                return JS_UNDEFINED;
            eraseHeaders(headers, name);
            if (headers->guard == Guard_RequestNoCors)
                eraseHeaders(headers, "range");
            return JS_UNDEFINED;
        }
        case Method_Get:
        case Method_Has: {
            if (!prFetchIsHeaderName(name))
                return JS_ThrowTypeError(ctx, "Invalid header name");
            if (magic == Method_Has)
                return JS_NewBool(ctx, findHeader(headers, name) != kArrayMissing);
            TextBuffer combined;
            JSValue result = combinedValue(headers, name, &combined) ? newByteString(ctx, textView(&combined)) : JS_NULL;
            textReset(&combined);
            return result;
        }
        case Method_Set: {
            // fetch/#dom-headers-set
            value = prFetchNormalizeHeaderValue(value);
            bool ignored = false;
            if (!validate(ctx, headers, name, value, &ignored))
                return JS_EXCEPTION;
            if (ignored || (headers->guard == Guard_RequestNoCors && !prFetchIsNoCorsSafelistedRequestHeader(name, value)))
                return JS_UNDEFINED;
            uint32_t first = findHeader(headers, name);
            if (first == kArrayMissing) {
                appendHeader(headers, name, value);
            } else {
                prStringReplace(&headers->values.data[first], value);
                eraseHeaders(headers, name, first + 1);
            }
            if (headers->guard == Guard_RequestNoCors)
                eraseHeaders(headers, "range");
            return JS_UNDEFINED;
        }
    }
    return JS_UNDEFINED;
}

// fetch/#dom-headers-getsetcookie
JSValue getSetCookie(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    JsHeaders* headers = headersOf(ctx, self);
    if (headers == nullptr)
        return JS_EXCEPTION;
    JSValue all = JS_NewArray(ctx);
    uint32_t count = 0;
    for (uint32_t i = findHeader(headers, "set-cookie"); i != kArrayMissing; i = findHeader(headers, "set-cookie", i + 1))
        JS_SetPropertyUint32(ctx, all, count++, newByteString(ctx, prStringView(&headers->values.data[i])));
    return all;
}

// A header of the sorted and combined list that headers iterate (fetch/#concept-header-list-sort-and-combine).
struct SortedHeader {
    PrStringView name;          // as one of the list's headers names it, which is lowered when it is read
    uint32_t first;             // the index of the first header of the name in the list
    uint32_t setCookie;         // for Set-Cookie, the index of the header itself, whose value stands alone
};

// Returns the [name, value] pair at the index argv[0] of the sorted and combined list, or undefined past the end.
JSValue entryAt(JSContext* ctx, JSValueConst self, int, JSValueConst* argv) {
    JsHeaders* headers = headersOf(ctx, self);
    uint32_t index = 0;
    if (headers == nullptr || JS_ToUint32(ctx, &index, argv[0]) < 0)
        return JS_EXCEPTION;
    Array<SortedHeader> sorted;
    for (uint32_t i = 0; i < headers->names.count; ++i) {
        PrStringView name = prStringView(&headers->names.data[i]);
        bool setCookie = asciiEqualsFold(name, "set-cookie");
        if (!setCookie && findHeader(headers, name) != i)
            continue;
        uint32_t at = 0;
        while (at < sorted.count && asciiCompareFold(sorted.data[at].name, name) <= 0)
            ++at;
        sorted.insert(at, SortedHeader{name, findHeader(headers, name), setCookie ? i : kArrayMissing});
    }
    JSValue result = JS_UNDEFINED;
    if (index < sorted.count) {
        const SortedHeader& entry = sorted.data[index];
        TextBuffer name;
        textAppend(&name, entry.name);
        textAsciiLower(&name);
        TextBuffer value;
        if (entry.setCookie != kArrayMissing)
            textAppend(&value, prStringView(&headers->values.data[entry.setCookie]));
        else
            combinedValue(headers, entry.name, &value);
        result = jsEntry(ctx, newByteString(ctx, textView(&name)), newByteString(ctx, textView(&value)));
        textReset(&name);
        textReset(&value);
    }
    sorted.reset();
    return result;
}

JSValue iterate(JSContext* ctx, JSValueConst self, int, JSValueConst*, int magic) {
    if (headersOf(ctx, self) == nullptr)
        return JS_EXCEPTION;
    return jsPairIterator(ctx, headersIteratorClass, self, entryAt, JsMapPart(magic));
}

JSValue forEachMethod(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    if (headersOf(ctx, self) == nullptr)
        return JS_EXCEPTION;
    return jsPairForEach(ctx, self, entryAt, argc, argv);
}

void headersFinalizer(JSRuntime* runtime, JSValue self) {
    jsFinalize<JsHeaders>(runtime, self, headersClass, headersDestroy);
}

void headersMark(JSRuntime* runtime, JSValueConst self, JS_MarkFunc* mark) {
    jsMark(runtime, self, headersClass, mark);
}

// ---------------------------------------------------------------------------------------------------------------------
// Bodies (fetch/#bodies)

// A body with its bytes (fetch/#concept-body), or the null body.
struct Body {
    Array<uint8_t> bytes;
    bool present;               // false for the null body
    bool used;                  // true once a read consumed it (fetch/#dom-body-bodyused)
};

// The type of a body that its source gives (fetch/#concept-bodyinit-extract), as a literal, or the null view.
struct ExtractedBody {
    PrByteView bytes;
    PrStringView type;
};

// fetch/#concept-bodyinit-extract, for the BodyInit kinds that the engine has: a BufferSource, URLSearchParams and a
// string. The call's scope holds the bytes.
bool extractBody(JSContext* ctx, JSValueConst init, ExtractedBody* out) {
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (jsBufferSource(ctx, init, &data, &size)) {
        *out = ExtractedBody{PrByteView{data, size}, PrStringView{nullptr, 0}};
        return true;
    }
    PrStringView text;
    if (PrUrlSearchParams* params = prJsUrlSearchParamsFromValue(ctx, init)) {
        PrString serialized = prUrlSearchParamsToString(params);
        text = prStringView(&serialized);
        uint8_t* bytes = static_cast<uint8_t*>(prMemoryAlloc(text.size != 0 ? text.size : 1));
        if (text.size != 0)
            memCopy(bytes, text.data, text.size);
        jsHoldBytes(ctx, bytes);
        *out = ExtractedBody{PrByteView{bytes, text.size}, "application/x-www-form-urlencoded;charset=UTF-8"};
        prStringReset(&serialized);
        return true;
    }
    if (!jsReadString(ctx, init, &text))
        return false;
    *out = ExtractedBody{PrByteView{stringBytes(text), text.size}, "text/plain;charset=UTF-8"};
    return true;
}

// Returns a promise rejected with the pending exception, which it clears.
JSValue rejectWithException(JSContext* ctx) {
    JSValue error = JS_GetException(ctx);
    JSValue promise = jsSettledPromise(ctx, error, true);
    JS_FreeValue(ctx, error);
    return promise;
}

enum {
    Read_Text,
    Read_Json,
    Read_ArrayBuffer,
    Read_Bytes,
};
typedef uint8_t Read;

// fetch/#concept-body-consume-body: settles a promise with the body's bytes as `read` packages them
// (fetch/#concept-body-package-data). A body that was used rejects with a TypeError.
JSValue consume(JSContext* ctx, Body* body, Read read) {
    if (body->used) {
        JS_ThrowTypeError(ctx, "The body has already been read");
        return rejectWithException(ctx);
    }
    if (body->present)
        body->used = true;
    const uint8_t* data = body->bytes.data != nullptr ? body->bytes.data : reinterpret_cast<const uint8_t*>("");
    PrStringView bytes{reinterpret_cast<const char*>(data), body->bytes.count};
    JSValue result = JS_UNDEFINED;
    switch (read) {
        case Read_ArrayBuffer:
            result = JS_NewArrayBufferCopy(ctx, stringBytes(bytes), bytes.size);
            break;
        case Read_Bytes:
            result = JS_NewUint8ArrayCopy(ctx, stringBytes(bytes), bytes.size);
            break;
        case Read_Text:
        case Read_Json: {
            // encoding/#utf-8-decode, which removes a byte order mark. JSON parsing needs a NUL after the text.
            PrCharsetDecoder decoder = prCharsetDecoderMake(PrCharset_Utf8, 0);
            PrString decoded = {};
            PrStringView text = prCharsetDecodeView(&decoder, bytes, PrCharsetInput_Final, &decoded);
            if (read == Read_Text) {
                result = JS_NewStringLen(ctx, text.data != nullptr ? text.data : "", text.size);
            } else {
                TextBuffer source;
                textAppend(&source, text);
                textAppendChar(&source, '\0');
                result = JS_ParseJSON(ctx, textView(&source).data, text.size, "<json>");
                textReset(&source);
            }
            prStringReset(&decoded);
            break;
        }
    }
    if (JS_IsException(result))
        return rejectWithException(ctx);
    JSValue promise = jsSettledPromise(ctx, result, false);
    JS_FreeValue(ctx, result);
    return promise;
}

// ---------------------------------------------------------------------------------------------------------------------
// Response (fetch/#response-class)

// A response (fetch/#concept-response). Its headers are the Headers object that the Response holds as `headers`.
struct JsResponse {
    PrFetchResponseType type;
    bool error;                 // a network error, whose type is "error"
    bool constructed;           // made by script, whose type is "default"
    uint16_t status;
    PrString statusText;
    PrString url;               // the null string for a response without a URL
    Body body;
};

void responseDestroy(JsResponse* response) {
    prStringReset(&response->statusText);
    prStringReset(&response->url);
    response->body.bytes.reset();
    prMemoryFree(response);
}

JsResponse* PR_NULLABLE responseOf(JSContext* ctx, JSValueConst self) {
    return jsValueOf<JsResponse>(ctx, self, responseClass, "Response");
}

// Returns a Response object of `response`, with `headers` as its Headers. Takes ownership of both.
JSValue newResponse(JSContext* ctx, JsResponse* response, JsHeaders* headers) {
    JSValue object = jsWrap(ctx, responseClass, JS_UNDEFINED, response, true);
    if (JS_IsException(object)) {
        responseDestroy(response);
        headersDestroy(headers);
        return object;
    }
    JSValue held = newHeaders(ctx, headers);
    if (JS_IsException(held)) {
        JS_FreeValue(ctx, object);
        return held;
    }
    jsSetSameObject(ctx, object, "headers", held);
    JS_FreeValue(ctx, held);
    return object;
}

JsHeaders* headersOfResponse(JSContext* ctx, JSValueConst self) {
    JSValue held = jsSameObject(ctx, self, "headers");
    JsHeaders* headers = jsHandleAs<JsHeaders>(ctx, held, headersClass);
    JS_FreeValue(ctx, held);
    return headers;
}

// Returns true if `status` is a null body status (fetch/#null-body-status).
bool isNullBodyStatus(uint16_t status) {
    return status == 101 || status == 103 || status == 204 || status == 205 || status == 304;
}

// Returns true if `text` matches HTTP's reason-phrase: tabs, spaces, visible ASCII and obs-text (rfc 9112, 4).
bool isReasonPhrase(PrStringView text) {
    for (size_t i = 0; i < text.size; ++i) {
        uint8_t c = uint8_t(text.data[i]);
        if (c != '\t' && c != ' ' && (c < 0x21 || c == 0x7f))
            return false;
    }
    return true;
}

// Reads member `name` of a dictionary, which the call's scope holds. Returns false if reading it threw.
bool readMember(JSContext* ctx, JSValueConst dictionary, const char* name, JSValue* out) {
    *out = JS_GetPropertyStr(ctx, dictionary, name);
    if (JS_IsException(*out))
        return false;
    JsHeld held = {};
    held.kind = JsHeldKind_Value;
    held.value = *out;
    jsHold(ctx, held);
    return true;
}

// fetch/#initialize-a-response, with `body` extracted from the constructor's body, or null for the null body.
bool initializeResponse(JSContext* ctx, JsResponse* response, JsHeaders* headers, JSValueConst init,
        const ExtractedBody* PR_NULLABLE body) {
    bool present = false;
    if (!jsDictionary(ctx, init, &present))
        return false;
    uint32_t status = 200;
    PrStringView statusText = "";
    JSValue headersInit = JS_UNDEFINED;
    if (present) {
        // The members in lexicographic order: headers, status, statusText. The status converts as an unsigned short
        // does (webidl/#js-unsigned-short), modulo 2^16.
        JSValue value;
        if (!readMember(ctx, init, "headers", &headersInit) || !readMember(ctx, init, "status", &value))
            return false;
        if (!JS_IsUndefined(value)) {
            if (JS_ToUint32(ctx, &status, value) < 0)
                return false;
            status &= 0xffff;
        }
        if (!readMember(ctx, init, "statusText", &value))
            return false;
        if (!JS_IsUndefined(value) && !readByteString(ctx, value, &statusText))
            return false;
    }
    if (status < 200 || status > 599) {
        JS_ThrowRangeError(ctx, "The status must be in the range 200 to 599");
        return false;
    }
    if (!isReasonPhrase(statusText)) {
        JS_ThrowTypeError(ctx, "Invalid statusText");
        return false;
    }
    response->status = uint16_t(status);
    response->statusText = prStringCopy(statusText);
    if (!JS_IsUndefined(headersInit) && !headersFill(ctx, headers, headersInit))
        return false;
    if (body != nullptr) {
        if (isNullBodyStatus(response->status)) {
            JS_ThrowTypeError(ctx, "A response with this status cannot have a body");
            return false;
        }
        response->body.present = true;
        if (body->bytes.size != 0)
            response->body.bytes.append(body->bytes.data, uint32_t(body->bytes.size));
        if (body->type.data != nullptr && findHeader(headers, "content-type") == kArrayMissing)
            appendHeader(headers, "Content-Type", body->type);
    }
    return true;
}

// fetch/#dom-response
JSValue constructResponse(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    ExtractedBody body = {};
    bool hasBody = argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0]);
    if (hasBody && !extractBody(ctx, argv[0], &body))
        return JS_EXCEPTION;
    JsResponse* response = allocZeroed<JsResponse>();
    response->constructed = true;
    JsHeaders* headers = headersCreate(Guard_Response);
    if (!initializeResponse(ctx, response, headers, argc > 1 ? argv[1] : JS_UNDEFINED, hasBody ? &body : nullptr)) {
        responseDestroy(response);
        headersDestroy(headers);
        return JS_EXCEPTION;
    }
    return jsConstructed(ctx, newTarget, newResponse(ctx, response, headers));
}

// fetch/#dom-response-error
JSValue errorStatic(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JsResponse* response = allocZeroed<JsResponse>();
    response->error = true;
    return newResponse(ctx, response, headersCreate(Guard_Immutable));
}

// fetch/#dom-response-json
JSValue jsonStatic(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    JSValue text = JS_JSONStringify(ctx, argv[0], JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(text))
        return text;
    if (JS_IsUndefined(text))
        return JS_ThrowTypeError(ctx, "The value does not serialize as JSON");
    PrStringView serialized;
    bool read = jsReadString(ctx, text, &serialized);
    JS_FreeValue(ctx, text);
    if (!read)
        return JS_EXCEPTION;
    ExtractedBody body{PrByteView{stringBytes(serialized), serialized.size}, "application/json"};
    JsResponse* response = allocZeroed<JsResponse>();
    response->constructed = true;
    JsHeaders* headers = headersCreate(Guard_Response);
    if (!initializeResponse(ctx, response, headers, argc > 1 ? argv[1] : JS_UNDEFINED, &body)) {
        responseDestroy(response);
        headersDestroy(headers);
        return JS_EXCEPTION;
    }
    return newResponse(ctx, response, headers);
}

// fetch/#dom-response-type
JSValue typeGet(JSContext* ctx, JSValueConst self) {
    JsResponse* response = responseOf(ctx, self);
    if (response == nullptr)
        return JS_EXCEPTION;
    if (response->error)
        return jsFixedString(ctx, "error");
    if (response->constructed)
        return jsFixedString(ctx, "default");
    switch (response->type) {
        case PrFetchResponseType_Basic: return jsFixedString(ctx, "basic");
        case PrFetchResponseType_Cors: return jsFixedString(ctx, "cors");
        case PrFetchResponseType_Opaque: return jsFixedString(ctx, "opaque");
        case PrFetchResponseType_OpaqueRedirect: return jsFixedString(ctx, "opaqueredirect");
    }
    return jsFixedString(ctx, "basic");
}

JSValue urlGet(JSContext* ctx, JSValueConst self) {
    JsResponse* response = responseOf(ctx, self);
    if (response == nullptr)
        return JS_EXCEPTION;
    PrStringView url = prStringView(&response->url);
    return JS_NewStringLen(ctx, url.data != nullptr ? url.data : "", url.size);
}

JSValue statusGet(JSContext* ctx, JSValueConst self) {
    JsResponse* response = responseOf(ctx, self);
    return response != nullptr ? JS_NewUint32(ctx, response->status) : JS_EXCEPTION;
}

// fetch/#dom-response-ok
JSValue okGet(JSContext* ctx, JSValueConst self) {
    JsResponse* response = responseOf(ctx, self);
    return response != nullptr ? JS_NewBool(ctx, response->status >= 200 && response->status <= 299) : JS_EXCEPTION;
}

JSValue statusTextGet(JSContext* ctx, JSValueConst self) {
    JsResponse* response = responseOf(ctx, self);
    return response != nullptr ? newByteString(ctx, prStringView(&response->statusText)) : JS_EXCEPTION;
}

JSValue headersGet(JSContext* ctx, JSValueConst self) {
    if (responseOf(ctx, self) == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, self, "headers");
}

JSValue bodyUsedGet(JSContext* ctx, JSValueConst self) {
    JsResponse* response = responseOf(ctx, self);
    return response != nullptr ? JS_NewBool(ctx, response->body.used) : JS_EXCEPTION;
}

// text(), json(), arrayBuffer() and bytes(), selected by the magic. A body that its signal errored rejects with the
// signal's reason, however often it is read.
JSValue readMethod(JSContext* ctx, JSValueConst self, int, JSValueConst*, int magic) {
    JsResponse* response = responseOf(ctx, self);
    if (response == nullptr)
        return rejectWithException(ctx);
    JSValue signal = jsSameObject(ctx, self, "signal");
    JSValue reason = prJsDomAbortSignalReason(ctx, signal);
    JS_FreeValue(ctx, signal);
    if (!JS_IsUndefined(reason) && response->body.present) {
        JSValue promise = jsSettledPromise(ctx, reason, true);
        JS_FreeValue(ctx, reason);
        return promise;
    }
    JS_FreeValue(ctx, reason);
    return consume(ctx, &response->body, Read(magic));
}

// fetch/#dom-response-clone
JSValue cloneMethod(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    JsResponse* response = responseOf(ctx, self);
    if (response == nullptr)
        return JS_EXCEPTION;
    if (response->body.used)
        return JS_ThrowTypeError(ctx, "The body has already been read");
    JsHeaders* original = headersOfResponse(ctx, self);
    JsHeaders* headers = headersCreate(original->guard);
    for (uint32_t i = 0; i < original->names.count; ++i) {
        headers->names.push(prStringCopy(prStringView(&original->names.data[i])));
        headers->values.push(prStringCopy(prStringView(&original->values.data[i])));
    }
    JsResponse* clone = allocZeroed<JsResponse>();
    *clone = JsResponse{
        .type = response->type,
        .error = response->error,
        .constructed = response->constructed,
        .status = response->status,
        .statusText = prStringCopy(prStringView(&response->statusText)),
        .url = prStringCopy(prStringView(&response->url)),
    };
    clone->body.present = response->body.present;
    if (response->body.bytes.count != 0)
        clone->body.bytes.append(response->body.bytes.data, response->body.bytes.count);
    // The clone's body is a branch of the same stream (fetch/#concept-body-clone), which the same abort errors.
    JSValue object = newResponse(ctx, clone, headers);
    JSValue signal = jsSameObject(ctx, self, "signal");
    if (!JS_IsException(object) && !JS_IsUndefined(signal))
        jsSetSameObject(ctx, object, "signal", signal);
    JS_FreeValue(ctx, signal);
    return object;
}

void responseFinalizer(JSRuntime* runtime, JSValue self) {
    jsFinalize<JsResponse>(runtime, self, responseClass, responseDestroy);
}

void responseMark(JSRuntime* runtime, JSValueConst self, JS_MarkFunc* mark) {
    jsMark(runtime, self, responseClass, mark);
}

// ---------------------------------------------------------------------------------------------------------------------
// The request of fetch()

// Reads the enumeration member `name` of a RequestInit, given as one of `values`, into `*out` as its index. Leaves
// `*out` unchanged if the member is absent. Throws a TypeError for a value that is not one of them.
template <size_t N>
bool readEnum(JSContext* ctx, JSValueConst init, const char* name, const PrStringView (&values)[N], uint32_t* out) {
    JSValue value;
    if (!readMember(ctx, init, name, &value))
        return false;
    if (JS_IsUndefined(value))
        return true;
    PrStringView text;
    if (!jsReadString(ctx, value, &text))
        return false;
    for (uint32_t i = 0; i < N; ++i) {
        if (text == values[i]) {
            *out = i;
            return true;
        }
    }
    JS_ThrowTypeError(ctx, "'%.*s' is not a valid value of %s", int(text.size), text.data, name);
    return false;
}

constexpr PrStringView kModes[] = {"navigate", "same-origin", "no-cors", "cors"};
constexpr PrStringView kCredentials[] = {"omit", "same-origin", "include"};
constexpr PrStringView kRedirects[] = {"follow", "error", "manual"};

constexpr JSCFunctionListEntry kHeadersProto[] = {
    PR_JS_TAG("Headers"),
    PR_JS_MAGIC("append", 2, nameMethod, Method_Append),
    PR_JS_MAGIC("delete", 1, nameMethod, Method_Delete),
    PR_JS_MAGIC("get", 1, nameMethod, Method_Get),
    PR_JS_METHOD("getSetCookie", 0, getSetCookie),
    PR_JS_MAGIC("has", 1, nameMethod, Method_Has),
    PR_JS_MAGIC("set", 2, nameMethod, Method_Set),
    PR_JS_MAGIC("entries", 0, iterate, JsMapPart_Entries),
    PR_JS_MAGIC("keys", 0, iterate, JsMapPart_Keys),
    PR_JS_MAGIC("values", 0, iterate, JsMapPart_Values),
    PR_JS_METHOD("forEach", 1, forEachMethod),
};

constexpr JSCFunctionListEntry kResponseProto[] = {
    PR_JS_TAG("Response"),
    PR_JS_GET("type", typeGet),
    PR_JS_GET("url", urlGet),
    PR_JS_GET("status", statusGet),
    PR_JS_GET("ok", okGet),
    PR_JS_GET("statusText", statusTextGet),
    PR_JS_GET("headers", headersGet),
    PR_JS_METHOD("clone", 0, cloneMethod),
    PR_JS_GET("bodyUsed", bodyUsedGet),
    PR_JS_MAGIC("arrayBuffer", 0, readMethod, Read_ArrayBuffer),
    PR_JS_MAGIC("bytes", 0, readMethod, Read_Bytes),
    PR_JS_MAGIC("json", 0, readMethod, Read_Json),
    PR_JS_MAGIC("text", 0, readMethod, Read_Text),
};

constexpr JSCFunctionListEntry kResponseStatic[] = {
    PR_JS_METHOD("error", 0, errorStatic),
    PR_JS_METHOD("json", 1, jsonStatic),
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

void prJsFetchBind(JSContext* ctx) {
    JSValue headers = jsDefineClass(ctx, {
        .name = "Headers",
        .id = &headersClass,
        .finalizer = headersFinalizer,
        .mark = headersMark,
        .proto = kHeadersProto,
        .protoCount = int(countOf(kHeadersProto)),
        .constructor = constructHeaders,
        .iterator = "entries",
    });
    JS_FreeValue(ctx, headers);
    jsPairIteratorBind(ctx, &headersIteratorClass, "Headers Iterator");
    JSValue response = jsDefineClass(ctx, {
        .name = "Response",
        .id = &responseClass,
        .finalizer = responseFinalizer,
        .mark = responseMark,
        .proto = kResponseProto,
        .protoCount = int(countOf(kResponseProto)),
        .statics = kResponseStatic,
        .staticCount = int(countOf(kResponseStatic)),
        .constructor = constructResponse,
    });
    JS_FreeValue(ctx, response);
}

bool prJsFetchRequestRead(JSContext* ctx, JSValueConst input, JSValueConst init, PrStringView baseUrl, PrJsFetchRequest* out) {
    *out = PrJsFetchRequest{
        .mode = PrFetchMode_Cors,
        .credentials = PrFetchCredentials_SameOrigin,
        .signal = JS_UNDEFINED,
    };
    PrStringView given;
    if (!jsReadString(ctx, input, &given))
        return false;
    // The URL parses before init is read, so that its TypeError comes first.
    PrUrl* base = nullptr;
    PrUrl* parsed = nullptr;
    bool valid = prUrlCreate(baseUrl, nullptr, PrCharset_Utf8, &base) == PrStatus_Ok &&
        prUrlCreate(given, base, PrCharset_Utf8, &parsed) == PrStatus_Ok;
    bool userinfo = valid && (prUrlUsername(parsed).size != 0 || prUrlPassword(parsed).size != 0);
    if (valid && !userinfo)
        out->parsedUrl = prUrlSerialize(parsed, PrUrlSerialization_Whole);
    prUrlDestroy(parsed);
    prUrlDestroy(base);
    if (!valid || userinfo) {
        JS_ThrowTypeError(ctx, valid ? "A request's URL cannot have credentials" : "Invalid URL");
        return false;
    }
    out->url = prStringView(&out->parsedUrl);
    bool present = false;
    if (!jsDictionary(ctx, init, &present))
        return false;
    JsHeaders* headers = headersCreate(Guard_Request);
    ExtractedBody body = {};
    bool hasBody = false;
    bool ok = true;
    if (present) {
        // The members in lexicographic order: body, credentials, headers, method, mode, redirect, signal. The call's
        // scope holds each member's value, so that a buffer that a body names outlives the conversion.
        JSValue value;
        ok = readMember(ctx, init, "body", &value);
        hasBody = ok && !JS_IsUndefined(value) && !JS_IsNull(value);
        ok = ok && (!hasBody || extractBody(ctx, value, &body));
        uint32_t credentials = 1;
        ok = ok && readEnum(ctx, init, "credentials", kCredentials, &credentials);
        constexpr PrFetchCredentials kCredentialsOf[] = {
            PrFetchCredentials_Omit, PrFetchCredentials_SameOrigin, PrFetchCredentials_Include,
        };
        out->credentials = kCredentialsOf[credentials];
        ok = ok && readMember(ctx, init, "headers", &value) && (JS_IsUndefined(value) || headersFill(ctx, headers, value));
        ok = ok && readMember(ctx, init, "method", &value);
        if (ok && !JS_IsUndefined(value)) {
            PrStringView method;
            ok = readByteString(ctx, value, &method);
            if (ok && !prFetchIsMethod(method)) {
                JS_ThrowTypeError(ctx, "Invalid method");
                ok = false;
            } else if (ok && prFetchIsForbiddenMethod(method)) {
                JS_ThrowTypeError(ctx, "Forbidden method");
                ok = false;
            }
            if (ok)
                out->method = prFetchNormalizeMethod(method);
        }
        uint32_t mode = 3;
        ok = ok && readEnum(ctx, init, "mode", kModes, &mode);
        if (ok && mode == 0) {
            JS_ThrowTypeError(ctx, "A request's mode cannot be navigate");
            ok = false;
        }
        constexpr PrFetchMode kModeOf[] = {
            PrFetchMode_Navigate, PrFetchMode_SameOrigin, PrFetchMode_NoCors,
            PrFetchMode_Cors,
        };
        out->mode = kModeOf[mode];
        uint32_t redirect = 0;
        ok = ok && readEnum(ctx, init, "redirect", kRedirects, &redirect);
        constexpr PrFetchRedirect kRedirectOf[] = {
            PrFetchRedirect_Follow, PrFetchRedirect_Error,
            PrFetchRedirect_Manual,
        };
        out->redirect = kRedirectOf[redirect];
        ok = ok && readMember(ctx, init, "signal", &value);
        if (ok && !JS_IsUndefined(value) && !JS_IsNull(value)) {
            ok = jsValueOf<PrDomAbortSignal>(ctx, value, prJsDomAbortSignalClass, "an AbortSignal") != nullptr;
            out->signal = value;
        }
    }
    PrStringView method = out->method.data != nullptr ? out->method : PrStringView{"GET"};
    if (ok && out->mode == PrFetchMode_NoCors) {
        // fetch/#dom-request: a no-cors request sends only safelisted methods and headers.
        if (!prFetchIsCorsSafelistedMethod(method)) {
            JS_ThrowTypeError(ctx, "A no-cors request's method must be GET, HEAD or POST");
            ok = false;
        }
        for (uint32_t i = 0; ok && i < headers->names.count;) {
            PrStringView name = prStringView(&headers->names.data[i]);
            if (prFetchIsNoCorsSafelistedRequestHeader(name, prStringView(&headers->values.data[i]))) {
                ++i;
                continue;
            }
            prStringReset(&headers->names.data[i]);
            prStringReset(&headers->values.data[i]);
            headers->names.erase(i);
            headers->values.erase(i);
        }
    }
    if (ok && hasBody && (method == "GET" || method == "HEAD")) {
        JS_ThrowTypeError(ctx, "A GET or HEAD request cannot have a body");
        ok = false;
    }
    if (ok && hasBody && body.type.data != nullptr && findHeader(headers, "content-type") == kArrayMissing)
        appendHeader(headers, "Content-Type", body.type);
    if (ok) {
        // The headers and the body are copied into one block, which the views then point into.
        uint32_t size = uint32_t(body.bytes.size);
        for (uint32_t i = 0; i < headers->names.count; ++i)
            size += prStringView(&headers->names.data[i]).size + prStringView(&headers->values.data[i]).size;
        out->bytes = size != 0 ? static_cast<uint8_t*>(prMemoryAlloc(size)) : nullptr;
        uint32_t used = 0;
        auto copy = [&](PrStringView text) {
            uint32_t at = used;
            if (text.size != 0)
                memCopy(out->bytes + at, stringBytes(text), text.size);
            used += uint32_t(text.size);
            return PrStringView{reinterpret_cast<const char*>(out->bytes) + at, text.size};
        };
        out->headerCount = headers->names.count;
        if (out->headerCount != 0)
            out->headers = static_cast<PrFetchHeader*>(prMemoryAlloc(out->headerCount * sizeof(PrFetchHeader)));
        else
            out->headers = nullptr;
        for (uint32_t i = 0; i < headers->names.count; ++i) {
            PrStringView name = copy(prStringView(&headers->names.data[i]));
            out->headers[i] = PrFetchHeader{name, copy(prStringView(&headers->values.data[i]))};
        }
        PrStringView bytes = copy(PrStringView{reinterpret_cast<const char*>(body.bytes.data), body.bytes.size});
        out->body = PrByteView{stringBytes(bytes), bytes.size};
    }
    headersDestroy(headers);
    return ok;
}

void prJsFetchRequestReset(PrJsFetchRequest* request) {
    prStringReset(&request->parsedUrl);
    prMemoryFree(request->headers);
    prMemoryFree(request->bytes);
}

JSValue prJsFetchResponseNew(JSContext* ctx, const PrJsFetchResponseDesc* desc) {
    JsHeaders* headers = headersCreate(Guard_Immutable);
    for (uint32_t i = 0; i < desc->headerCount; ++i) {
        headers->names.push(prStringCopy(desc->headers[i].name));
        headers->values.push(prStringCopy(desc->headers[i].value));
    }
    JsResponse* response = allocZeroed<JsResponse>();
    response->type = desc->type;
    response->status = desc->status;
    if (desc->url.data != nullptr)
        response->url = prStringCopy(desc->url);
    response->body.present = desc->type != PrFetchResponseType_Opaque &&
        desc->type != PrFetchResponseType_OpaqueRedirect && !isNullBodyStatus(desc->status);
    if (desc->body.size != 0)
        response->body.bytes.append(desc->body.data, uint32_t(desc->body.size));
    JSValue object = newResponse(ctx, response, headers);
    if (!JS_IsException(object) && !JS_IsUndefined(desc->signal))
        jsSetSameObject(ctx, object, "signal", desc->signal);
    return object;
}
