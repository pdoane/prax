// Percent encoding and decoding of bytes.
//
// url - https://url.spec.whatwg.org/

#include "pr/url_percent.h"
#include "url/src/url_percent.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/bitset.h"
#include "pr/string_ops.h"

namespace pr_impl {

namespace {

struct EncodeSets {
    uint64_t words[PrUrlPercentEncodeSet_Count][4];
};

constexpr bool inSet(uint32_t byte, const char* members) {
    for (; *members != '\0'; ++members) {
        if (byte == uint8_t(*members))
            return true;
    }
    return false;
}

// url/#c0-control-percent-encode-set and the sets defined after it.
constexpr EncodeSets makeEncodeSets() {
    EncodeSets sets = {};
    for (uint32_t byte = 0; byte < 256; ++byte) {
        bool c0 = byte <= 0x1f || byte > 0x7e;
        bool query = c0 || inSet(byte, " \"#<>");
        bool path = query || inSet(byte, "?^`{}");
        bool userinfo = path || inSet(byte, "/:;=@[\\]|");
        bool component = userinfo || inSet(byte, "$%&+,");
        bool member[PrUrlPercentEncodeSet_Count] = {};
        member[PrUrlPercentEncodeSet_C0Control] = c0;
        member[PrUrlPercentEncodeSet_Fragment] = c0 || inSet(byte, " \"<>`");
        member[PrUrlPercentEncodeSet_Query] = query;
        member[PrUrlPercentEncodeSet_SpecialQuery] = query || byte == '\'';
        member[PrUrlPercentEncodeSet_Path] = path;
        member[PrUrlPercentEncodeSet_Userinfo] = userinfo;
        member[PrUrlPercentEncodeSet_Component] = component;
        member[PrUrlPercentEncodeSet_Form] = component || inSet(byte, "!'()~");
        for (uint32_t set = 0; set < PrUrlPercentEncodeSet_Count; ++set) {
            if (member[set])
                bitsetSet(sets.words[set], byte);
        }
    }
    return sets;
}

constexpr EncodeSets kEncodeSets = makeEncodeSets();
constexpr char kHexDigits[] = "0123456789ABCDEF";

} // namespace

void urlAppendPercentEncodedIn(TextBuffer* out, PrStringView text, PrCharset charset, PrUrlPercentEncodeSet set) {
    if (charset == PrCharset_Utf8) {
        urlAppendPercentEncoded(out, text, set);
        return;
    }
    PrCharsetEncoder encoder = prCharsetEncoderMake(charset);
    TextBuffer bytes;
    for (size_t at = 0;;) {
        PrStringView rest = stringSlice(text, at, text.size - at);
        textClear(&bytes);
        bytes.text.resize(uint32_t(prCharsetEncodeMaxSize(&encoder, rest.size)));
        size_t size = 0;
        uint32_t unencoded = 0;
        at += prCharsetEncode(&encoder, rest, PrCharsetInput_Final, bytes.text.data, &size, &unencoded);
        bytes.text.count = uint32_t(size);
        urlAppendPercentEncoded(out, textView(&bytes), set);
        if (unencoded == kPrCharsetUnencodedNone)
            break;
        textAppend(out, "%26%23");
        textAppendU64(out, unencoded);
        textAppend(out, "%3B");
    }
    textReset(&bytes);
}

} // namespace pr_impl

using namespace pr_impl;

size_t prUrlPercentEncode(PrStringView input, PrUrlPercentEncodeSet set, char* PR_NULLABLE out,
                          size_t capacity) {
    PR_ASSERT(set < PrUrlPercentEncodeSet_Count);
    if (out == nullptr)
        capacity = 0;
    size_t size = 0;
    auto put = [&](char c) {
        if (size < capacity)
            out[size] = c;
        ++size;
    };
    for (size_t i = 0; i < input.size; ++i) {
        uint8_t byte = uint8_t(input.data[i]);
        if (bitsetTest(kEncodeSets.words[set], byte)) {
            put('%');
            put(kHexDigits[byte >> 4]);
            put(kHexDigits[byte & 15]);
        } else {
            put(char(byte));
        }
    }
    return size;
}

size_t prUrlPercentDecode(PrStringView input, char* PR_NULLABLE out, size_t capacity) {
    if (out == nullptr)
        capacity = 0;
    size_t size = 0;
    for (size_t i = 0; i < input.size; ++i, ++size) {
        char byte = input.data[i];
        if (byte == '%' && input.size - i > 2 && asciiIsHexDigit(input.data[i + 1])
            && asciiIsHexDigit(input.data[i + 2])) {
            byte = char(asciiHexValue(input.data[i + 1]) << 4 | asciiHexValue(input.data[i + 2]));
            i += 2;
        }
        if (size < capacity)
            out[size] = byte;
    }
    return size;
}
