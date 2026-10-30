// The HTTP cache's rules (rfc 9111): what a response may be stored as, how long it stays fresh, and the record that a
// store keeps for it.

#include "fetch/backends/builtin/src/fetch_cache.h"

#include "pr/ascii.h"
#include "pr/mem.h"
#include "pr/string_ops.h"

namespace pr_impl {

namespace {

// The start of a record, and the version of its layout, which is in the machine's byte order.
constexpr uint8_t kRecordMagic[4] = {'P', 'R', 'H', 'C'};
constexpr uint32_t kRecordVersion = 1;

// The size that a record writes for a header value that is the null view.
constexpr uint32_t kNullValue = 0xffffffffu;

// The fraction of the time since a response's Last-Modified that it stays fresh by heuristic (rfc 9111 section 4.2.2).
constexpr double kHeuristicFraction = 0.1;

constexpr const char* kMonths[12] = {
    "jan", "feb", "mar", "apr", "may", "jun",
    "jul", "aug", "sep", "oct", "nov", "dec",
};

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
        PrStringView element = stringTrim(stringSlice(value, start, end - start), asciiIsTabOrSpace);
        if (element.size != 0)
            fn(element);
        start = end + 1;
    }
}

// Parses a non-negative integer of seconds (rfc 9111 section 1.2.2). Returns false if `text` is not one.
bool parseSeconds(PrStringView text, double* out) {
    if (text.size >= 2 && text.data[0] == '"' && text.data[text.size - 1] == '"')
        text = stringSlice(text, 1, text.size - 2);
    if (text.size == 0)
        return false;
    double seconds = 0;
    for (size_t i = 0; i < text.size; ++i) {
        if (!asciiIsDigit(text.data[i]))
            return false;
        seconds = seconds * 10 + double(text.data[i] - '0');
    }
    *out = seconds;
    return true;
}

// Returns the number of days from 1970-01-01 to the date in the proleptic Gregorian calendar.
int64_t daysFromCivil(int64_t year, int64_t month, int64_t day) {
    year -= month <= 2 ? 1 : 0;
    int64_t era = (year >= 0 ? year : year - 399) / 400;
    int64_t yearOfEra = year - era * 400;
    int64_t dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int64_t dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + dayOfEra - 719468;
}

void appendU32(TextBuffer* out, uint32_t value) {
    textAppend(out, reinterpret_cast<const char*>(&value), 4);
}

void appendText(TextBuffer* out, PrStringView text) {
    appendU32(out, text.data != nullptr ? uint32_t(text.size) : kNullValue);
    textAppend(out, text);
}

// Reads the fields of a record, in order, from a range of bytes.
struct Reader {
    const uint8_t* at;
    const uint8_t* end;

    bool take(void* out, size_t size) {
        if (size_t(end - at) < size)
            return false;
        memCopy(out, at, size);
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
        if (asciiEqualsFold(headers[i].name, "pragma")) {
            eachElement(headers[i].value, [&](PrStringView element) {
                control.noCache = control.noCache || asciiEqualsFold(element, "no-cache");
            });
            continue;
        }
        if (!asciiEqualsFold(headers[i].name, "cache-control"))
            continue;
        eachElement(headers[i].value, [&](PrStringView element) {
            size_t equals = stringFind(element, '=');
            PrStringView name = stringTrim(stringSlice(element, 0, equals), asciiIsTabOrSpace);
            PrStringView value;
            if (equals < element.size)
                value = stringTrim(stringSlice(element, equals + 1, element.size - equals - 1), asciiIsTabOrSpace);
            else
                value = PrStringView{nullptr, 0};
            if (asciiEqualsFold(name, "no-store")) {
                control.noStore = true;
            } else if (asciiEqualsFold(name, "no-cache")) {
                control.noCache = true;
            } else if (asciiEqualsFold(name, "max-age")) {
                double seconds = 0;
                // An invalid max-age makes the response stale (rfc 9111 section 4.2.1).
                control.hasMaxAge = true;
                control.maxAge = parseSeconds(value, &seconds) ? seconds : 0;
            }
        });
    }
    return control;
}

bool fetchCacheParseDate(PrStringView text, double* out) {
    int64_t day = -1;
    int64_t month = -1;
    int64_t year = -1;
    int64_t hour = -1;
    int64_t minute = 0;
    int64_t second = 0;
    size_t at = 0;
    while (at < text.size) {
        while (at < text.size && (text.data[at] == ' ' || text.data[at] == ',' || text.data[at] == '-'))
            ++at;
        size_t end = at;
        while (end < text.size && text.data[end] != ' ' && text.data[end] != ',' && text.data[end] != '-')
            ++end;
        PrStringView token = stringSlice(text, at, end - at);
        at = end;
        if (token.size == 0)
            continue;
        if (token.size == 8 && token.data[2] == ':' && token.data[5] == ':') {
            for (size_t i = 0; i < 8; ++i) {
                if (i != 2 && i != 5 && !asciiIsDigit(token.data[i]))
                    return false;
            }
            hour = (token.data[0] - '0') * 10 + (token.data[1] - '0');
            minute = (token.data[3] - '0') * 10 + (token.data[4] - '0');
            second = (token.data[6] - '0') * 10 + (token.data[7] - '0');
        } else if (asciiIsDigit(token.data[0])) {
            int64_t value = 0;
            for (size_t i = 0; i < token.size; ++i) {
                if (!asciiIsDigit(token.data[i]))
                    return false;
                value = value * 10 + (token.data[i] - '0');
            }
            if (day < 0 && token.size <= 2)
                day = value;
            else if (year < 0)
                year = token.size <= 2 ? (value < 70 ? 2000 + value : 1900 + value) : value;
            else
                return false;
        } else if (token.size >= 3) {
            for (int64_t m = 0; m < 12; ++m) {
                if (asciiEqualsFold(stringSlice(token, 0, 3), kMonths[m]) && token.size == 3)
                    month = m + 1;
            }
        }
    }
    if (day < 1 || day > 31 || month < 1 || year < 1601 || hour < 0 || hour > 23 || minute > 59 || second > 60)
        return false;
    int64_t seconds = daysFromCivil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
    *out = double(seconds) * 1000;
    return true;
}

PrStringView fetchCacheHeader(const PrFetchHeader* PR_NULLABLE headers, uint32_t count, PrStringView name) {
    for (uint32_t i = 0; i < count; ++i) {
        if (asciiEqualsFold(headers[i].name, name))
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
    double date = response->received;
    fetchCacheParseDate(fetchCacheHeader(headers, count, "date"), &date);
    PrStringView expires = fetchCacheHeader(headers, count, "expires");
    if (expires.data != nullptr) {
        double when = 0;
        // An Expires that is not a date is in the past (rfc 9111 section 5.3).
        return fetchCacheParseDate(expires, &when) && when > date ? when - date : 0;
    }
    double modified = 0;
    if (fetchCacheParseDate(fetchCacheHeader(headers, count, "last-modified"), &modified) && modified < date)
        return (date - modified) * kHeuristicFraction;
    return 0;
}

double fetchCacheAge(const FetchCachedResponse* response, double now) {
    const PrFetchHeader* headers = response->headers.data;
    uint32_t count = response->headers.count;
    double date = response->received;
    fetchCacheParseDate(fetchCacheHeader(headers, count, "date"), &date);
    double apparent = response->received > date ? response->received - date : 0;
    double ageSeconds = 0;
    parseSeconds(stringTrim(fetchCacheHeader(headers, count, "age"), asciiIsTabOrSpace), &ageSeconds);
    double corrected = ageSeconds * 1000 + (response->received - response->requested);
    double initial = apparent > corrected ? apparent : corrected;
    double resident = now > response->received ? now - response->received : 0;
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
        if (value.data != nullptr && stringTrim(value,
                asciiIsTabOrSpace) != stringTrim(varied.value, asciiIsTabOrSpace))
            return false;
    }
    return true;
}

void fetchCacheWrite(const FetchCachedResponse* response, TextBuffer* out) {
    textAppend(out, reinterpret_cast<const char*>(kRecordMagic), 4);
    appendU32(out, kRecordVersion);
    appendU32(out, response->status);
    textAppend(out, reinterpret_cast<const char*>(&response->requested), 8);
    textAppend(out, reinterpret_cast<const char*>(&response->received), 8);
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
    textAppend(out, reinterpret_cast<const char*>(&size), 8);
    textAppend(out, reinterpret_cast<const char*>(response->body.data), uint32_t(response->body.size));
}

bool fetchCacheRead(PrByteView bytes, FetchCachedResponse* out) {
    Reader reader{bytes.data, bytes.data + bytes.size};
    uint8_t magic[4];
    uint32_t version = 0;
    uint32_t status = 0;
    uint32_t headers = 0;
    if (!reader.take(magic, 4) || !memEquals(magic, kRecordMagic, 4) || !reader.take(&version, 4) ||
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
