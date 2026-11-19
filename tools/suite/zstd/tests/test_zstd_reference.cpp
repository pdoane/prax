// lib/zstd's decoder beside the reference decoder, over frames that the reference encoder writes: every level, with and
// without checksums and content sizes, in small windows and with long-distance matching, of data that the encoder codes
// differently, at sizes from nothing to several blocks. The format is lossless, so each decodes to what was encoded,
// whole and a piece at a time. And the other way: the reference decoder over the frames that lib/zstd's encoder writes
// at each of its levels, of the same data.

#include "pr/array.h"
#include "pr/mem.h"
#include "pr/zstd.h"

#include "tools/harness/test.h"
#include "zstd.h"

#include <chrono>

using namespace pr;

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

// A sequence of pseudo-random numbers, the same on every run.
struct Sequence {
    uint32_t state = 0x2545f491u;

    uint32_t next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
};

// The kinds of data that an encoder chooses differently for.
enum {
    Kind_Words,     // words of a small vocabulary: matches and Huffman coded literals
    Kind_Smooth,    // bytes that change little from one to the next: literals of few symbols, and few matches
    Kind_Random,    // bytes at random, which are stored
    Kind_Constant,  // one byte, which is a run
    Kind_Mixed,     // a stretch of each, and the first again from far back
    Kind_Count,
};
typedef uint8_t Kind;

void appendKind(Array<uint8_t>& data, Kind kind, uint32_t size, Sequence* random) {
    const char* const kWords[] = {
        "alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta", "iota", "kappa",
        "lambda", "mu", "nu", "xi", "omicron", "pi",
    };
    uint32_t end = data.count() + size;
    uint8_t smooth = 128;
    while (data.count() < end) {
        if (kind == Kind_Words) {
            for (const char* at = kWords[random->next() % 16]; *at != 0 && data.count() < end; ++at)
                data.push(uint8_t(*at));
            if (data.count() < end)
                data.push(uint8_t(random->next() % 9 == 0 ? '\n' : ' '));
        } else if (kind == Kind_Smooth) {
            smooth = uint8_t(smooth + random->next() % 5 - 2);
            data.push(smooth);
        } else if (kind == Kind_Random) {
            data.push(uint8_t(random->next()));
        } else {
            data.push(0x5a);
        }
    }
}

Array<uint8_t> makeData(Kind kind, uint32_t size, Sequence* random) {
    Array<uint8_t> data;
    if (kind != Kind_Mixed) {
        appendKind(data, kind, size, random);
        return data;
    }
    for (Kind part = Kind_Words; part < Kind_Mixed; ++part)
        appendKind(data, part, size / 5, random);
    // The start again, which only a long window reaches.
    uint32_t rest = size - data.count();
    for (uint32_t i = 0; i < rest; ++i) {
        uint8_t byte = i < data.count() ? data[i] : uint8_t(0);
        data.push(byte);
    }
    return data;
}

// Decodes `frames` with lib/zstd at once into room for `capacity` bytes.
Outcome decodeWhole(const Array<uint8_t>& frames, uint32_t capacity, Array<uint8_t>* out) {
    out->resize(capacity);
    PrZstdDecoder* decoder = prZstdDecoderCreate(kPrZstdDefaultWindowLimit);
    size_t consumed = 0;
    size_t written = 0;
    Outcome result = readOutcome(decoder, PrByteView{frames.data(), frames.count()}, true, &consumed,
        capacity != 0 ? out->data() : nullptr, capacity, &written);
    prZstdDecoderDestroy(decoder);
    out->resize(uint32_t(written));
    return result;
}

// Decodes `frames` with lib/zstd, `piece` bytes of input at a time into room for `room` bytes.
Outcome decodePieces(const Array<uint8_t>& frames, size_t piece, size_t room, Array<uint8_t>* out) {
    PrZstdDecoder* decoder = prZstdDecoderCreate(kPrZstdDefaultWindowLimit);
    Array<uint8_t> buffer;
    buffer.resize(uint32_t(room));
    out->clear();
    Outcome result = Outcome_Input;
    size_t at = 0;
    for (;;) {
        size_t size = frames.count() - at < piece ? frames.count() - at : piece;
        size_t consumed = 0;
        size_t written = 0;
        result = readOutcome(decoder, PrByteView{frames.data() + at, size}, at + size == frames.count(),
            &consumed, buffer.data(), room, &written);
        at += consumed;
        for (size_t i = 0; i < written; ++i)
            out->push(buffer[uint32_t(i)]);
        if (result != Outcome_Input && result != Outcome_Output)
            break;
    }
    prZstdDecoderDestroy(decoder);
    return result;
}

bool same(const Array<uint8_t>& a, const Array<uint8_t>& b) {
    return a.count() == b.count() && (a.count() == 0 || prMemEquals(a.data(), b.data(), a.count()));
}

// What the reference encoder is asked for besides a level.
struct Options {
    bool checksum;
    bool contentSize;
    int windowLog;      // zero for the level's own
    bool longDistance;
};

Array<uint8_t> encode(const Array<uint8_t>& data, int level, const Options& options) {
    ZSTD_CCtx* context = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, level);
    ZSTD_CCtx_setParameter(context, ZSTD_c_checksumFlag, options.checksum ? 1 : 0);
    ZSTD_CCtx_setParameter(context, ZSTD_c_contentSizeFlag, options.contentSize ? 1 : 0);
    if (options.windowLog != 0)
        ZSTD_CCtx_setParameter(context, ZSTD_c_windowLog, options.windowLog);
    if (options.longDistance)
        ZSTD_CCtx_setParameter(context, ZSTD_c_enableLongDistanceMatching, 1);
    Array<uint8_t> frame;
    frame.resize(uint32_t(ZSTD_compressBound(data.count())));
    size_t size = ZSTD_compress2(context, frame.data(), frame.count(), data.data(), data.count());
    REQUIRE(!ZSTD_isError(size));
    frame.resize(uint32_t(size));
    ZSTD_freeCCtx(context);
    return frame;
}

// The seconds that each decoder has taken to decode whole frames, and the bytes that they decoded to.
struct Timing {
    double ours;
    double reference;
    uint64_t bytes;
};
Timing gTiming;

double seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Checks that lib/zstd decodes `frame` to `data`, as the reference decoder does.
void checkFrame(const Array<uint8_t>& frame, const Array<uint8_t>& data, bool pieces) {
    Array<uint8_t> reference;
    reference.resize(data.count());
    double start = seconds();
    size_t size = ZSTD_decompress(reference.data(), reference.count(), frame.data(), frame.count());
    double middle = seconds();
    REQUIRE(!ZSTD_isError(size));
    reference.resize(uint32_t(size));
    REQUIRE(same(reference, data));
    Array<uint8_t> decoded;
    Outcome result = decodeWhole(frame, data.count(), &decoded);
    gTiming.reference += middle - start;
    gTiming.ours += seconds() - middle;
    gTiming.bytes += data.count();
    CHECK(result == Outcome_Done);
    CHECK(same(decoded, reference));
    if (!pieces)
        return;
    CHECK(decodePieces(frame, 4093, 61, &decoded) == Outcome_Done);
    CHECK(same(decoded, reference));
    CHECK(decodePieces(frame, 7, 100000, &decoded) == Outcome_Done);
    CHECK(same(decoded, reference));
}

} // namespace

TEST_CASE("zstd decodes what the reference encoder writes at every level") {
    const uint32_t kSizes[] = {0, 1, 10, 1000, 70000, 300000};
    Sequence random;
    uint32_t frames = 0;
    for (Kind kind = 0; kind < Kind_Count; ++kind) {
        for (uint32_t size : kSizes) {
            Array<uint8_t> data = makeData(kind, size, &random);
            for (int level = -5; level <= 22; ++level) {
                if (level == 0)
                    continue;
                CAPTURE(kind);
                CAPTURE(size);
                CAPTURE(level);
                Options options{.checksum = level % 2 == 0, .contentSize = level % 3 != 0};
                checkFrame(encode(data, level, options), data, size <= 70000 || level == 3);
                ++frames;
            }
        }
    }
    MESSAGE("frames: " << frames);
}

TEST_CASE("zstd decodes small windows, long-distance matches and frames one after another") {
    const int kWindowLogs[] = {10, 14, 17, 21};
    Sequence random;
    Array<uint8_t> all;
    Array<uint8_t> stream;
    for (Kind kind = 0; kind < Kind_Count; ++kind) {
        Array<uint8_t> data = makeData(kind, 1500000, &random);
        for (int windowLog : kWindowLogs) {
            CAPTURE(kind);
            CAPTURE(windowLog);
            Options options{.checksum = true, .contentSize = false, .windowLog = windowLog};
            Array<uint8_t> frame = encode(data, 5, options);
            checkFrame(frame, data, windowLog == 10);
            if (windowLog == 14) {
                for (uint8_t byte : frame)
                    stream.push(byte);
                for (uint8_t byte : data)
                    all.push(byte);
            }
        }
        Options distant{.checksum = true, .contentSize = true, .windowLog = 24, .longDistance = true};
        checkFrame(encode(data, 19, distant), data, false);
    }
    Array<uint8_t> decoded;
    CHECK(decodeWhole(stream, all.count(), &decoded) == Outcome_Done);
    CHECK(same(decoded, all));
    // An output a byte short of the stream.
    CHECK(decodeWhole(stream, all.count() - 1, &decoded) == Outcome_Output);
    CHECK(decodePieces(stream, 65536, 65536, &decoded) == Outcome_Done);
    CHECK(same(decoded, all));
    MESSAGE("megabytes a second, over every frame of this program: " << double(gTiming.bytes) / 1e6 / gTiming.ours <<
        ", the reference's: " << double(gTiming.bytes) / 1e6 / gTiming.reference);
}

TEST_CASE("the reference decoder decodes what lib/zstd encodes at every level") {
    const uint32_t kSizes[] = {0, 1, 10, 1000, 70000, 300000, 1500000};
    const PrZstdLevel kLevels[] = {PrZstdLevel_Fast, PrZstdLevel_Default, PrZstdLevel_Small};
    Sequence random;
    uint32_t frames = 0;
    for (Kind kind = 0; kind < Kind_Count; ++kind) {
        for (uint32_t size : kSizes) {
            Array<uint8_t> data = makeData(kind, size, &random);
            for (PrZstdLevel level : kLevels) {
                CAPTURE(kind);
                CAPTURE(size);
                CAPTURE(level);
                Array<uint8_t> frame;
                frame.resize(uint32_t(prZstdEncodeBound(data.count())));
                frame.resize(uint32_t(prZstdEncode(level, PrByteView{data.data(), data.count()}, frame.data())));
                CHECK(ZSTD_getFrameContentSize(frame.data(), frame.count()) == data.count());
                Array<uint8_t> decoded;
                decoded.resize(data.count() + 1);
                size_t got = ZSTD_decompress(decoded.data(), decoded.count(), frame.data(), frame.count());
                REQUIRE(!ZSTD_isError(got));
                decoded.resize(uint32_t(got));
                CHECK(same(decoded, data));
                // And a piece at a time, as a stream.
                ZSTD_DStream* stream = ZSTD_createDStream();
                Array<uint8_t> pieces;
                uint8_t room[4096];
                ZSTD_inBuffer in{frame.data(), 0, 0};
                size_t left = 1;
                while (in.size < frame.count() || left != 0) {
                    in.size = in.size + 777 < frame.count() ? in.size + 777 : frame.count();
                    ZSTD_outBuffer out{room, sizeof(room), 0};
                    left = ZSTD_decompressStream(stream, &out, &in);
                    REQUIRE(!ZSTD_isError(left));
                    for (size_t k = 0; k < out.pos; ++k)
                        pieces.push(room[k]);
                    if (in.pos == frame.count() && left == 0)
                        break;
                }
                ZSTD_freeDStream(stream);
                CHECK(same(pieces, data));
                ++frames;
            }
        }
    }
    MESSAGE("frames: " << frames);
}
