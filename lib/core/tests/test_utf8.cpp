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

Decoded decodeWtf8(PrStringView text, uint32_t offset = 0) {
    Decoded out = {0, 0};
    out.width = prWtf8Decode(text, offset, &out.codePoint);
    return out;
}

String encode(uint32_t codePoint) {
    char buffer[kPrUtf8MaxBytes];
    return String(PrStringView(buffer, prUtf8Encode(codePoint, buffer)));
}

String encodeWtf8(uint32_t codePoint) {
    char buffer[kPrUtf8MaxBytes];
    return String(PrStringView(buffer, prWtf8Encode(codePoint, buffer)));
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
    CHECK(decodeWtf8("a", 1).width == 0);
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

TEST_CASE("a null decodes as itself") {
    CHECK(decode(PrStringView("\0", 1)).codePoint == 0);
    CHECK(decode(PrStringView("\0", 1)).width == 1);
    CHECK(decodeWtf8(PrStringView("\0", 1)).codePoint == 0);
}

TEST_CASE("UTF-8 has no surrogate, so a surrogate's bytes are ill formed") {
    // 0xed cannot lead a sequence whose second byte is 0xa0, so each byte is replaced separately.
    CHECK(decode("\xed\xa0\x80").codePoint == kPrUtf8Replacement);
    CHECK(decode("\xed\xa0\x80").width == 1);
    CHECK(decode("\xed\x9f\xbf").codePoint == 0xd7ff);
    CHECK(decode("\xee\x80\x80").codePoint == 0xe000);
}

TEST_CASE("WTF-8 has lone surrogates, and a split pair decodes as the pair's code point") {
    CHECK(decodeWtf8("\xed\xa0\x80").codePoint == 0xd800);
    CHECK(decodeWtf8("\xed\xa0\x80").width == 3);
    CHECK(decodeWtf8("\xed\xbf\xbf").codePoint == 0xdfff);
    CHECK(decodeWtf8("\xed\xb0\x80\xed\xa0\x80").codePoint == 0xdc00);   // low then high: two lone surrogates
    CHECK(decodeWtf8("\xed\xa0\xbd\xed\xb8\x80").codePoint == 0x1f600);
    CHECK(decodeWtf8("\xed\xa0\xbd\xed\xb8\x80").width == 6);
    CHECK(decodeWtf8("\xf0\x9f\x98\x80").codePoint == 0x1f600);
    CHECK(decodeWtf8("\xc0\xaf").codePoint == kPrUtf8Replacement);
}

TEST_CASE("a WTF-8 code point read as a scalar value is U+FFFD for a lone surrogate") {
    uint32_t codePoint = 0;
    CHECK(prWtf8DecodeScalar("\xed\xa0\x80", 0, &codePoint) == 3);
    CHECK(codePoint == kPrUtf8Replacement);
    CHECK(prWtf8DecodeScalar("\xed\xa0\xbd\xed\xb8\x80", 0, &codePoint) == 6);
    CHECK(codePoint == 0x1f600);
    CHECK(prWtf8DecodeScalar("\xc3\xa9", 0, &codePoint) == 2);
    CHECK(codePoint == 0xe9);
    CHECK(prWtf8DecodeScalar("a", 1, &codePoint) == 0);
}

TEST_CASE("validating stops at the first ill-formed sequence") {
    auto validate = [](PrStringView text) {
        return prUtf8Validate(text);
    };
    CHECK(validate("") == 0);
    CHECK(validate("plain ASCII that runs past sixteen bytes") == 40);
    CHECK(validate("caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80") == 14);
    CHECK(validate("ab\xed\xa0\x80") == 2);   // a surrogate
    CHECK(validate("0123456789abcdef\x80") == 16);
    CHECK(validate("ab\xe2\x82") == 2);
    CHECK(validate("ab\xc0\xaf" "cd") == 2);
}

TEST_CASE("WTF-8 validates lone surrogates, and stops at a split pair") {
    CHECK(prWtf8Validate("ab\xed\xa0\x80") == 5);
    CHECK(prWtf8Validate("ab\xed\xb0\x80\xed\xa0\x80") == 8);
    CHECK(prWtf8Validate("ab\xed\xa0\xbd\xed\xb8\x80") == 2);
    CHECK(prWtf8Validate("ab\xed\xa0\xbd\xed\xb8") == 5);   // the low surrogate is cut short
    CHECK(prWtf8Validate("ab\xc0\xaf") == 2);
}

TEST_CASE("validating finds a sequence wherever it falls in a long text") {
    // A well-formed sequence at each offset of text long enough for several blocks, followed by an ill-formed one.
    struct Form {
        size_t (*validate)(PrStringView text);
        PrStringView sequences[4];
    };
    const Form forms[] = {
        {prUtf8Validate, {"\xc3\xa9", "\xe2\x82\xac", "\xf0\x9f\x98\x80", "\xed\x9f\xbf"}},
        {prWtf8Validate, {"\xc3\xa9", "\xed\xa0\x80", "\xf0\x9f\x98\x80", "\xed\xbf\xbf"}},
    };
    for (const Form& form : forms) {
        for (PrStringView sequence : form.sequences) {
            uint32_t size = uint32_t(sequence.size);
            for (uint32_t at = 0; at < 150; ++at) {
                String text;
                for (uint32_t i = 0; i < 200; ++i)
                    text.append('a');
                text.replace(at, size, sequence);
                CAPTURE(at);
                CHECK(form.validate(text) == text.count());

                text.replace(at + size + 7, 1, "\x80");
                CHECK(form.validate(text) == at + size + 7);

                // If the sequence is cut short, the text stops being well formed at the sequence.
                text.replace(at, size, format("{}a", prStringSlice(sequence, 0, size - 1)));
                CHECK(form.validate(text) == at);
            }
        }
    }
    // A surrogate in UTF-8 past the first blocks.
    String text;
    for (uint32_t i = 0; i < 100; ++i)
        text.append('a');
    text.append("\xed\xa0\x80");
    CHECK(prUtf8Validate(text) == 100);
    CHECK(prWtf8Validate(text) == 103);
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
    CHECK(repair("\xed\xa0\x80") == "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd");
    CHECK(repair("a\xf0\x9f\x98") == "a\xef\xbf\xbd");
}

TEST_CASE("a decoder of bytes one at a time agrees with decoding the whole text") {
    // Returns the code points that a decoder gives for `text`, with a replacement for a sequence that is incomplete at
    // the end.
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
            at += prUtf8Decode(text, at, &codePoint);
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

TEST_CASE("a surrogate and a value out of range encode as U+FFFD in UTF-8") {
    CHECK(encode(0xd800) == "\xef\xbf\xbd");
    CHECK(encode(0x110000) == "\xef\xbf\xbd");
}

TEST_CASE("a surrogate encodes as its three bytes in WTF-8") {
    CHECK(encodeWtf8(0xd800) == "\xed\xa0\x80");
    CHECK(encodeWtf8(0x1f600) == "\xf0\x9f\x98\x80");
    CHECK(encodeWtf8(0x110000) == "\xef\xbf\xbd");
}

TEST_CASE("a low surrogate appended after a high one joins it as their pair") {
    auto append = [](Span<const uint32_t> codePoints) {
        PrTextBuffer out = {};
        for (uint32_t codePoint : codePoints)
            prTextBufferAppendWtf8CodePoint(&out, codePoint);
        String result(prTextBufferView(&out));
        prTextBufferReset(&out);
        return result;
    };
    const uint32_t pair[] = {0xd83d, 0xde00};
    const uint32_t between[] = {'a', 0xd83d, 0xde00, 'b'};
    const uint32_t reversed[] = {0xde00, 0xd83d};
    const uint32_t lone[] = {0xd83d, 'a'};
    const uint32_t twoHighs[] = {0xd83d, 0xd83d, 0xde00};
    CHECK(append(pair) == "\xf0\x9f\x98\x80");
    CHECK(append(between) == "a\xf0\x9f\x98\x80" "b");
    CHECK(append(reversed) == "\xed\xb8\x80\xed\xa0\xbd");
    CHECK(append(lone) == "\xed\xa0\xbd" "a");
    CHECK(prWtf8Validate(append(twoHighs)) == 7);
}

TEST_CASE("WTF-8 repair keeps lone surrogates, replaces ill-formed sequences and joins split pairs") {
    auto repair = [](PrStringView before, PrStringView text) {
        PrTextBuffer out = {};
        prTextBufferAppend(&out, before);
        prTextBufferAppendWtf8(&out, text);
        String result(prTextBufferView(&out));
        prTextBufferReset(&out);
        return result;
    };
    CHECK(repair("", "a\xed\xa0\x80" "b") == "a\xed\xa0\x80" "b");
    CHECK(repair("", "a\xe2\x82" "b") == "a\xef\xbf\xbd" "b");
    CHECK(repair("", "\xed\xa0\xbd\xed\xb8\x80") == "\xf0\x9f\x98\x80");
    CHECK(repair("a\xed\xa0\xbd", "\xed\xb8\x80" "b") == "a\xf0\x9f\x98\x80" "b");
    CHECK(repair("\xed\xb8\x80", "\xed\xa0\xbd") == "\xed\xb8\x80\xed\xa0\xbd");
}

TEST_CASE("WTF-8 converts to UTF-8 with one U+FFFD for each lone surrogate, and UTF-8 converts to itself") {
    PrTextBuffer scratch = {};
    PrStringView utf8 = "caf\xc3\xa9";
    CHECK(prTextBufferUtf8FromWtf8(&scratch, utf8).data == utf8.data);
    CHECK(String(prTextBufferUtf8FromWtf8(&scratch, "a\xed\xa0\x80" "b")) == "a\xef\xbf\xbd" "b");
    CHECK(String(prTextBufferUtf8FromWtf8(&scratch, "\xed\xa0\xbd\xed\xb8\x80")) == "\xf0\x9f\x98\x80");
    CHECK(String(prTextBufferUtf8FromWtf8(&scratch, "a\xe2\x82")) == "a\xef\xbf\xbd");
    prTextBufferReset(&scratch);
}

TEST_CASE("UTF-16 converts to WTF-8 and back, lone surrogates and all") {
    const uint16_t units[] = {'a', 0xe9, 0xd83d, 0xde00, 0xd800, 'b', 0xdc00};
    char bytes[32];
    uint32_t size = prWtf8FromUtf16(units, 7, nullptr, 0);
    CHECK(size == 1 + 2 + 4 + 3 + 1 + 3);
    CHECK(prWtf8FromUtf16(units, 7, bytes, sizeof(bytes)) == size);
    CHECK(PrStringView{bytes, size} == "a\xc3\xa9\xf0\x9f\x98\x80\xed\xa0\x80" "b\xed\xb0\x80");
    uint16_t back[8] = {};
    CHECK(prWtf8ToUtf16(PrStringView{bytes, size}, back, 8) == 7);
    CHECK(prMemEquals(back, units, sizeof(units)));
    CHECK(prWtf8ToUtf16("a\xe2\x82", back, 8) == kPrWtf8Invalid);
    CHECK(prWtf8FromUtf16(nullptr, 0, nullptr, 0) == 0);
}
