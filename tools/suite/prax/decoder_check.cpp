// The harness of the prax-decoders suite (tools/suite/prax/decoders.py): a decoder given its input damaged.
//
// `decoder_check <check> <path> [<check> <path>]...` checks the file at each path, and writes a line of JSON for each,
// {"ok": <bool>, "detail": <text>}, whose detail says what went wrong when the check fails. The decoder is the file's
// by its extension: .zst, .br, .lzfse, .webp, or a font.
//
//   flips          each byte flipped by 0x01, 0x80 and 0xff, then decoded: a stream in pieces and whole, an image every
//                  frame. A decoder keeps its contract, and a Zstandard frame with a checksum that still decodes
//                  decodes to what the file does
//   cut            each proper prefix, which does not decode whole
//   table:<TAG>    the font with the table cut to each length, and with its first offsets past its end, read through
//                  every call that reads the table (MATH alone so far)
//
// What the checks cannot see, a read or a write out of bounds, the sanitizer build sees: `./dev.sh suite asan
// prax-decoders`.

#include "pr/array.h"
#include "pr/blob.h"
#include "pr/blob_ref.h"
#include "pr/brotli.h"
#include "pr/file.h"
#include "pr/font.h"
#include "pr/font_math.h"
#include "pr/lzfse.h"
#include "pr/min_max.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/webp.h"
#include "pr/zstd.h"

#include "tools/harness/font_tables.h"
#include "tools/harness/harness.h"

using namespace harness;
using namespace pr;

namespace {

constexpr uint8_t kFlips[] = {0x01, 0x80, 0xff};

// How a decode ended: Done, Truncated, Failed, or Broken if the decoder broke its contract, by reading or writing past
// what it was given or by making no progress.
enum {
    Ended_Done,
    Ended_Truncated,
    Ended_Failed,
    Ended_Broken,
};
typedef uint8_t Ended;

struct Decoded {
    Ended ended = Ended_Failed;
    String text;
};

// The streaming decoders, which share one shape: a decoder made, read with input and room for output, and destroyed.
struct Zstd {
    using Decoder = PrZstdDecoder;
    static PrZstdDecoder* create() { return prZstdDecoderCreate(kPrZstdDefaultWindowLimit); }
    static void destroy(PrZstdDecoder* d) { prZstdDecoderDestroy(d); }
    static PrStatus read(PrZstdDecoder* d, PrByteView in, bool final, uint8_t* out, size_t capacity, size_t* consumed,
            size_t* written, uint8_t* progress) {
        PrZstdRead r{};
        PrStatus status = prZstdDecoderRead(d, in, final, out, capacity, &r);
        *consumed = r.consumed;
        *written = r.written;
        *progress = r.progress;
        return status;
    }
    static constexpr uint8_t kInput = PrZstdProgress_Input;
    static constexpr uint8_t kOutput = PrZstdProgress_Output;
};

struct Lzfse {
    using Decoder = PrLzfseDecoder;
    static PrLzfseDecoder* create() { return prLzfseDecoderCreate(); }
    static void destroy(PrLzfseDecoder* d) { prLzfseDecoderDestroy(d); }
    static PrStatus read(PrLzfseDecoder* d, PrByteView in, bool final, uint8_t* out, size_t capacity, size_t* consumed,
            size_t* written, uint8_t* progress) {
        PrLzfseRead r{};
        PrStatus status = prLzfseDecoderRead(d, in, final, out, capacity, &r);
        *consumed = r.consumed;
        *written = r.written;
        *progress = r.progress;
        return status;
    }
    static constexpr uint8_t kInput = PrLzfseProgress_Input;
    static constexpr uint8_t kOutput = PrLzfseProgress_Output;
};

struct Brotli {
    using Decoder = PrBrotliDecoder;
    static PrBrotliDecoder* create() { return prBrotliDecoderCreate(); }
    static void destroy(PrBrotliDecoder* d) { prBrotliDecoderDestroy(d); }
    static PrStatus read(PrBrotliDecoder* d, PrByteView in, bool final, uint8_t* out, size_t capacity, size_t* consumed,
            size_t* written, uint8_t* progress) {
        PrBrotliRead r{};
        PrStatus status = prBrotliDecoderRead(d, in, final, out, capacity, &r);
        *consumed = r.consumed;
        *written = r.written;
        *progress = r.progress;
        return status;
    }
    static constexpr uint8_t kInput = PrBrotliProgress_Input;
    static constexpr uint8_t kOutput = PrBrotliProgress_Output;
};

// Decodes `input` in pieces of `inputChunk` bytes into an output of `outputChunk` bytes.
template <typename Codec>
Decoded decode(PrByteView input, size_t inputChunk, size_t outputChunk) {
    typename Codec::Decoder* decoder = Codec::create();
    Decoded decoded;
    Array<uint8_t> output(outputChunk);
    size_t at = 0;
    for (;;) {
        const size_t size = min(inputChunk, input.size - at);
        size_t consumed = 0;
        size_t written = 0;
        uint8_t progress = 0;
        PrStatus status = Codec::read(decoder, PrByteView{input.data + at, size}, at + size == input.size,
            output.data(), output.count(), &consumed, &written, &progress);
        if (consumed > size || written > output.count()) {
            decoded.ended = Ended_Broken;
            break;
        }
        decoded.text.append(PrStringView{reinterpret_cast<const char*>(output.data()), written});
        at += consumed;
        if (status != PrStatus_Ok) {
            decoded.ended = status == PrStatus_Truncated ? Ended_Truncated : Ended_Failed;
            break;
        }
        if (progress != Codec::kInput && progress != Codec::kOutput) {
            decoded.ended = Ended_Done;
            break;
        }
        if (consumed == 0 && written == 0 && progress != Codec::kInput) {
            decoded.ended = Ended_Broken;
            break;
        }
    }
    Codec::destroy(decoder);
    return decoded;
}

// A copy of exactly `bytes`, so that a read past them is a read past the allocation.
Array<uint8_t> copyOf(PrByteView bytes) {
    Array<uint8_t> out;
    out.append(bytes.data, uint32_t(bytes.size));
    return out;
}

template <typename Codec>
String streamFlips(PrByteView file, bool checksummed) {
    Decoded expected = decode<Codec>(file, file.size, 1u << 20);
    if (expected.ended != Ended_Done)
        return "the file does not decode";
    Array<uint8_t> bytes = copyOf(file);
    for (uint32_t at = 0; at < bytes.count(); ++at) {
        for (uint8_t flip : kFlips) {
            bytes[at] ^= flip;
            PrByteView changed{bytes.data(), bytes.count()};
            const Decoded tries[] = {decode<Codec>(changed, 37, 1000), decode<Codec>(changed, changed.size, 1u << 20)};
            for (const Decoded& got : tries) {
                if (got.ended == Ended_Broken)
                    return format("byte {} flipped by {:#04x}: the decoder broke its contract", at, flip);
                if (checksummed && got.ended == Ended_Done && got.text != expected.text)
                    return format("byte {} flipped by {:#04x}: decodes to other content past its checksum", at, flip);
            }
            bytes[at] ^= flip;
        }
    }
    return {};
}

// Returns true if the first frame of a Zstandard file ends with a checksum of its content: the frame header's
// descriptor, after the magic number, sets the Content_Checksum_flag.
bool zstdChecksummed(PrByteView file) {
    return file.size > 4 && (file.data[4] & 0x04) != 0;
}

template <typename Codec>
String streamCut(PrByteView file) {
    for (size_t size = 0; size < file.size; ++size) {
        Array<uint8_t> prefix = copyOf(PrByteView{file.data, size});
        PrByteView cut{prefix.data(), prefix.count()};
        const Decoded tries[] = {decode<Codec>(cut, size + 1, 4096), decode<Codec>(cut, 3, 100)};
        for (const Decoded& got : tries) {
            if (got.ended == Ended_Broken)
                return format("the prefix of {} bytes: the decoder broke its contract", size);
            if (got.ended == Ended_Done)
                return format("the prefix of {} bytes decodes whole", size);
        }
    }
    return {};
}

// Opens `bytes` and reads every frame. A changed byte can claim a far larger image than the file's own: past a million
// pixels, the image is refused.
PrStatus webpRead(PrByteView bytes) {
    PrWebpLimits limits = prWebpLimitsDefault();
    limits.pixels = 1u << 20;
    PrWebpDecoder* decoder = nullptr;
    const PrStatus opened = prWebpDecoderCreate(bytes, &limits, bytes.size, &decoder);
    if (decoder == nullptr)
        return opened;
    const PrWebpInfo& info = *prWebpDecoderInfo(decoder);
    const uint32_t size = info.alpha ? 4 : 3;
    Array<uint8_t> native(size_t(info.extent.x) * info.extent.y * size);
    PrStatus status = PrStatus_Ok;
    for (uint32_t frame = 0; frame < info.frameCount && status == PrStatus_Ok; ++frame)
        status = prWebpDecoderRead(decoder, frame, PrPixels{native.data(), info.extent.x * size, info.extent});
    prWebpDecoderDestroy(decoder);
    return status == PrStatus_Ok ? opened : status;
}

String webpFlips(PrByteView file) {
    if (webpRead(file) != PrStatus_Ok)
        return "the file does not decode";
    for (uint32_t at = 0; at < file.size; ++at) {
        for (uint8_t flip : kFlips) {
            Array<uint8_t> bytes = copyOf(file);
            bytes[at] ^= flip;
            webpRead(PrByteView{bytes.data(), bytes.count()});
        }
    }
    return {};
}

String webpCut(PrByteView file) {
    for (size_t size = 0; size < file.size; ++size) {
        Array<uint8_t> bytes = copyOf(PrByteView{file.data, size});
        if (webpRead(PrByteView{bytes.data(), bytes.count()}) == PrStatus_Ok)
            return format("the prefix of {} bytes reads whole", size);
    }
    return {};
}

constexpr uint32_t kMath = PR_FONT_TAG('M', 'A', 'T', 'H');

// The step between the lengths that a table is cut to, past its first bytes, which are cut at every length.
constexpr uint32_t kTableStep = 97;
constexpr uint32_t kTableEveryLength = 256;

// Reads every MATH value at glyphs inside and past the font. Returns what is wrong, or an empty string.
String readMath(const PrFont* font) {
    if (!(prFontMathConstants(font)->scriptScriptPercentScaleDown > 0))
        return "a constant does not fall back";
    const uint16_t glyphs[] = {9, 71, 1006, 2981, 0xFFFF};
    const PrFontMathAxis axes[] = {PrFontMathAxis_Horizontal, PrFontMathAxis_Vertical};
    for (uint16_t glyph : glyphs) {
        prFontMathGlyphItalicsCorrection(font, glyph);
        prFontMathGlyphTopAccentAttachment(font, glyph);
        prFontMathGlyphIsExtendedShape(font, glyph);
        prFontMathGlyphKern(font, glyph, PrFontMathCorner_BottomRight, 0.5f);
        prFontMathGlyphPreferredAdvance(font, glyph);
        for (PrFontMathAxis axis : axes) {
            uint32_t parts = prFontMathGlyphPartCount(font, glyph, axis);
            for (uint32_t i = 0; i < parts; ++i)
                prFontMathGlyphPart(font, glyph, axis, i);
            PrFontMathStretchInfo info{};
            PrFontGlyph out[64];
            if (prFontMathGlyphStretch(font, glyph, axis, 20, &info, out, 64) >= 0x10000)
                return format("glyph {} stretches to too many glyphs", glyph);
        }
    }
    return {};
}

String tableCut(PrByteView file, PrStringView tag) {
    if (tag != "MATH")
        return format("no reads of the table {} are known", tag);
    Array<uint8_t> font = copyOf(file);
    Array<uint8_t> table = tableOf(font, kMath);
    if (table.isEmpty())
        return "the font has no MATH table";
    for (uint32_t size = 0; size < table.count(); size += size < kTableEveryLength ? 1 : kTableStep) {
        Array<uint8_t> cut;
        cut.append(table.data(), size);
        Array<uint8_t> past = cut;
        for (uint32_t at = 4; at + 2 <= past.count() && at < 10; at += 2)
            set16(past, at, 0xFFF0);
        const Array<uint8_t>* const tables[] = {&cut, &past};
        for (const Array<uint8_t>* each : tables) {
            const PrFont* made = nullptr;
            if (prFontCreate(BlobRef::adopt(prBlobCopy(withTable(font, kMath, *each))), 0, nullptr, &made) !=
                    PrStatus_Ok)
                return format("cut to {} bytes, the font does not open", size);
            String wrong = readMath(made);
            prFontRelease(made);
            if (!wrong.isEmpty())
                return format("cut to {} bytes{}: {}", size, each == &past ? ", its offsets past the end" : "", wrong);
        }
    }
    return {};
}

String check(PrStringView kind, PrStringView path) {
    BlobRef blob = BlobRef::adopt(prFileOpen(path));
    if (blob.get() == nullptr)
        return "the file cannot be read";
    PrByteView file = prBlobBytes(blob.get());
    if (prStringStartsWith(kind, "table:"))
        return tableCut(file, prStringFrom(kind, 6));
    bool flips = kind == "flips";
    if (!flips && kind != "cut")
        return format("no check is named {}", kind);
    if (prStringEndsWith(path, ".zst"))
        return flips ? streamFlips<Zstd>(file, zstdChecksummed(file)) : streamCut<Zstd>(file);
    if (prStringEndsWith(path, ".br"))
        return flips ? streamFlips<Brotli>(file, false) : streamCut<Brotli>(file);
    if (prStringEndsWith(path, ".lzfse"))
        return flips ? streamFlips<Lzfse>(file, false) : streamCut<Lzfse>(file);
    if (prStringEndsWith(path, ".webp"))
        return flips ? webpFlips(file) : webpCut(file);
    return "no decoder reads files of this kind";
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; i += 2) {
        String detail = check(argv[i], argv[i + 1]);
        PrTextBuffer line{};
        prTextBufferAppend(&line, detail.isEmpty() ? "{\"ok\": true, \"detail\": " : "{\"ok\": false, \"detail\": ");
        harnessWriteString(&line, detail);
        prTextBufferAppend(&line, "}");
        harnessWriteLine(prTextBufferView(&line));
        prTextBufferReset(&line);
    }
    return 0;
}
