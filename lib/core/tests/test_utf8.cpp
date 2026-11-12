#include "pr/array.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/utf8.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// The code point at `offset`, and the number of bytes that encoded it.
struct Decoded {
    uint32_t codePoint;
    uint32_t width;
};

Decoded decode(PrStringView text, uint32_t offset = 0) {
    Decoded out = {0, 0};
    out.width = prUtf8Decode(text, offset, &out.codePoint);
    return out;
}

Decoded decodeScalar(PrStringView text, uint32_t offset = 0) {
    Decoded out = {0, 0};
    out.width = prUtf8DecodeScalar(text, offset, &out.codePoint);
    return out;
}

String encode(uint32_t codePoint) {
    char buffer[kPrUtf8MaxBytes];
    return String(PrStringView(buffer, prUtf8Encode(codePoint, buffer)));
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a sequence decodes to the code point it spells") {
    CHECK(decode("a").codePoint == 'a');
    CHECK(decode("a").width == 1);
    CHECK(decode("\xc3\xa9").codePoint == 0xe9);
    CHECK(decode("\xc3\xa9").width == 2);
    CHECK(decode("\xe2\x82\xac").codePoint == 0x20ac);
    CHECK(decode("\xf0\x9f\x98\x80").codePoint == 0x1f600);
    CHECK(decode("\xf0\x9f\x98\x80").width == 4);
}

TEST_CASE("reading at or past the end returns zero") {
    CHECK(decode("").width == 0);
    CHECK(decode("a", 1).width == 0);
    CHECK(decodeScalar("a", 1).width == 0);
}

TEST_CASE("ill-formed input yields U+FFFD") {
    CHECK(decode("\x80").codePoint == kPrUtf8Replacement);
    CHECK(decode("\x80").width == 1);
    CHECK(decode("\xc3").codePoint == kPrUtf8Replacement);   // truncated
    CHECK(decode("\xc3""a").codePoint == kPrUtf8Replacement);
    CHECK(decode("\xc0\xaf").codePoint == kPrUtf8Replacement);         // overlong
    CHECK(decode("\xf4\x90\x80\x80").codePoint == kPrUtf8Replacement); // above U+10FFFF
}

TEST_CASE("one U+FFFD stands for each maximal subpart") {
    // The bytes that begin a well-formed sequence form one subpart, however many there are.
    CHECK(decode("\xe2\x82""a").width == 2);
    CHECK(decode("\xe2\x82").width == 2);
    CHECK(decode("\xf0\x9f\x98""a").width == 3);
    CHECK(decode("\xf0\x9f\x98").width == 3);

    // A second byte outside the lead's range does not form a sequence with it.
    CHECK(decode("\xe0\x80\x80").width == 1);     // overlong
    CHECK(decode("\xf4\x90\x80\x80").width == 1); // above U+10FFFF
    CHECK(decode("\xc0\xaf").width == 1);
    CHECK(decode("\xff").width == 1);
}

TEST_CASE("a surrogate is a code point, and a null is a null") {
    CHECK(decode("\xed\xa0\x80").codePoint == 0xd800);
    CHECK(decode("\xed\xa0\x80").width == 3);
    CHECK(decode("\xed\xbf\xbf").codePoint == 0xdfff);

    Decoded decoded = decode(PrStringView("\0", 1));
    CHECK(decoded.codePoint == 0);
    CHECK(decoded.width == 1);
}

TEST_CASE("a scalar value is never a surrogate and never a null") {
    // The three bytes are ill formed rather than one code point, so each byte is replaced separately.
    CHECK(decodeScalar("\xed\xa0\x80").codePoint == kPrUtf8Replacement);
    CHECK(decodeScalar("\xed\xa0\x80").width == 1);

    Decoded decoded = decodeScalar(PrStringView("\0", 1));
    CHECK(decoded.codePoint == kPrUtf8Replacement);
    CHECK(decoded.width == 1);

    CHECK(decodeScalar("\xf0\x9f\x98\x80").codePoint == 0x1f600);
    CHECK(decodeScalar("\xf0\x9f\x98\x80").width == 4);
}

TEST_CASE("validating stops at the first ill-formed sequence") {
    auto validate = [](PrStringView text) {
        return prUtf8Validate(text);
    };
    CHECK(validate("") == 0);
    CHECK(validate("plain ASCII that runs past sixteen bytes") == 40);
    CHECK(validate("caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80") == 14);
    CHECK(validate("\xed\xa0\x80") == 3);   // a surrogate
    CHECK(validate("0123456789abcdef\x80") == 16);
    CHECK(validate("ab\xe2\x82") == 2);
    CHECK(validate("ab\xc0\xaf" "cd") == 2);
}

TEST_CASE("validating finds a sequence wherever it falls in a long text") {
    // A well-formed sequence at each offset of text long enough for several blocks, followed by an ill-formed one.
    const PrStringView sequences[] = {"\xc3\xa9", "\xe2\x82\xac", "\xf0\x9f\x98\x80", "\xed\xa0\x80"};
    for (PrStringView sequence : sequences) {
        uint32_t size = uint32_t(sequence.size);
        for (uint32_t at = 0; at < 150; ++at) {
            String text;
            for (uint32_t i = 0; i < 200; ++i)
                text.append('a');
            text.replace(at, size, sequence);
            CAPTURE(at);
            CHECK(prUtf8Validate(text) == text.count());

            text.replace(at + size + 7, 1, "\x80");
            CHECK(prUtf8Validate(text) == at + size + 7);

            // If the sequence is cut short, the text stops being well formed at the sequence.
            text.replace(at, size, format("{}a", prStringSlice(sequence, 0, size - 1)));
            CHECK(prUtf8Validate(text) == at);
        }
    }
}

TEST_CASE("repaired text has U+FFFD in place of each maximal subpart") {
    auto repair = [](PrStringView text) {
        PrTextBuffer out = {};
        prTextBufferAppendUtf8(&out, text);
        String result(prTextBufferView(&out));
        prTextBufferReset(&out);
        return result;
    };
    CHECK(repair("caf\xc3\xa9") == "caf\xc3\xa9");
    CHECK(repair("a\xe2\x82""b") == "a\xef\xbf\xbd""b");
    CHECK(repair("\xe0\x80\x80") == "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd");
    CHECK(repair("\xed\xa0\x80") == "\xed\xa0\x80");
    CHECK(repair("a\xf0\x9f\x98") == "a\xef\xbf\xbd");
}

TEST_CASE("a decoder of bytes one at a time agrees with decoding the whole text as scalar values") {
    // Returns the code points that a decoder gives for `text`, with a replacement for a sequence that is incomplete at
    // the end. The decoder keeps U+0000, which prUtf8DecodeScalar replaces.
    auto pushed = [](PrStringView text) {
        Array<uint32_t> out;
        PrUtf8Decoder decoder = {};
        for (size_t at = 0; at < text.size;) {
            uint32_t codePoint = 0;
            PrUtf8Step step = prUtf8DecoderPush(&decoder, uint8_t(text.data[at]), &codePoint);
            if (step != PrUtf8Step_Interrupted)
                ++at;
            if (step == PrUtf8Step_CodePoint)
                out.push(codePoint);
            else if (step != PrUtf8Step_Continue)
                out.push(kPrUtf8Replacement);
        }
        if (prUtf8DecoderIsInSequence(&decoder))
            out.push(kPrUtf8Replacement);
        return out;
    };
    auto whole = [](PrStringView text) {
        Array<uint32_t> out;
        for (size_t at = 0; at < text.size;) {
            uint32_t codePoint = 0;
            at += prUtf8DecodeScalar(text, at, &codePoint);
            out.push(codePoint);
        }
        return out;
    };
    const PrStringView texts[] = {
        "caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80", "a\xe2\x82""b", "\xe0\x80\x80",
        "\xed\xa0\x80", "a\xf0\x9f\x98", "\xc0\xaf\xff", "\xf4\x90\x80\x80", "\xf0\x9f\x98\x80\x80", "\xc3",
    };
    for (PrStringView text : texts) {
        CAPTURE(text);
        CHECK(pushed(text) == whole(text));
    }
    CHECK(pushed(PrStringView("\0", 1)) == Array<uint32_t>(1));
}

TEST_CASE("a code point encodes to the sequence that spells it") {
    CHECK(encode('a') == "a");
    CHECK(encode(0xe9) == "\xc3\xa9");
    CHECK(encode(0x20ac) == "\xe2\x82\xac");
    CHECK(encode(0x1f600) == "\xf0\x9f\x98\x80");
}

TEST_CASE("a surrogate and a value out of range encode as U+FFFD") {
    CHECK(encode(0xd800) == "\xef\xbf\xbd");
    CHECK(encode(0x110000) == "\xef\xbf\xbd");
}
