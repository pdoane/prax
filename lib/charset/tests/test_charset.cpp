#include "pr/charset.h"
#include "pr/utf8.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

using namespace std::string_literals;

extern "C" bool prTestCharsetC(void);

namespace pr_impl {

namespace {

// The text of `bytes` decoded whole by a new decoder of `charset`, or "error" if a fatal decoder fails.
std::string decoded(PrCharset charset, const std::string& bytes, PrCharsetDecoderFlags flags = 0) {
    PrCharsetDecoder decoder = prCharsetDecoderMake(charset, flags);
    PrRawString out{};
    PrStringView text = prCharsetDecodeView(&decoder, {bytes.data(), bytes.size()}, PrCharsetInput_Final, &out);
    std::string result = text.data == nullptr ? "error" : std::string(text.data, text.size);
    prRawStringReset(&out);
    return result;
}

// The text of `bytes` decoded in parts of `step` bytes, through prCharsetDecode, which checks the bound that
// prCharsetDecodeMaxSize gives.
std::string decodedInParts(PrCharset charset, const std::string& bytes, size_t step) {
    PrCharsetDecoder decoder = prCharsetDecoderMake(charset, 0);
    std::string result;
    size_t at = 0;
    do {
        size_t size = bytes.size() - at < step ? bytes.size() - at : step;
        PrCharsetInput input = at + size == bytes.size() ? PrCharsetInput_Final : PrCharsetInput_Partial;
        size_t capacity = prCharsetDecodeMaxSize(&decoder, size);
        std::vector<char> out(capacity + 1, '!');
        size_t written = 0;
        CHECK(prCharsetDecode(&decoder, {bytes.data() + at, size}, input, out.data(), &written));
        CHECK(written <= capacity);
        CHECK(out[capacity] == '!');
        result.append(out.data(), written);
        at += size;
    } while (at < bytes.size());
    return result;
}

// Returns true if decoding `bytes` gives a view of them, starting `offset` bytes in, without copying them.
bool decodesInPlace(PrCharset charset, const std::string& bytes, size_t offset = 0) {
    PrCharsetDecoder decoder = prCharsetDecoderMake(charset, 0);
    PrRawString out{};
    PrStringView text = prCharsetDecodeView(&decoder, {bytes.data(), bytes.size()}, PrCharsetInput_Final, &out);
    bool inPlace = prRawStringIsNull(&out) && text.data == bytes.data() + offset && text.size == bytes.size() - offset;
    prRawStringReset(&out);
    return inPlace;
}

// The bytes of `text` encoded whole by a new encoder of `charset`, with each code point that the charset cannot encode
// written as "&#N;" between the bytes around it.
std::string encoded(PrCharset charset, const std::string& text) {
    PrCharsetEncoder encoder = prCharsetEncoderMake(charset);
    std::string result;
    size_t at = 0;
    for (;;) {
        std::vector<char> out(prCharsetEncodeMaxSize(&encoder, text.size() - at));
        size_t size = 0;
        uint32_t unencoded = 0;
        at += prCharsetEncode(&encoder, {text.data() + at, text.size() - at}, PrCharsetInput_Final, out.data(), &size,
            &unencoded);
        CHECK(size <= out.size());
        result.append(out.data(), size);
        if (unencoded == kPrCharsetUnencodedNone)
            return result;
        result += "&#" + std::to_string(unencoded) + ";";
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("labels name their charsets, ignoring case and surrounding whitespace") {
    CHECK(prCharsetFromLabel("utf-8") == PrCharset_Utf8);
    CHECK(prCharsetFromLabel(" \t UTF8\n") == PrCharset_Utf8);
    CHECK(prCharsetFromLabel("Latin1") == PrCharset_Windows1252);
    CHECK(prCharsetFromLabel("ascii") == PrCharset_Windows1252);
    CHECK(prCharsetFromLabel("iso-8859-1") == PrCharset_Windows1252);
    CHECK(prCharsetFromLabel("csiso58gb231280") == PrCharset_Gbk);
    CHECK(prCharsetFromLabel("iso-2022-kr") == PrCharset_Replacement);
    CHECK(prCharsetFromLabel("unicodefffe") == PrCharset_Utf16Be);
    CHECK(prCharsetFromLabel("utf-16") == PrCharset_Utf16Le);
    CHECK(prCharsetFromLabel("x-user-defined") == PrCharset_XUserDefined);
    CHECK(prCharsetFromLabel("utf-7") == PrCharset_None);
    CHECK(prCharsetFromLabel("utf-8 x") == PrCharset_None);
    CHECK(prCharsetFromLabel("") == PrCharset_None);
    CHECK(prCharsetFromLabel(PrStringView{}) == PrCharset_None);
}

TEST_CASE("every charset's name is one of its labels") {
    CHECK(prCharsetName(PrCharset_None).data == nullptr);
    CHECK(prCharsetName(PrCharset_ShiftJis) == "Shift_JIS");
    CHECK(prCharsetName(PrCharset_Windows1252) == "windows-1252");
    for (PrCharset charset = PrCharset_None + 1; charset < PrCharset_Count; ++charset)
        CHECK(prCharsetFromLabel(prCharsetName(charset)) == charset);
}

TEST_CASE("charsets without an encoder encode as UTF-8") {
    CHECK(prCharsetOutput(PrCharset_Utf16Le) == PrCharset_Utf8);
    CHECK(prCharsetOutput(PrCharset_Utf16Be) == PrCharset_Utf8);
    CHECK(prCharsetOutput(PrCharset_Replacement) == PrCharset_Utf8);
    CHECK(prCharsetOutput(PrCharset_ShiftJis) == PrCharset_ShiftJis);
}

TEST_CASE("a byte order mark names its charset") {
    CHECK(prCharsetSniffBom("\xEF\xBB\xBFx") == PrCharset_Utf8);
    CHECK(prCharsetSniffBom("\xFE\xFF") == PrCharset_Utf16Be);
    CHECK(prCharsetSniffBom("\xFF\xFE\x00") == PrCharset_Utf16Le);
    CHECK(prCharsetSniffBom("\xEF\xBB") == PrCharset_None);
    CHECK(prCharsetSniffBom("") == PrCharset_None);
}

TEST_CASE("the start of a stream decides its byte order mark once it is as long as the mark that it could start") {
    CHECK_FALSE(prCharsetIsBomDecided(""));
    CHECK_FALSE(prCharsetIsBomDecided("\xEF\xBB"));
    CHECK_FALSE(prCharsetIsBomDecided("\xFE"));
    CHECK(prCharsetIsBomDecided("\xFE\xFF"));
    CHECK(prCharsetIsBomDecided("\xEF\xBB\xBF"));
    CHECK(prCharsetIsBomDecided("<"));
}

TEST_CASE("UTF-8 decodes well-formed text in place") {
    CHECK(decodesInPlace(PrCharset_Utf8, "plain ascii"));
    CHECK(decodesInPlace(PrCharset_Utf8, "caf\xC3\xA9 \xE4\xB8\xAD \xF0\x9F\x98\x80"));
    CHECK(decodesInPlace(PrCharset_Utf8, "\xEF\xBB\xBFwith a mark", 3));
    CHECK(decodesInPlace(PrCharset_Utf8, ""));
    CHECK_FALSE(decodesInPlace(PrCharset_Utf8, "caf\xE9"));
    CHECK(decoded(PrCharset_Utf8, "\xEF\xBB\xBFx", PrCharsetDecoderFlag_KeepBom) == "\xEF\xBB\xBFx");
    // Only the first mark is removed.
    CHECK(decoded(PrCharset_Utf8, "\xEF\xBB\xBF\xEF\xBB\xBFx") == "\xEF\xBB\xBFx");
}

TEST_CASE("UTF-8 replaces each maximal subpart of an ill-formed sequence") {
    CHECK(decoded(PrCharset_Utf8, "a\xFF" "b") == "a\xEF\xBF\xBD" "b");
    CHECK(decoded(PrCharset_Utf8, "\xF0\x80\x80") == "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
    CHECK(decoded(PrCharset_Utf8, "\xE2\x82") == "\xEF\xBF\xBD");
    CHECK(decoded(PrCharset_Utf8, "\xE2\x82x") == "\xEF\xBF\xBDx");
    // A surrogate's three bytes are ill-formed.
    CHECK(decoded(PrCharset_Utf8, "\xED\xA0\x80") == "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
    CHECK(decoded(PrCharset_Utf8, "\xF4\x90\x80\x80") == "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
    CHECK(decoded(PrCharset_Utf8, "\xC0\xAF") == "\xEF\xBF\xBD\xEF\xBF\xBD");
}

TEST_CASE("a sequence that spans two parts decodes as one") {
    std::string text = "\xE2\x82\xAC \xF0\x9F\x98\x80 \xEF\xBB\xBF";
    for (size_t step = 1; step <= text.size(); ++step)
        CHECK(decodedInParts(PrCharset_Utf8, text, step) == text);
    CHECK(decodedInParts(PrCharset_Utf8, "\xEF\xBB\xBFx", 1) == "x");
}

TEST_CASE("a fatal decoder fails at the first error") {
    CHECK(decoded(PrCharset_Utf8, "ok\xFF", PrCharsetDecoderFlag_Fatal) == "error");
    CHECK(decoded(PrCharset_Utf8, "\xE2\x82", PrCharsetDecoderFlag_Fatal) == "error");
    CHECK(decoded(PrCharset_Utf8, "ok", PrCharsetDecoderFlag_Fatal) == "ok");
    CHECK(decoded(PrCharset_Iso88598, "\xA1", PrCharsetDecoderFlag_Fatal) == "error");
    CHECK(decoded(PrCharset_Utf16Le, "A", PrCharsetDecoderFlag_Fatal) == "error");
}

TEST_CASE("single-byte charsets decode ASCII in place and the rest through their index") {
    CHECK(decodesInPlace(PrCharset_Windows1252, "only ascii, long enough to cross a block of sixteen"));
    CHECK(decoded(PrCharset_Windows1252, "caf\xE9") == "caf\xC3\xA9");
    CHECK(decoded(PrCharset_Windows1252, "\x80\x81\x9F") == "\xE2\x82\xAC\xC2\x81\xC5\xB8");
    // A byte order mark is text in a charset other than UTF-8.
    CHECK(decoded(PrCharset_Windows1252, "\xEF\xBB\xBF") == "\xC3\xAF\xC2\xBB\xC2\xBF");
    CHECK(decoded(PrCharset_Koi8R, "\xC1") == "\xD0\xB0");
    CHECK(decoded(PrCharset_Iso88598, "\xA1") == "\xEF\xBF\xBD");
    CHECK(decoded(PrCharset_Iso88598I, "\xE0") == "\xD7\x90");
    CHECK(decoded(PrCharset_XUserDefined, "a\x80\xFF") == "a\xEF\x9E\x80\xEF\x9F\xBF");
}

TEST_CASE("the Chinese charsets decode their indexes") {
    CHECK(decoded(PrCharset_Gbk, "\xD6\xD0") == "\xE4\xB8\xAD");
    CHECK(decoded(PrCharset_Gb18030, "\x80") == "\xE2\x82\xAC");
    CHECK(decoded(PrCharset_Gb18030, "\x81\x30\x81\x30") == "\xC2\x80");
    CHECK(decoded(PrCharset_Gb18030, "\x84\x31\xA4\x39") == "\xEF\xBF\xBF");
    CHECK(decoded(PrCharset_Gb18030, "\x90\x30\x81\x30") == "\xF0\x90\x80\x80");
    // The bytes after an invalid lead are read again.
    CHECK(decoded(PrCharset_Gb18030, "\x81\x30\x41") == "\xEF\xBF\xBD" "0A");
    CHECK(decoded(PrCharset_Gb18030, "\x81\x30\x81\x41") == "\xEF\xBF\xBD" "0\xE4\xB8\x84");
    CHECK(decoded(PrCharset_Gb18030, "\x81") == "\xEF\xBF\xBD");
    CHECK(decoded(PrCharset_Big5, "\xA4\x40") == "\xE4\xB8\x80");
    CHECK(decoded(PrCharset_Big5, "\x88\x62") == "\xC3\x8A\xCC\x84");
    CHECK(decoded(PrCharset_Big5, "\x87\x40") == "\xE4\x8F\xB0");
    CHECK(decoded(PrCharset_Big5, "\x80") == "\xEF\xBF\xBD");
    CHECK(decoded(PrCharset_Big5, "\xA4 ") == "\xEF\xBF\xBD ");
}

TEST_CASE("the Japanese and Korean charsets decode their indexes") {
    CHECK(decoded(PrCharset_EucJp, "\xA4\xA2") == "\xE3\x81\x82");
    CHECK(decoded(PrCharset_EucJp, "\x8E\xB1") == "\xEF\xBD\xB1");
    CHECK(decoded(PrCharset_EucJp, "\x8F\xB0\xA1") == "\xE4\xB8\x82");
    CHECK(decoded(PrCharset_ShiftJis, "\x82\xA0") == "\xE3\x81\x82");
    CHECK(decoded(PrCharset_ShiftJis, "\xB1\x80") == "\xEF\xBD\xB1\xC2\x80");
    CHECK(decoded(PrCharset_ShiftJis, "\xF0\x40") == "\xEE\x80\x80");
    CHECK(decoded(PrCharset_ShiftJis, "\x82") == "\xEF\xBF\xBD");
    CHECK(decoded(PrCharset_EucKr, "\xB0\xA1") == "\xEA\xB0\x80");
    CHECK(decoded(PrCharset_EucKr, "\xB0 ") == "\xEF\xBF\xBD ");
}

TEST_CASE("ISO-2022-JP switches its state at each escape") {
    CHECK(decoded(PrCharset_Iso2022Jp, "a\x1B$B\x24\x22\x1B(Bb") == "a\xE3\x81\x82" "b");
    CHECK(decoded(PrCharset_Iso2022Jp, "\x1B(J\x5C\x7E") == "\xC2\xA5\xE2\x80\xBE");
    CHECK(decoded(PrCharset_Iso2022Jp, "\x1B(I\x31") == "\xEF\xBD\xB1");
    // Two escapes in a row are an error, and so is an escape that the decoder does not know.
    CHECK(decoded(PrCharset_Iso2022Jp, "\x1B(B\x1B(Ba") == "\xEF\xBF\xBD" "a");
    CHECK(decoded(PrCharset_Iso2022Jp, "\x1B(Xa") == "\xEF\xBF\xBD(Xa");
    CHECK(decoded(PrCharset_Iso2022Jp, "\x0E") == "\xEF\xBF\xBD");
    CHECK(decoded(PrCharset_Iso2022Jp, "\x1B") == "\xEF\xBF\xBD");
    CHECK(decoded(PrCharset_Iso2022Jp, "\x1B$") == "\xEF\xBF\xBD$");
    CHECK(decoded(PrCharset_Iso2022Jp, "\x1B$B\x24") == "\xEF\xBF\xBD");
    CHECK(decodesInPlace(PrCharset_Iso2022Jp, "ascii"));
}

TEST_CASE("UTF-16 pairs its surrogates and replaces the unpaired") {
    CHECK(decoded(PrCharset_Utf16Le, "A\x00"s) == "A");
    CHECK(decoded(PrCharset_Utf16Be, "\x00" "A"s) == "A");
    CHECK(decoded(PrCharset_Utf16Le, "\xFF\xFE" "A\x00"s) == "A");
    CHECK(decoded(PrCharset_Utf16Le, "\xFF\xFE" "A\x00"s, PrCharsetDecoderFlag_KeepBom) == "\xEF\xBB\xBF" "A");
    CHECK(decoded(PrCharset_Utf16Le, "\x3D\xD8\x00\xDE"s) == "\xF0\x9F\x98\x80");
    CHECK(decoded(PrCharset_Utf16Be, "\xD8\x3D\xDE\x00"s) == "\xF0\x9F\x98\x80");
    CHECK(decoded(PrCharset_Utf16Le, "\x00\xD8" "A\x00"s) == "\xEF\xBF\xBD" "A");
    CHECK(decoded(PrCharset_Utf16Le, "\x00\xD8\x00\xD8\x00\xDC"s) == "\xEF\xBF\xBD\xF0\x90\x80\x80");
    CHECK(decoded(PrCharset_Utf16Le, "\x00\xDC"s) == "\xEF\xBF\xBD");
    CHECK(decoded(PrCharset_Utf16Le, "\x00\xD8"s) == "\xEF\xBF\xBD");
    CHECK(decoded(PrCharset_Utf16Le, "A\x00" "B"s) == "A\xEF\xBF\xBD");
}

TEST_CASE("replacement decodes any input but the empty one to one U+FFFD") {
    CHECK(decoded(PrCharset_Replacement, "") == "");
    CHECK(decoded(PrCharset_Replacement, "anything") == "\xEF\xBF\xBD");
    CHECK(decodedInParts(PrCharset_Replacement, "anything", 2) == "\xEF\xBF\xBD");
}

TEST_CASE("every charset decodes the same in parts as whole, within the bound") {
    uint32_t state = 12345;
    std::string bytes;
    for (uint32_t i = 0; i < 600; ++i) {
        state = state * 1103515245 + 12345;
        // Mostly bytes that start or continue sequences, with ASCII and escapes among them.
        uint32_t r = state >> 16;
        bytes += char(r % 5 == 0 ? 0x1b : r % 7 == 0 ? 0x24 + (r >> 8) % 6 : 0x20 + (r >> 4) % 0xe0);
    }
    for (PrCharset charset = PrCharset_None + 1; charset < PrCharset_Count; ++charset) {
        std::string name(prCharsetName(charset).data, prCharsetName(charset).size);
        CAPTURE(name);
        std::string whole = decoded(charset, bytes);
        CHECK(decodedInParts(charset, bytes, bytes.size()) == whole);
        for (size_t step : {size_t(1), size_t(2), size_t(3), size_t(5), size_t(17)})
            CHECK(decodedInParts(charset, bytes, step) == whole);
    }
}

TEST_CASE("each charset encodes the code points of its index") {
    CHECK(encoded(PrCharset_Utf8, "a\xC3\xA9") == "a\xC3\xA9");
    CHECK(encoded(PrCharset_Windows1252, "caf\xC3\xA9 \xE2\x82\xAC") == "caf\xE9 \x80");
    CHECK(encoded(PrCharset_Windows1252, "a\xE4\xB8\xAD" "b") == "a&#20013;b");
    CHECK(encoded(PrCharset_XUserDefined, "\xEF\x9E\x80") == "\x80");
    CHECK(encoded(PrCharset_Gbk, "\xE4\xB8\xAD\xE2\x82\xAC") == "\xD6\xD0\x80");
    CHECK(encoded(PrCharset_Gb18030, "\xE2\x82\xAC\xC2\x80\xF0\x90\x80\x80") ==
        "\xA2\xE3\x81\x30\x81\x30\x90\x30\x81\x30");
    CHECK(encoded(PrCharset_Gbk, "\xF0\x90\x80\x80") == "&#65536;");
    CHECK(encoded(PrCharset_Gb18030, "\xEE\x97\xA5") == "&#58853;");
    CHECK(encoded(PrCharset_Gb18030, "\xEE\x9E\x8D") == "\xA6\xD9");
    CHECK(encoded(PrCharset_Big5, "\xE4\xB8\x80\xE2\x95\x90") == "\xA4\x40\xF9\xF9");
    CHECK(encoded(PrCharset_EucJp, "\xE3\x81\x82\xEF\xBD\xB1\xC2\xA5") == "\xA4\xA2\x8E\xB1\x5C");
    CHECK(encoded(PrCharset_ShiftJis, "\xE3\x81\x82\xEF\xBD\xB1\xC2\x80") == "\x82\xA0\xB1\x80");
    // U+2252 is in jis0208 among the pointers that Shift_JIS skips, and again at a pointer that it encodes with.
    CHECK(encoded(PrCharset_ShiftJis, "\xE2\x89\x92") == "\x81\xE0");
    CHECK(encoded(PrCharset_EucKr, "\xEA\xB0\x80") == "\xB0\xA1");
}

TEST_CASE("ISO-2022-JP escapes to the state that each code point needs, and back to ASCII at the end") {
    CHECK(encoded(PrCharset_Iso2022Jp, "a\xE3\x81\x82" "b") == "a\x1B$B\x24\x22\x1B(Bb");
    CHECK(encoded(PrCharset_Iso2022Jp, "\xC2\xA5") == "\x1B(J\x5C\x1B(B");
    CHECK(encoded(PrCharset_Iso2022Jp, "\xEF\xBD\xB1") == "\x1B$B\x25\x22\x1B(B");
    CHECK(encoded(PrCharset_Iso2022Jp, "\xE3\x81\x82\xE4\xB8\x82") == "\x1B$B\x24\x22\x1B(B&#19970;");
    // An escape in the text would change the decoder's state, so it is an error, reported as U+FFFD.
    CHECK(encoded(PrCharset_Iso2022Jp, "\x1B") == "&#65533;");
}

TEST_CASE("what a charset decodes from one or two bytes encodes back to bytes that decode the same") {
    for (PrCharset charset = PrCharset_None + 1; charset < PrCharset_Count; ++charset) {
        if (prCharsetOutput(charset) != charset || charset == PrCharset_Utf8 || charset == PrCharset_Iso2022Jp)
            continue;
        std::string name(prCharsetName(charset).data, prCharsetName(charset).size);
        CAPTURE(name);
        uint32_t checked = 0;
        for (uint32_t first = 0x80; first < 0x100; ++first) {
            for (uint32_t second = 0; second < 0x100; second += first < 0x81 ? 0x100 : 1) {
                std::string bytes(1, char(first));
                if (first >= 0x81)
                    bytes += char(second);
                std::string text = decoded(charset, bytes);
                uint32_t codePoint = 0;
                // Only bytes that decode whole, to one code point, round trip.
                if (prUtf8Decode({text.data(), text.size()}, 0, &codePoint) != text.size() ||
                        codePoint == kPrUtf8Replacement)
                    continue;
                std::string back = encoded(charset, text);
                CAPTURE(text);
                if (back.find("&#") != std::string::npos) {
                    // Big5 does not encode its Hong Kong extensions, gb18030 does not encode U+E5E5, and Shift_JIS does
                    // not encode its user-defined area.
                    CHECK((charset == PrCharset_Big5 || charset == PrCharset_Gb18030 || charset == PrCharset_Gbk ||
                        (charset == PrCharset_ShiftJis && codePoint >= 0xe000 && codePoint < 0xe758)));
                    continue;
                }
                CHECK(decoded(charset, back) == text);
                ++checked;
            }
        }
        CHECK(checked > 0);
    }
}

TEST_CASE("charsets work through the C API") {
    CHECK(prTestCharsetC());
}

TEST_CASE("a MIME type's charset parameter names its charset, where the type parses and the label is one") {
    CHECK(prCharsetFromMimeType("text/html; charset=Shift_JIS") == PrCharset_ShiftJis);
    CHECK(prCharsetFromMimeType("text/css;charset=\"utf-8\"; charset=latin1") == PrCharset_Utf8);
    CHECK(prCharsetFromMimeType("text/plain; charset=nonsense") == PrCharset_None);
    CHECK(prCharsetFromMimeType("text/plain") == PrCharset_None);
    CHECK(prCharsetFromMimeType("text; charset=utf-8") == PrCharset_None);
}
