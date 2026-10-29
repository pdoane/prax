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

void writeField(TextBuffer* out, const char* name, PrStringView value) {
    textAppendChar(out, ',');
    harnessWriteString(out, name);
    textAppendChar(out, ':');
    harnessWriteString(out, value);
}

void writeTaken(TextBuffer* out, const char* name, PrString value) {
    writeField(out, name, prStringView(&value));
    prStringReset(&value);
}

// Returns the component after `prefix`, as the search and hash getters write it.
PrString prefixed(PrStringView prefix, const PrString* component) {
    TextBuffer text;
    if (!prStringIsEmpty(component)) {
        textAppend(&text, prefix);
        textAppend(&text, prStringView(component));
    }
    return textTake(&text);
}

void writeUrl(TextBuffer* out, const Url* url) {
    writeTaken(out, "href", urlSerialize(url, PrUrlSerialization_Whole));
    PrString protocol = prStringClone(&url->scheme);
    prStringAppend(&protocol, ":");
    writeTaken(out, "protocol", protocol);
    writeField(out, "username", prStringView(&url->username));
    writeField(out, "password", prStringView(&url->password));
    writeField(out, "hostname", prStringView(&url->host));
    TextBuffer port;
    if (url->hasPort)
        textAppendU64(&port, url->port);
    writeField(out, "port", textView(&port));
    textReset(&port);
    writeField(out, "pathname", prStringView(&url->path));
    writeTaken(out, "search", prefixed("?", &url->query));
    writeTaken(out, "hash", prefixed("#", &url->fragment));

    PrString href = urlSerialize(url, PrUrlSerialization_Whole);
    PrUrl* reparsed = nullptr;
    if (urlParse(prStringView(&href), nullptr, PrCharset_Utf8, &reparsed) == PrStatus_Ok)
        writeTaken(out, "reparsed", urlSerialize(reparsed, PrUrlSerialization_Whole));
    urlDestroy(reparsed);
    prStringReset(&href);
}

// Reads the base length in the field. Returns false for `-`.
bool baseSize(PrStringView field, size_t* out) {
    if (field == "-")
        return false;
    uint64_t value = 0;
    parseU64(field.data, field.size, &value);
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
        PrStringView payload = textView(&test.input);
        size_t size = 0;
        bool hasBase = baseSize(test.fields[0], &size);
        if (size > payload.size)
            return 1;
        PrUrl* base = nullptr;
        PrUrl* url = nullptr;
        PrStatus status =
            hasBase ? urlParse(stringSlice(payload, 0, size), nullptr, PrCharset_Utf8, &base) : PrStatus_Ok;
        if (status == PrStatus_Ok)
            status = urlParse(stringSlice(payload, size, payload.size - size), base, PrCharset_Utf8, &url);

        TextBuffer out;
        textAppend(&out, "{\"status\":");
        harnessWriteString(&out, prStatusName(status));
        if (url != nullptr)
            writeUrl(&out, url);
        textAppendChar(&out, '}');
        harnessWriteLine(textView(&out));
        textReset(&out);
        urlDestroy(base);
        urlDestroy(url);
    }
    harnessCaseReset(&test);
    return 0;
}
