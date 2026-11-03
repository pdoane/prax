// The urltestdata.json harness, which tools/suite/url_parsing.py drives.
//
// The single field of a case is the byte length of the base at the start of its input, or `-` for no base. The response
// is the parse status and, for a URL, the components that the corpus records and the serialization of the URL that its
// serialization parses to.

#include "pr/assert.h"
#include "pr/integer.h"
#include "pr/string_ops.h"

#include "tools/harness/harness.h"
#include "url/src/url_record.h"

using namespace harness;

using namespace pr_impl;

namespace {

void writeField(PrTextBuffer* out, const char* name, PrStringView value) {
    prTextBufferAppendChar(out, ',');
    harnessWriteString(out, name);
    prTextBufferAppendChar(out, ':');
    harnessWriteString(out, value);
}

void writeTaken(PrTextBuffer* out, const char* name, PrRawString value) {
    writeField(out, name, prRawStringView(&value));
    prRawStringReset(&value);
}

// Returns the component after `prefix`, as the search and hash getters write it.
PrRawString prefixed(PrStringView prefix, const PrRawString* component) {
    PrTextBuffer text = {};
    if (!prRawStringIsEmpty(component)) {
        prTextBufferAppend(&text, prefix);
        prTextBufferAppend(&text, prRawStringView(component));
    }
    return prTextBufferTake(&text);
}

void writeUrl(PrTextBuffer* out, const Url* url) {
    writeTaken(out, "href", urlSerialize(url, PrUrlSerialization_Whole));
    PrRawString protocol = prRawStringClone(&url->scheme);
    prRawStringAppend(&protocol, ":");
    writeTaken(out, "protocol", protocol);
    writeField(out, "username", prRawStringView(&url->username));
    writeField(out, "password", prRawStringView(&url->password));
    writeField(out, "hostname", prRawStringView(&url->host));
    PrTextBuffer port = {};
    if (url->hasPort)
        prTextBufferAppendU64(&port, url->port, 10);
    writeField(out, "port", prTextBufferView(&port));
    prTextBufferReset(&port);
    writeField(out, "pathname", prRawStringView(&url->path));
    writeTaken(out, "search", prefixed("?", &url->query));
    writeTaken(out, "hash", prefixed("#", &url->fragment));

    PrRawString href = urlSerialize(url, PrUrlSerialization_Whole);
    PrUrl* reparsed = nullptr;
    if (urlParse(prRawStringView(&href), nullptr, PrCharset_Utf8, &reparsed) == PrStatus_Ok)
        writeTaken(out, "reparsed", urlSerialize(reparsed, PrUrlSerialization_Whole));
    urlDestroy(reparsed);
    prRawStringReset(&href);
}

// Reads the base length in the field. Returns false for `-`.
bool baseSize(PrStringView field, size_t* out) {
    if (field == "-")
        return false;
    uint64_t value = 0;
    prU64Parse(field.data, field.size, &value, 10);
    *out = size_t(value);
    return true;
}

} // namespace

int main() {
    prCrashHandler();
    HarnessCase test = {};
    while (harnessReadCase(&test)) {
        if (test.fieldCount != 1)
            return 1;
        PrStringView payload = prTextBufferView(&test.input);
        size_t size = 0;
        bool hasBase = baseSize(test.fields[0], &size);
        if (size > payload.size)
            return 1;
        PrUrl* base = nullptr;
        PrUrl* url = nullptr;
        PrStatus status =
            hasBase ? urlParse(prStringSlice(payload, 0, size), nullptr, PrCharset_Utf8, &base) : PrStatus_Ok;
        if (status == PrStatus_Ok)
            status = urlParse(prStringSlice(payload, size, payload.size - size), base, PrCharset_Utf8, &url);

        PrTextBuffer out = {};
        prTextBufferAppend(&out, "{\"status\":");
        harnessWriteString(&out, prStatusName(status));
        if (url != nullptr)
            writeUrl(&out, url);
        prTextBufferAppendChar(&out, '}');
        harnessWriteLine(prTextBufferView(&out));
        prTextBufferReset(&out);
        urlDestroy(base);
        urlDestroy(url);
    }
    harnessCaseReset(&test);
    return 0;
}
