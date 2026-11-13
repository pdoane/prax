// The HTTP cache's rules (rfc 9111): what a response may be stored as, how long it stays fresh, and the record that a
// store keeps for it.

#include "fetch/backends/builtin/src/fetch_cache.h"

#include "pr/ascii.h"
#include "pr/date_time.h"
#include "pr/mem.h"
#include "pr/string_ops.h"
#include "pr/string_trim.h"

namespace pr_impl {

namespace {

// The start of a record, and the version of its layout, which is in the machine's byte order.
constexpr uint8_t kRecordMagic[4] = {'P', 'R', 'H', 'C'};
constexpr uint32_t kRecordVersion = 2;

// The size that a record writes for a header value that is the null view.
constexpr uint32_t kNullValue = 0xffffffffu;

// The fraction of the time since a response's Last-Modified that it stays fresh by heuristic (rfc 9111 section 4.2.2).
constexpr double kHeuristicFraction = 0.1;

// Reads an HTTP-date (rfc 9110 section 5.6.7) into `out`. Returns false if `text` is not one.
bool parseDate(PrStringView text, PrTimeSystem* out) {
    PrDateTime dateTime{};
    if (!prDateTimeParse(text, PrDateTimeFormat_Http, &dateTime))
        return false;
    *out = prDateTimeToSystem(dateTime);
    return true;
}

// Calls `fn` with each element of the comma-separated list `value`, trimmed.
template <typename Fn>
void eachElement(PrStringView value, Fn fn) {
    size_t start = 0;
    while (start <= value.size) {
        size_t end = start;
        bool quoted = false;
        while (end < value.size && (quoted || value.data[end] != ',')) {
            if (value.data[end] == '"')
                quoted = !quoted;
            ++end;
        }
        PrStringView element = stringTrimBy(prStringSlice(value, start, end - start), prAsciiIsTabOrSpace);
        if (element.size != 0)
            fn(element);
        start = end + 1;
    }
}

// Parses a non-negative integer of seconds (rfc 9111 section 1.2.2). Returns false if `text` is not one.
bool parseSeconds(PrStringView text, double* out) {
    if (text.size >= 2 && text.data[0] == '"' && text.data[text.size - 1] == '"')
        text = prStringSlice(text, 1, text.size - 2);
    if (text.size == 0)
        return false;
    double seconds = 0;
    for (size_t i = 0; i < text.size; ++i) {
        if (!prAsciiIsDigit(text.data[i]))
            return false;
        seconds = seconds * 10 + double(text.data[i] - '0');
    }
    *out = seconds;
    return true;
}

void appendU32(PrTextBuffer* out, uint32_t value) {
    prTextBufferAppend(out, PrStringView{reinterpret_cast<const char*>(&value), 4});
}

void appendText(PrTextBuffer* out, PrStringView text) {
    appendU32(out, text.data != nullptr ? uint32_t(text.size) : kNullValue);
    prTextBufferAppend(out, text);
}

// Reads the fields of a record, in order, from a range of bytes.
struct Reader {
    const uint8_t* at;
    const uint8_t* end;

    bool take(void* out, size_t size) {
        if (size_t(end - at) < size)
            return false;
        prMemCopy(out, at, size);
        at += size;
        return true;
    }

    bool text(PrStringView* out) {
        uint32_t size = 0;
        if (!take(&size, 4))
            return false;
        if (size == kNullValue) {
            *out = PrStringView{nullptr, 0};
            return true;
        }
        if (size_t(end - at) < size)
            return false;
        *out = PrStringView{reinterpret_cast<const char*>(at), size};
        at += size;
        return true;
    }
};

} // namespace

FetchCacheControl fetchCacheControlFromHeaders(const PrFetchHeader* PR_NULLABLE headers, uint32_t count) {
    FetchCacheControl control{};
    for (uint32_t i = 0; i < count; ++i) {
        if (prAsciiEqualsFold(headers[i].name, "pragma")) {
            eachElement(headers[i].value, [&](PrStringView element) {
                control.noCache = control.noCache || prAsciiEqualsFold(element, "no-cache");
            });
            continue;
        }
        if (!prAsciiEqualsFold(headers[i].name, "cache-control"))
            continue;
        eachElement(headers[i].value, [&](PrStringView element) {
            size_t equals = prStringFindByte(element, '=');
            PrStringView name = stringTrimBy(prStringSlice(element, 0, equals), prAsciiIsTabOrSpace);
            PrStringView value = {nullptr, 0};
            if (equals < element.size) {
                PrStringView afterEquals = prStringSlice(element, equals + 1, element.size - equals - 1);
                value = stringTrimBy(afterEquals, prAsciiIsTabOrSpace);
            }
            if (prAsciiEqualsFold(name, "no-store")) {
                control.noStore = true;
            } else if (prAsciiEqualsFold(name, "no-cache")) {
                control.noCache = true;
            } else if (prAsciiEqualsFold(name, "max-age")) {
                double seconds = 0;
                // An invalid max-age makes the response stale (rfc 9111 section 4.2.1).
                control.hasMaxAge = true;
                control.maxAge = parseSeconds(value, &seconds) ? seconds : 0;
            }
        });
    }
    return control;
}

PrStringView fetchCacheHeader(const PrFetchHeader* PR_NULLABLE headers, uint32_t count, PrStringView name) {
    for (uint32_t i = 0; i < count; ++i) {
        if (prAsciiEqualsFold(headers[i].name, name))
            return headers[i].value;
    }
    return PrStringView{nullptr, 0};
}

bool fetchCacheIsStorable(uint16_t status, const PrFetchHeader* PR_NULLABLE headers, uint32_t count) {
    // Partial content is not asked for, and interim responses are never final.
    if (status < 200 || status == 206)
        return false;
    FetchCacheControl control = fetchCacheControlFromHeaders(headers, count);
    if (control.noStore)
        return false;
    bool everything = false;
    eachElement(fetchCacheHeader(headers, count, "vary"), [&](PrStringView element) {
        everything = everything || element == "*";
    });
    if (everything)
        return false;
    // A response is storable with explicit freshness, or with a status that is heuristically cacheable (rfc 9110
    // section 15.1).
    bool heuristic = status == 200 || status == 203 || status == 204 || status == 300 || status == 301 || status ==
        308 || status == 404 || status == 405 || status == 410 || status == 414 || status == 501;
    return heuristic || control.hasMaxAge || fetchCacheHeader(headers, count, "expires").data != nullptr;
}

double fetchCacheLifetime(const FetchCachedResponse* response) {
    const PrFetchHeader* headers = response->headers.data;
    uint32_t count = response->headers.count;
    FetchCacheControl control = fetchCacheControlFromHeaders(headers, count);
    if (control.hasMaxAge)
        return control.maxAge * 1000;
    PrTimeSystem date = response->received;
    parseDate(fetchCacheHeader(headers, count, "date"), &date);
    PrStringView expires = fetchCacheHeader(headers, count, "expires");
    if (expires.data != nullptr) {
        PrTimeSystem when{};
        // An Expires that is not a date is in the past (rfc 9111 section 5.3).
        return parseDate(expires, &when) && when.ticks > date.ticks ? prTimeSystemMilliseconds(date, when) : 0;
    }
    PrTimeSystem modified{};
    PrStringView lastModified = fetchCacheHeader(headers, count, "last-modified");
    if (parseDate(lastModified, &modified) && modified.ticks < date.ticks)
        return prTimeSystemMilliseconds(modified, date) * kHeuristicFraction;
    return 0;
}

double fetchCacheAge(const FetchCachedResponse* response, PrTimeSystem now) {
    const PrFetchHeader* headers = response->headers.data;
    uint32_t count = response->headers.count;
    PrTimeSystem received = response->received;
    PrTimeSystem date = received;
    parseDate(fetchCacheHeader(headers, count, "date"), &date);
    double apparent = received.ticks > date.ticks ? prTimeSystemMilliseconds(date, received) : 0;
    double ageSeconds = 0;
    parseSeconds(stringTrimBy(fetchCacheHeader(headers, count, "age"), prAsciiIsTabOrSpace), &ageSeconds);
    double corrected = ageSeconds * 1000 + prTimeSystemMilliseconds(response->requested, received);
    double initial = apparent > corrected ? apparent : corrected;
    double resident = now.ticks > received.ticks ? prTimeSystemMilliseconds(received, now) : 0;
    return initial + resident;
}

bool fetchCacheVaryMatches(const FetchCachedResponse* response, const PrFetchHeader* PR_NULLABLE request, uint32_t count) {
    for (uint32_t i = 0; i < response->varied.count; ++i) {
        const PrFetchHeader& varied = response->varied.data[i];
        if (varied.name == "*")
            return false;
        PrStringView value = fetchCacheHeader(request, count, varied.name);
        if ((value.data == nullptr) != (varied.value.data == nullptr))
            return false;
        if (value.data != nullptr && stringTrimBy(value,
                prAsciiIsTabOrSpace) != stringTrimBy(varied.value, prAsciiIsTabOrSpace))
            return false;
    }
    return true;
}

void fetchCacheWrite(const FetchCachedResponse* response, PrTextBuffer* out) {
    prTextBufferAppend(out, PrStringView{reinterpret_cast<const char*>(kRecordMagic), 4});
    appendU32(out, kRecordVersion);
    appendU32(out, response->status);
    prTextBufferAppend(out, PrStringView{reinterpret_cast<const char*>(&response->requested), 8});
    prTextBufferAppend(out, PrStringView{reinterpret_cast<const char*>(&response->received), 8});
    appendU32(out, response->headers.count);
    for (uint32_t i = 0; i < response->headers.count; ++i) {
        appendText(out, response->headers.data[i].name);
        appendText(out, response->headers.data[i].value);
    }
    appendU32(out, response->varied.count);
    for (uint32_t i = 0; i < response->varied.count; ++i) {
        appendText(out, response->varied.data[i].name);
        appendText(out, response->varied.data[i].value);
    }
    uint64_t size = response->body.size;
    prTextBufferAppend(out, PrStringView{reinterpret_cast<const char*>(&size), 8});
    const char* body = reinterpret_cast<const char*>(response->body.data);
    prTextBufferAppend(out, PrStringView{body, response->body.size});
}

bool fetchCacheRead(PrByteView bytes, FetchCachedResponse* out) {
    Reader reader{bytes.data, bytes.data + bytes.size};
    uint8_t magic[4];
    uint32_t version = 0;
    uint32_t status = 0;
    uint32_t headers = 0;
    if (!reader.take(magic, 4) || !prMemEquals(magic, kRecordMagic, 4) || !reader.take(&version, 4) ||
            version != kRecordVersion || !reader.take(&status, 4) || !reader.take(&out->requested, 8) ||
            !reader.take(&out->received, 8) || !reader.take(&headers, 4))
        return false;
    out->status = uint16_t(status);
    for (uint32_t i = 0; i < headers; ++i) {
        PrFetchHeader header{};
        if (!reader.text(&header.name) || !reader.text(&header.value))
            return false;
        out->headers.push(header);
    }
    uint32_t varied = 0;
    if (!reader.take(&varied, 4))
        return false;
    for (uint32_t i = 0; i < varied; ++i) {
        PrFetchHeader header{};
        if (!reader.text(&header.name) || !reader.text(&header.value))
            return false;
        out->varied.push(header);
    }
    uint64_t size = 0;
    if (!reader.take(&size, 8) || uint64_t(reader.end - reader.at) != size)
        return false;
    out->body = PrByteView{reader.at, size_t(size)};
    return true;
}

void fetchCachedResponseReset(FetchCachedResponse* response) {
    response->headers.reset();
    response->varied.reset();
}

} // namespace pr_impl
