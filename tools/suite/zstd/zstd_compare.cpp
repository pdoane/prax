// The harness of the zstd suite (tools/suite/zstd.py): lib/zstd beside Zstandard's reference library, over data of each
// kind that an encoder codes differently, at sizes from nothing to several blocks.
//
// `zstd_compare --list` writes the key of each case on a line of its own. `zstd_compare <key>...` checks each case and
// writes a line of JSON for each, {"ok": <bool>, "detail": <text>}, whose detail says what differs when the case fails.
// A case's data is made from its kind and size alone, so a case runs the same alone as among the others.
//
//   decode/<kind>.<size>.<level>   lib/zstd decodes what the reference encoder writes at the level, with a checksum at
//                                  even levels and a content size at levels not divisible by 3: whole, as the
//                                  reference decodes it, and, up to 70000 bytes and at level 3, a piece at a time
//   window/<kind>.<log>            lib/zstd decodes a frame of 1.5 MB at level 5 in a window of 2^log bytes
//   distant/<kind>                 lib/zstd decodes a frame of 1.5 MB at level 19 with long-distance matching
//   stream/all                     lib/zstd decodes the frames of every kind one after another, whole, a piece at a
//                                  time, and into an output a byte short, which it fills
//   encode/<kind>.<size>.<level>   the reference decoder decodes what lib/zstd encodes at the level, whole and as a
//                                  stream

#include "pr/array.h"
#include "pr/mem.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/zstd.h"

#include "tools/harness/harness.h"

#include "zstd.h"

using namespace harness;
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

// A sequence of pseudo-random numbers, the same on every run from the same seed.
struct Sequence {
    uint32_t state;

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

const char* const kKindNames[] = {"words", "smooth", "random", "constant", "mixed"};

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

// The data of `kind` of `size` bytes, made from the two alone.
Array<uint8_t> makeData(Kind kind, uint32_t size) {
    Sequence random{0x2545f491u ^ (uint32_t(kind) * 0x9e3779b9u) ^ size};
    Array<uint8_t> data;
    if (kind != Kind_Mixed) {
        appendKind(data, kind, size, &random);
        return data;
    }
    for (Kind part = Kind_Words; part < Kind_Mixed; ++part)
        appendKind(data, part, size / 5, &random);
    // The start again, which only a long window reaches.
    uint32_t rest = size - data.count();
    for (uint32_t i = 0; i < rest; ++i)
        data.push(i < data.count() ? data[i] : uint8_t(0));
    return data;
}

// Decodes `frames` with lib/zstd at once into room for `capacity` bytes.
Outcome decodeWhole(const Array<uint8_t>& frames, uint32_t capacity, Array<uint8_t>* out) {
    out->resize(capacity);
    PrZstdDecoder* decoder = prZstdDecoderCreate(kPrZstdDefaultWindowLimit);
    PrZstdRead read{};
    PrStatus status = prZstdDecoderRead(decoder, PrByteView{frames.data(), frames.count()}, true,
        capacity != 0 ? out->data() : nullptr, capacity, &read);
    prZstdDecoderDestroy(decoder);
    out->resize(uint32_t(read.written));
    return outcomeOf(status, read.progress);
}

// Decodes `frames` with lib/zstd, `piece` bytes of input at a time into room for `room` bytes.
Outcome decodePieces(const Array<uint8_t>& frames, size_t piece, size_t room, Array<uint8_t>* out) {
    PrZstdDecoder* decoder = prZstdDecoderCreate(kPrZstdDefaultWindowLimit);
    Array<uint8_t> buffer(room);
    out->clear();
    Outcome result = Outcome_Input;
    size_t at = 0;
    for (;;) {
        size_t size = frames.count() - at < piece ? frames.count() - at : piece;
        PrZstdRead read{};
        PrStatus status = prZstdDecoderRead(decoder, PrByteView{frames.data() + at, size}, at + size == frames.count(),
            buffer.data(), room, &read);
        result = outcomeOf(status, read.progress);
        at += read.consumed;
        out->append(buffer.data(), uint32_t(read.written));
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
    Array<uint8_t> frame(ZSTD_compressBound(data.count()));
    size_t size = ZSTD_compress2(context, frame.data(), frame.count(), data.data(), data.count());
    ZSTD_freeCCtx(context);
    frame.resize(ZSTD_isError(size) ? 0 : uint32_t(size));
    return frame;
}

// Returns what differs when lib/zstd decodes `frame` to `data`, as the reference decoder does, or an empty string.
String checkFrame(const Array<uint8_t>& frame, const Array<uint8_t>& data, bool pieces) {
    if (frame.isEmpty() && !data.isEmpty())
        return "the reference encoder writes no frame";
    Array<uint8_t> reference(data.count());
    size_t size = ZSTD_decompress(reference.data(), reference.count(), frame.data(), frame.count());
    if (ZSTD_isError(size))
        return "the reference decoder refuses the frame";
    reference.resize(uint32_t(size));
    if (!same(reference, data))
        return "the reference decoder decodes the frame to other data";
    Array<uint8_t> decoded;
    if (decodeWhole(frame, data.count(), &decoded) != Outcome_Done || !same(decoded, reference))
        return "lib/zstd decodes the frame whole to other data";
    if (!pieces)
        return {};
    if (decodePieces(frame, 4093, 61, &decoded) != Outcome_Done || !same(decoded, reference))
        return "lib/zstd decodes the frame in pieces of 4093 bytes into 61 to other data";
    if (decodePieces(frame, 7, 100000, &decoded) != Outcome_Done || !same(decoded, reference))
        return "lib/zstd decodes the frame in pieces of 7 bytes to other data";
    return {};
}

String checkEncoded(Kind kind, uint32_t size, PrZstdLevel level) {
    Array<uint8_t> data = makeData(kind, size);
    Array<uint8_t> frame(prZstdEncodeBound(data.count()));
    frame.resize(uint32_t(prZstdEncode(level, PrByteView{data.data(), data.count()}, frame.data())));
    if (ZSTD_getFrameContentSize(frame.data(), frame.count()) != data.count())
        return "the frame does not give its content size";
    Array<uint8_t> decoded(data.count() + 1);
    size_t got = ZSTD_decompress(decoded.data(), decoded.count(), frame.data(), frame.count());
    if (ZSTD_isError(got))
        return format("the reference decoder refuses the frame: {}", PrStringView(ZSTD_getErrorName(got)));
    decoded.resize(uint32_t(got));
    if (!same(decoded, data))
        return "the reference decoder decodes the frame to other data";
    // And a piece at a time, as a stream.
    ZSTD_DStream* stream = ZSTD_createDStream();
    Array<uint8_t> pieces;
    uint8_t room[4096];
    ZSTD_inBuffer in{frame.data(), 0, 0};
    size_t left = 1;
    String detail;
    while (in.size < frame.count() || left != 0) {
        in.size = in.size + 777 < frame.count() ? in.size + 777 : frame.count();
        ZSTD_outBuffer out{room, sizeof(room), 0};
        left = ZSTD_decompressStream(stream, &out, &in);
        if (ZSTD_isError(left)) {
            detail = "the reference stream refuses the frame";
            break;
        }
        pieces.append(room, uint32_t(out.pos));
        if (in.pos == frame.count() && left == 0)
            break;
    }
    ZSTD_freeDStream(stream);
    if (detail.isEmpty() && !same(pieces, data))
        detail = "the reference stream decodes the frame to other data";
    return detail;
}

String checkStream() {
    Array<uint8_t> all;
    Array<uint8_t> stream;
    for (Kind kind = 0; kind < Kind_Count; ++kind) {
        Array<uint8_t> data = makeData(kind, 1500000);
        Options options{.checksum = true, .contentSize = false, .windowLog = 14};
        Array<uint8_t> frame = encode(data, 5, options);
        stream.append(frame.data(), frame.count());
        all.append(data.data(), data.count());
    }
    Array<uint8_t> decoded;
    if (decodeWhole(stream, all.count(), &decoded) != Outcome_Done || !same(decoded, all))
        return "lib/zstd decodes the frames whole to other data";
    if (decodeWhole(stream, all.count() - 1, &decoded) != Outcome_Output)
        return "lib/zstd does not fill an output a byte short of the frames";
    if (decodePieces(stream, 65536, 65536, &decoded) != Outcome_Done || !same(decoded, all))
        return "lib/zstd decodes the frames in pieces to other data";
    return {};
}

constexpr uint32_t kSizes[] = {0, 1, 10, 1000, 70000, 300000};
constexpr uint32_t kEncodedSizes[] = {0, 1, 10, 1000, 70000, 300000, 1500000};
constexpr int kWindowLogs[] = {10, 14, 17, 21};
constexpr PrZstdLevel kLevels[] = {PrZstdLevel_Fast, PrZstdLevel_Default, PrZstdLevel_Small};
const char* const kLevelNames[] = {"fast", "default", "small"};

// A case: its key, and what checks it.
struct Case {
    String key;
    String (*run)(Kind kind, uint32_t size, int value);
    Kind kind;
    uint32_t size;
    int value;
};

Array<Case> cases() {
    Array<Case> all;
    for (Kind kind = 0; kind < Kind_Count; ++kind) {
        for (uint32_t size : kSizes) {
            for (int level = -5; level <= 22; ++level) {
                if (level == 0)
                    continue;
                all.push(Case{
                    format("decode/{}.{}.{}", kKindNames[kind], size, level),
                    [](Kind k, uint32_t s, int l) {
                        Options options{.checksum = l % 2 == 0, .contentSize = l % 3 != 0};
                        Array<uint8_t> data = makeData(k, s);
                        return checkFrame(encode(data, l, options), data, s <= 70000 || l == 3);
                    },
                    kind,
                    size,
                    level,
                });
            }
        }
        for (int windowLog : kWindowLogs) {
            all.push(Case{
                format("window/{}.{}", kKindNames[kind], windowLog),
                [](Kind k, uint32_t, int log) {
                    Array<uint8_t> data = makeData(k, 1500000);
                    Options options{.checksum = true, .contentSize = false, .windowLog = log};
                    return checkFrame(encode(data, 5, options), data, log == 10);
                },
                kind,
                0,
                windowLog,
            });
        }
        all.push(Case{
            format("distant/{}", kKindNames[kind]),
            [](Kind k, uint32_t, int) {
                Array<uint8_t> data = makeData(k, 1500000);
                Options distant{.checksum = true, .contentSize = true, .windowLog = 24, .longDistance = true};
                return checkFrame(encode(data, 19, distant), data, false);
            },
            kind,
            0,
            0,
        });
        for (uint32_t size : kEncodedSizes) {
            for (uint32_t i = 0; i < 3; ++i) {
                all.push(Case{
                    format("encode/{}.{}.{}", kKindNames[kind], size, kLevelNames[i]),
                    [](Kind k, uint32_t s, int l) { return checkEncoded(k, s, kLevels[l]); },
                    kind,
                    size,
                    int(i),
                });
            }
        }
    }
    all.push(Case{"stream/all", [](Kind, uint32_t, int) { return checkStream(); }, 0, 0, 0});
    return all;
}

} // namespace

int main(int argc, char** argv) {
    Array<Case> all = cases();
    if (argc == 2 && PrStringView(argv[1]) == "--list") {
        for (const Case& c : all)
            harnessWriteLine(c.key);
        return 0;
    }
    for (int i = 1; i < argc; ++i) {
        String detail = "no case has this key";
        for (const Case& c : all) {
            if (c.key == PrStringView(argv[i])) {
                detail = c.run(c.kind, c.size, c.value);
                break;
            }
        }
        PrTextBuffer line{};
        prTextBufferAppend(&line, detail.isEmpty() ? "{\"ok\": true, \"detail\": " : "{\"ok\": false, \"detail\": ");
        harnessWriteString(&line, detail);
        prTextBufferAppend(&line, "}");
        harnessWriteLine(prTextBufferView(&line));
        prTextBufferReset(&line);
    }
    return 0;
}
