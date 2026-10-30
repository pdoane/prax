// JSON text read into values, and written from them.

#include "pr/json.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/ascii.h"
#include "pr/float.h"
#include "pr/float_convert.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/utf8.h"

namespace pr_impl {

namespace {

// A value of a document.
struct JsonValue {
    PrJsonKind kind;
    uint32_t count;             // the number of elements of an Array, members of an Object or bytes of a String
    uint32_t span;              // for an Array or Object, the number of values inside it at any depth
    union {
        double number;          // Number
        uint32_t text;          // for a String, the offset of its bytes in the document's `text`
    };
};

// A document's values in document order, and the unescaped bytes of its strings. The value after an array or object is
// at `index + 1 + span`.
struct Json {
    Array<JsonValue> values;    // by PrJsonValueIndex
    Array<char> text;
};

struct Reader {
    const char* text;
    size_t size;
    size_t at;
    Json* document;
};

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

void skipSpace(Reader& r) {
    while (r.at < r.size && isSpace(r.text[r.at]))
        ++r.at;
}

uint32_t emit(Reader& r, PrJsonKind kind) {
    uint32_t index = r.document->values.count;
    JsonValue& value = r.document->values.emplace();
    value.kind = kind;
    value.count = 0;
    value.span = 0;
    value.number = 0;
    return index;
}

bool literal(Reader& r, const char* word, PrJsonKind kind) {
    PrStringView rest{r.text + r.at, r.size - r.at};
    if (!stringStartsWith(rest, word))
        return false;
    r.at += PrStringView(word).size;
    emit(r, kind);
    return true;
}

bool number(Reader& r) {
    size_t start = r.at;
    auto digits = [&] {
        size_t from = r.at;
        while (r.at < r.size && asciiIsDigit(r.text[r.at]))
            ++r.at;
        return r.at > from;
    };
    if (r.at < r.size && r.text[r.at] == '-')
        ++r.at;
    if (r.at < r.size && r.text[r.at] == '0') {
        ++r.at;
    } else if (!digits()) {
        return false;
    }
    if (r.at < r.size && r.text[r.at] == '.') {
        ++r.at;
        if (!digits())
            return false;
    }
    if (r.at < r.size && (r.text[r.at] == 'e' || r.text[r.at] == 'E')) {
        ++r.at;
        if (r.at < r.size && (r.text[r.at] == '+' || r.text[r.at] == '-'))
            ++r.at;
        if (!digits())
            return false;
    }
    double value = 0;
    if (!decimalToF64(PrStringView{r.text + start, r.at - start}, &value))
        return false;
    uint32_t index = emit(r, PrJsonKind_Number);
    r.document->values.data[index].number = value;
    return true;
}

// Reads the four hex digits at the reader's position and returns them as a code unit. Returns -1 if they are not hex
// digits.
int32_t hexUnit(Reader& r) {
    if (r.size - r.at < 4)
        return -1;
    int32_t unit = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        if (!asciiIsHexDigit(r.text[r.at + i]))
            return -1;
        unit = unit << 4 | int32_t(asciiHexValue(r.text[r.at + i]));
    }
    r.at += 4;
    return unit;
}

void appendCodePoint(Array<char>& text, uint32_t codePoint) {
    char bytes[kPrUtf8MaxBytes];
    text.append(bytes, prUtf8Encode(codePoint, bytes));
}

// Reads a string at the reader's quote into the document's text.
bool string(Reader& r) {
    ++r.at;
    Array<char>& text = r.document->text;
    uint32_t offset = uint32_t(text.count);
    while (true) {
        size_t run = r.at;
        while (r.at < r.size && r.text[r.at] != '"' && r.text[r.at] != '\\' &&
                uint8_t(r.text[r.at]) >= 0x20 && uint8_t(r.text[r.at]) < 0x80)
            ++r.at;
        text.append(r.text + run, uint32_t(r.at - run));
        if (r.at >= r.size)
            return false;
        char c = r.text[r.at];
        if (c == '"') {
            ++r.at;
            break;
        }
        if (uint8_t(c) < 0x20)
            return false;
        if (c != '\\') {
            uint32_t codePoint = 0;
            r.at += prUtf8DecodeScalar(PrStringView{r.text, r.size}, r.at, &codePoint);
            appendCodePoint(text, codePoint);
            continue;
        }
        if (++r.at >= r.size)
            return false;
        char escape = r.text[r.at++];
        switch (escape) {
            case '"':
            case '\\':
            case '/':
                text.push(escape);
                break;
            case 'b':
                text.push('\b');
                break;
            case 'f':
                text.push('\f');
                break;
            case 'n':
                text.push('\n');
                break;
            case 'r':
                text.push('\r');
                break;
            case 't':
                text.push('\t');
                break;
            case 'u': {
                int32_t unit = hexUnit(r);
                if (unit < 0)
                    return false;
                uint32_t codePoint = uint32_t(unit);
                if (unit >= 0xd800 && unit < 0xdc00 && r.size - r.at >= 6 && r.text[r.at] == '\\' && r.text[r.at + 1] == 'u') {
                    size_t back = r.at;
                    r.at += 2;
                    int32_t low = hexUnit(r);
                    if (low >= 0xdc00 && low < 0xe000)
                        codePoint = 0x10000 + ((uint32_t(unit) - 0xd800) << 10) + (uint32_t(low) - 0xdc00);
                    else
                        r.at = back;
                }
                appendCodePoint(text, codePoint);
                break;
            }
            default:
                return false;
        }
    }
    uint32_t index = emit(r, PrJsonKind_String);
    JsonValue& value = r.document->values.data[index];
    value.text = offset;
    value.count = uint32_t(text.count) - offset;
    return true;
}

// Reads a value that is not an array or object, at the reader's position.
bool scalar(Reader& r) {
    if (r.at >= r.size)
        return false;
    switch (r.text[r.at]) {
        case '"':
            return string(r);
        case 't':
            return literal(r, "true", PrJsonKind_True);
        case 'f':
            return literal(r, "false", PrJsonKind_False);
        case 'n':
            return literal(r, "null", PrJsonKind_Null);
        default:
            return number(r);
    }
}

bool read(Reader& r) {
    Array<uint32_t> open;  // the open arrays and objects, innermost last
    bool ok = true;
    bool closed = false;
    while (ok) {
        // Read a value, if one is expected.
        if (!closed) {
            skipSpace(r);
            if (r.at < r.size && (r.text[r.at] == '[' || r.text[r.at] == '{')) {
                bool isObject = r.text[r.at] == '{';
                ++r.at;
                if (open.count == kPrJsonMaxDepth) {
                    ok = false;
                    break;
                }
                open.push(emit(r, isObject ? PrJsonKind_Object : PrJsonKind_Array));
                skipSpace(r);
                if (r.at < r.size && r.text[r.at] == (isObject ? '}' : ']')) {
                    ++r.at;
                    uint32_t index = open.data[--open.count];
                    r.document->values.data[index].span = r.document->values.count - index - 1;
                    closed = true;
                    continue;
                }
                if (isObject) {
                    ++r.document->values.data[open.last()].count;
                    if (r.at >= r.size || r.text[r.at] != '"' || !string(r)) {
                        ok = false;
                        break;
                    }
                    skipSpace(r);
                    if (r.at >= r.size || r.text[r.at] != ':') {
                        ok = false;
                        break;
                    }
                    ++r.at;
                } else {
                    ++r.document->values.data[open.last()].count;
                }
                continue;
            }
            if (!scalar(r)) {
                ok = false;
                break;
            }
        }
        closed = false;
        // Read what follows a value: the end, a separator, or the close of its container.
        skipSpace(r);
        if (open.count == 0)
            break;
        uint32_t container = open.last();
        bool isObject = r.document->values.data[container].kind == PrJsonKind_Object;
        if (r.at >= r.size) {
            ok = false;
            break;
        }
        char c = r.text[r.at++];
        if (c == ',') {
            ++r.document->values.data[container].count;
            if (isObject) {
                skipSpace(r);
                if (r.at >= r.size || r.text[r.at] != '"' || !string(r)) {
                    ok = false;
                    break;
                }
                skipSpace(r);
                if (r.at >= r.size || r.text[r.at] != ':') {
                    ok = false;
                    break;
                }
                ++r.at;
            }
            continue;
        }
        if (c != (isObject ? '}' : ']')) {
            --r.at;
            ok = false;
            break;
        }
        open.pop();
        r.document->values.data[container].span = r.document->values.count - container - 1;
        closed = true;
    }
    open.reset();
    return ok && r.at == r.size;
}

// An array or object that a writer has begun and not ended.
struct JsonWriterOpen {
    PrJsonKind kind;            // Array or Object
    bool hasValue;              // an element or a member has been written inside it
    bool awaitsValue;           // for an Object, a key has been written and its value has not
};

// JSON text under construction, and the containers that are open in it.
struct JsonWriter {
    TextBuffer text;
    Array<JsonWriterOpen> open; // innermost last
    bool hasRoot;
};

// Writes the separator that comes before a value, and records that the value is written.
void beforeValue(JsonWriter* w) {
    if (w->open.count == 0) {
        PR_ASSERT(!w->hasRoot);
        w->hasRoot = true;
        return;
    }
    JsonWriterOpen& open = w->open.last();
    if (open.kind == PrJsonKind_Object) {
        PR_ASSERT(open.awaitsValue);
        open.awaitsValue = false;
        return;
    }
    if (open.hasValue)
        textAppendChar(&w->text, ',');
    open.hasValue = true;
}

void begin(JsonWriter* w, PrJsonKind kind) {
    beforeValue(w);
    textAppendChar(&w->text, kind == PrJsonKind_Object ? '{' : '[');
    w->open.push({.kind = kind});
}

void end(JsonWriter* w, PrJsonKind kind) {
    PR_ASSERT(w->open.count != 0 && w->open.last().kind == kind && !w->open.last().awaitsValue);
    w->open.pop();
    textAppendChar(&w->text, kind == PrJsonKind_Object ? '}' : ']');
}

void quote(JsonWriter* w, PrStringView value) {
    static const char kHex[] = "0123456789abcdef";
    TextBuffer* out = &w->text;
    textAppendChar(out, '"');
    size_t i = 0;
    while (i < value.size) {
        size_t run = i;
        while (i < value.size && value.data[i] != '"' && value.data[i] != '\\' && uint8_t(value.data[i]) >= 0x20 &&
                uint8_t(value.data[i]) < 0x80)
            ++i;
        textAppend(out, value.data + run, uint32_t(i - run));
        if (i >= value.size)
            break;
        uint8_t c = uint8_t(value.data[i]);
        if (c >= 0x80) {
            uint32_t codePoint = 0;
            i += prUtf8DecodeScalar(value, i, &codePoint);
            textAppendCodePoint(out, codePoint);
            continue;
        }
        ++i;
        textAppendChar(out, '\\');
        switch (c) {
            case '"':
            case '\\':
                textAppendChar(out, char(c));
                break;
            case '\b':
                textAppendChar(out, 'b');
                break;
            case '\f':
                textAppendChar(out, 'f');
                break;
            case '\n':
                textAppendChar(out, 'n');
                break;
            case '\r':
                textAppendChar(out, 'r');
                break;
            case '\t':
                textAppendChar(out, 't');
                break;
            default:
                textAppend(out, "u00");
                textAppendChar(out, kHex[c >> 4]);
                textAppendChar(out, kHex[c & 15]);
                break;
        }
    }
    textAppendChar(out, '"');
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrJson : Json {};
struct PrJsonWriter : JsonWriter {};

PrJson* prJsonCreate(void) {
    return allocZeroed<PrJson>();
}

void prJsonDestroy(PrJson* PR_NULLABLE document) {
    if (document == nullptr)
        return;
    document->values.reset();
    document->text.reset();
    prMemoryFree(document);
}

bool prJsonParse(PrJson* document, PrStringView text, size_t* PR_NULLABLE errorAt) {
    document->values.clear();
    document->text.clear();
    Reader r{text.data, text.size, 0, document};
    if (stringStartsWith(text, "\xEF\xBB\xBF"))
        r.at = 3;
    bool ok = text.size > 0 && read(r);
    if (!ok) {
        document->values.clear();
        document->text.clear();
        if (errorAt != nullptr)
            *errorAt = r.at;
    }
    return ok;
}

uint32_t prJsonValueCount(const PrJson* document) {
    return document->values.count;
}

PrJsonKind prJsonKind(const PrJson* document, PrJsonValueIndex index) {
    return index < document->values.count ? document->values.data[index].kind : PrJsonKind_Null;
}

PrJsonValueIndex prJsonNext(const PrJson* document, PrJsonValueIndex index) {
    return index + 1 + document->values.data[index].span;
}

PrStringView prJsonString(const PrJson* document, PrJsonValueIndex index) {
    if (prJsonKind(document, index) != PrJsonKind_String)
        return PrStringView{};
    const JsonValue& value = document->values.data[index];
    // An empty string is the empty view, not the null view, even if no text is stored at all.
    if (value.count == 0)
        return "";
    return PrStringView{document->text.data + value.text, value.count};
}

double prJsonNumber(const PrJson* document, PrJsonValueIndex index, double fallback) {
    switch (prJsonKind(document, index)) {
        case PrJsonKind_Number:
            return document->values.data[index].number;
        case PrJsonKind_True:
            return 1;
        case PrJsonKind_False:
            return 0;
        default:
            return fallback;
    }
}

uint32_t prJsonCount(const PrJson* document, PrJsonValueIndex index) {
    PrJsonKind kind = prJsonKind(document, index);
    return kind == PrJsonKind_Array || kind == PrJsonKind_Object ? document->values.data[index].count : 0;
}

PrJsonValueIndex prJsonMember(const PrJson* document, PrJsonValueIndex object, PrStringView key) {
    if (prJsonKind(document, object) != PrJsonKind_Object)
        return kPrJsonMissing;
    PrJsonValueIndex found = kPrJsonMissing;
    for (PrJsonValueIndex i = prJsonFirst(document, object); i != kPrJsonMissing; i = prJsonAfter(document, object, i)) {
        if (prJsonString(document, i) == key)
            found = i + 1;
    }
    return found;
}

PrJsonValueIndex prJsonElement(const PrJson* document, PrJsonValueIndex array, uint32_t at) {
    if (prJsonKind(document, array) != PrJsonKind_Array || at >= document->values.data[array].count)
        return kPrJsonMissing;
    PrJsonValueIndex i = array + 1;
    for (uint32_t n = 0; n < at; ++n)
        i = prJsonNext(document, i);
    return i;
}

PrJsonValueIndex prJsonFirst(const PrJson* document, PrJsonValueIndex container) {
    return prJsonCount(document, container) > 0 ? container + 1 : kPrJsonMissing;
}

PrJsonValueIndex prJsonAfter(const PrJson* document, PrJsonValueIndex container, PrJsonValueIndex index) {
    PrJsonValueIndex next = prJsonNext(document, index);
    if (document->values.data[container].kind == PrJsonKind_Object)
        next = prJsonNext(document, next);
    return next < prJsonNext(document, container) ? next : kPrJsonMissing;
}

PrJsonWriter* prJsonWriterCreate(void) {
    return allocZeroed<PrJsonWriter>();
}

void prJsonWriterDestroy(PrJsonWriter* PR_NULLABLE writer) {
    if (writer == nullptr)
        return;
    writer->text.text.reset();
    writer->open.reset();
    prMemoryFree(writer);
}

void prJsonWriterClear(PrJsonWriter* writer) {
    textClear(&writer->text);
    writer->open.clear();
    writer->hasRoot = false;
}

void prJsonWriterBeginArray(PrJsonWriter* writer) {
    begin(writer, PrJsonKind_Array);
}

void prJsonWriterEndArray(PrJsonWriter* writer) {
    end(writer, PrJsonKind_Array);
}

void prJsonWriterBeginObject(PrJsonWriter* writer) {
    begin(writer, PrJsonKind_Object);
}

void prJsonWriterEndObject(PrJsonWriter* writer) {
    end(writer, PrJsonKind_Object);
}

void prJsonWriterKey(PrJsonWriter* writer, PrStringView key) {
    PR_ASSERT(writer->open.count != 0 && writer->open.last().kind == PrJsonKind_Object &&
        !writer->open.last().awaitsValue);
    JsonWriterOpen& open = writer->open.last();
    if (open.hasValue)
        textAppendChar(&writer->text, ',');
    open.hasValue = true;
    open.awaitsValue = true;
    quote(writer, key);
    textAppendChar(&writer->text, ':');
}

void prJsonWriterString(PrJsonWriter* writer, PrStringView value) {
    beforeValue(writer);
    quote(writer, value);
}

void prJsonWriterNumber(PrJsonWriter* writer, double value) {
    beforeValue(writer);
    if (!isFiniteF64(value)) {
        textAppend(&writer->text, "null");
        return;
    }
    char text[kF64TextMaxBytes];
    textAppend(&writer->text, text, uint32_t(formatF64(value, text)));
}

void prJsonWriterBool(PrJsonWriter* writer, bool value) {
    beforeValue(writer);
    textAppend(&writer->text, value ? "true" : "false");
}

void prJsonWriterNull(PrJsonWriter* writer) {
    beforeValue(writer);
    textAppend(&writer->text, "null");
}

void prJsonWriterValue(PrJsonWriter* writer, const PrJson* document, PrJsonValueIndex index) {
    PrJsonKind kind = prJsonKind(document, index);
    switch (kind) {
        case PrJsonKind_Null:
            prJsonWriterNull(writer);
            break;
        case PrJsonKind_False:
        case PrJsonKind_True:
            prJsonWriterBool(writer, kind == PrJsonKind_True);
            break;
        case PrJsonKind_Number:
            prJsonWriterNumber(writer, document->values.data[index].number);
            break;
        case PrJsonKind_String:
            prJsonWriterString(writer, prJsonString(document, index));
            break;
        case PrJsonKind_Array:
            prJsonWriterBeginArray(writer);
            for (PrJsonValueIndex i = prJsonFirst(document, index); i != kPrJsonMissing; i = prJsonAfter(document, index, i))
                prJsonWriterValue(writer, document, i);
            prJsonWriterEndArray(writer);
            break;
        case PrJsonKind_Object:
            prJsonWriterBeginObject(writer);
            for (PrJsonValueIndex i = prJsonFirst(document, index); i != kPrJsonMissing; i =
                    prJsonAfter(document, index, i)) {
                prJsonWriterKey(writer, prJsonString(document, i));
                prJsonWriterValue(writer, document, i + 1);
            }
            prJsonWriterEndObject(writer);
            break;
    }
}

PrStringView prJsonWriterText(const PrJsonWriter* writer) {
    return textView(&writer->text);
}
