// Zstandard encoding: frames that decode back to their input at every level, whole and in pieces, of inputs that the
// encoder codes differently.

#include "pr/array.h"
#include "pr/string.h"
#include "pr/zstd.h"

#include "tools/harness/test.h"

using namespace pr;

namespace pr_impl {

namespace {

// How a read ended, in one value: its progress if it succeeds, or how it failed.
enum {
    Outcome_Input,
    Outcome_Output,
    Outcome_Done,
    Outcome_Truncated,
    Outcome_Dictionary,
    Outcome_Invalid,
};
typedef uint8_t Outcome;

Outcome outcomeOf(PrStatus status, uint8_t progress) {
    if (status == PrStatus_Truncated)
        return Outcome_Truncated;
    if (status == PrStatus_NotSupported)
        return Outcome_Dictionary;
    if (status != PrStatus_Ok)
        return Outcome_Invalid;
    // The progresses are the first outcomes, in the same order.
    return Outcome(progress);
}

// Reads with `decoder`, as prZstdDecoderRead does, and returns how the read ended.
Outcome readOutcome(PrZstdDecoder* decoder, PrByteView input, bool final, size_t* consumed,
        uint8_t* PR_NULLABLE output, size_t capacity, size_t* written) {
    PrZstdRead read{};
    const PrStatus status = prZstdDecoderRead(decoder, input, final, output, capacity, &read);
    *consumed = read.consumed;
    *written = read.written;
    return outcomeOf(status, read.progress);
}

// Four hundred numbered runs of a phrase: 201,600 bytes.
String repeated() {
    String out;
    for (int i = 0; i < 400; ++i) {
        char number[4] = {char('0' + i / 1000), char('0' + i / 100 % 10), char('0' + i / 10 % 10), char('0' + i % 10)};
        out.append(PrStringView{number, 4});
        for (int k = 0; k < 25; ++k)
            out.append("the quick brown fox ");
    }
    return out;
}

// Bytes of xorshift32 from a seed of one.
String noise(size_t size) {
    Array<char> bytes(size);
    uint32_t x = 1;
    for (size_t i = 0; i < size; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        bytes.data()[i] = char(uint8_t(x));
    }
    return String(PrStringView{bytes.data(), size});
}

// 300,000 bytes of 'a' and 1,000 of 'b'.
String runs() {
    String out;
    out.append(300000, 'a');
    out.append(1000, 'b');
    return out;
}

constexpr PrStringView kText = "It was the best of times, it was the worst of times, it was the age of wisdom, it was the "
    "age of foolishness, it was the epoch of belief, it was the epoch of incredulity, it was the season of Light, it "
    "was the season of Darkness, it was the spring of hope, it was the winter of despair. <html><head><title>The "
    "Page</title></head><body class=\"main\">Welcome to the website, the information you need.</body></html>";

PrByteView bytesOf(PrStringView text) {
    return PrByteView{reinterpret_cast<const uint8_t*>(text.data), text.size};
}

PrByteView view(const Array<uint8_t>& bytes) {
    return PrByteView{bytes.data(), bytes.count()};
}

struct Decoded {
    Outcome result;
    String text;
};

// Decodes `input` in pieces of `inputChunk` bytes into an output of `outputChunk` bytes.
Decoded decode(PrByteView input, size_t inputChunk, size_t outputChunk) {
    PrZstdDecoder* d = prZstdDecoderCreate(kPrZstdDefaultWindowLimit);
    Decoded decoded{Outcome_Input, String()};
    Array<uint8_t> output;
    output.resize(uint32_t(outputChunk));
    size_t at = 0;
    for (;;) {
        size_t size = inputChunk < input.size - at ? inputChunk : input.size - at;
        size_t consumed = 0;
        size_t written = 0;
        decoded.result = readOutcome(d, PrByteView{input.data + at, size}, at + size == input.size, &consumed,
            output.data(), outputChunk, &written);
        decoded.text.append(PrStringView{reinterpret_cast<const char*>(output.data()), written});
        at += consumed;
        if (decoded.result != Outcome_Input && decoded.result != Outcome_Output)
            break;
    }
    prZstdDecoderDestroy(d);
    return decoded;
}

// A mixture: text, noise and runs, in pieces.
String mixed() {
    String n = noise(30000);
    String r = repeated();
    String out;
    for (int i = 0; i < 20; ++i) {
        out.append(PrStringView{r.view().data + i * 9000, 5000});
        out.append(PrStringView{n.view().data + i * 1500, 1200});
        for (int k = 0; k < 50 + 10 * i; ++k)
            out.append(1, char('a' + i));
    }
    return out;
}

constexpr PrZstdLevel kLevels[] = {PrZstdLevel_Fast, PrZstdLevel_Default, PrZstdLevel_Small};

Array<uint8_t> encode(PrZstdLevel level, PrStringView input) {
    Array<uint8_t> packed;
    packed.resize(uint32_t(prZstdEncodeBound(input.size)));
    packed.resize(uint32_t(prZstdEncode(level, bytesOf(input), packed.data())));
    return packed;
}

// Encodes `input` at each level, checks the frame against the bound, and decodes it whole and in pieces.
void roundTrip(PrStringView input) {
    for (PrZstdLevel level : kLevels) {
        CAPTURE(level);
        Array<uint8_t> packed = encode(level, input);
        CHECK(packed.count() <= prZstdEncodeBound(input.size));
        Decoded whole = decode(view(packed), packed.count(), input.size + 1);
        CHECK(whole.result == Outcome_Done);
        CHECK((whole.text == input));
        Decoded pieces = decode(view(packed), 1000, 777);
        CHECK(pieces.result == Outcome_Done);
        CHECK((pieces.text == input));
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("zstd encodes what it decodes back at every level") {
    roundTrip("");
    roundTrip("x");
    roundTrip("abcabcabc");
    roundTrip(kText);
    roundTrip(repeated().view());
    roundTrip(noise(200000).view());
    roundTrip(runs().view());
    roundTrip(mixed().view());
    // Inputs of a block and around it, and of several.
    String r = repeated();
    const size_t kSizes[] = {131071, 131072, 131073, 150000, 201600};
    for (size_t size : kSizes) {
        CAPTURE(size);
        roundTrip(PrStringView{r.view().data, size});
    }
    // Noise repeated after more than Fast's window of 512K, which no match may reach, and after less.
    String n = noise(600000);
    String far = n;
    far.append(PrStringView{n.view().data, 100000});
    far.append(PrStringView{n.view().data + 300000, 100000});
    roundTrip(far.view());
}

TEST_CASE("zstd encodes small inputs in one segment, noise stored and runs as RLE blocks") {
    Array<uint8_t> packed = encode(PrZstdLevel_Default, kText);
    CHECK((packed[4] & 32) != 0);
    CHECK(packed.count() < kText.size);
    // The magic number, the descriptor, a content size of two bytes, a block header, the block and the checksum.
    String n = noise(1000);
    CHECK(encode(PrZstdLevel_Fast, n.view()).count() == 4 + 1 + 2 + 3 + 1000 + 4);
    String a = runs();
    CHECK(encode(PrZstdLevel_Fast, PrStringView{a.view().data, 200000}).count() < 40);
    // A larger input names its window, and states its content size.
    String big = noise(5000000);
    Array<uint8_t> frame = encode(PrZstdLevel_Fast, big.view());
    CHECK((frame[4] & 32) == 0);
    CHECK(prZstdContentSize(view(frame)) == 5000000);
    CHECK(decode(view(frame), 1 << 20, 1 << 16).result == Outcome_Done);
}
