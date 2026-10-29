// image_bench: measures the cost of decoding each image with the built-in decoders.
//
// ./dev.sh bench image-decode runs this harness and records its rows. A row is an image file: its size, its pixel
// count, the time to decode it into RGBA8, and for a PNG, the time to inflate its pixel data alone. Each time is
// measured by harnessMeasure, as the engine's other benchmarks measure theirs.
//
// `image_bench FILE...` writes one JSON row per file that decodes.

#include "pr/array.h"
#include "pr/assert.h"
#include "pr/compress.h"
#include "pr/compress_builtin.h"
#include "pr/image.h"
#include "pr/image_builtin.h"
#include "pr/text_buffer.h"

#include "tools/harness/harness.h"

#include <stdio.h>
#include <string.h>

using namespace pr;
using namespace harness;

namespace {

uint32_t readBe32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// Replaces the contents of `out` with a PNG's zlib stream: the data of its IDAT chunks, joined. Leaves `out` empty for
// any other file.
void pngStream(PrByteView bytes, Array<uint8_t>& out) {
    out.clear();
    static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (bytes.size < 8 || memcmp(bytes.data, kSignature, 8) != 0)
        return;
    size_t at = 8;
    while (at + 12 <= bytes.size) {
        uint32_t length = readBe32(bytes.data + at);
        if (length > bytes.size - at - 12)
            return;
        if (memcmp(bytes.data + at + 4, "IDAT", 4) == 0)
            out.append(bytes.data + at + 8, length);
        at += size_t(length) + 12;
    }
}

void appendLiteral(TextBuffer* out, const char* text) {
    textAppend(out, text, uint32_t(strlen(text)));
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    PrCompressCodec* compressCodec = prCompressBuiltinCreate();
    PrImageCodec* imageCodec = prImageBuiltinCreate(compressCodec);
    PrImageDecoder* decoder = prImageDecoderCreate(imageCodec);
    TextBuffer file{};
    Array<uint8_t> pixels;
    Array<uint8_t> stream;
    Array<uint8_t> inflated;
    int status = 0;
    for (int i = 1; i < argc; ++i) {
        if (!harnessReadFile(&file, argv[i])) {
            fprintf(stderr, "cannot read %s\n", argv[i]);
            status = 1;
            continue;
        }
        PrByteView bytes{reinterpret_cast<const uint8_t*>(file.text.data), file.text.count};
        PrImageSource source{};
        source.memory = bytes;
        if (prImageDecoderOpen(decoder, source, nullptr) != PrStatus_Ok) {
            fprintf(stderr, "cannot open %s\n", argv[i]);
            status = 1;
            continue;
        }
        const PrImageInfo* info = prImageDecoderInfo(decoder);
        size_t size = size_t(info->extent.x) * info->extent.y * 4;
        pixels.clear();
        pixels.reserve(uint32_t(size));
        auto decode = [&] {
            return prImageDecoderReadRgba8(decoder, 0, PrImageAlpha_Straight, PrImageOrientation_Identity,
                                           PrPixels{pixels.data, size_t(info->extent.x) * 4, info->extent});
        };
        if (decode() != PrStatus_Ok) {
            fprintf(stderr, "cannot decode %s\n", argv[i]);
            prImageDecoderClose(decoder);
            status = 1;
            continue;
        }
        HarnessCounts counts{};
        HarnessCounters decoded = harnessMeasure([&] { decode(); });
        harnessCountsAdd(&counts, "decode", decoded);

        // Inflating is timed into a buffer large enough for the whole stream.
        pngStream(bytes, stream);
        double inflateMillis = -1;
        if (stream.count != 0) {
            inflated.clear();
            inflated.reserve(uint32_t(size * 2 + info->extent.y * 2 + 64));
            auto inflate = [&] {
                PrCompressRead read{};
                return prCompressCodecDecode(compressCodec, PrCompressFormat_Zlib,
                                             PrByteView{stream.data, stream.count}, inflated.data, inflated.capacity,
                                             &read);
            };
            if (inflate() == PrStatus_Ok) {
                HarnessCounters inflating = harnessMeasure([&] { inflate(); });
                harnessCountsAdd(&counts, "inflate", inflating);
                inflateMillis = inflating.millis;
            }
        }

        TextBuffer row{};
        appendLiteral(&row, "{\"name\":");
        harnessWriteString(&row, harnessFileName(argv[i]));
        appendLiteral(&row, ",\"bytes\":");
        harnessWriteNumber(&row, bytes.size);
        appendLiteral(&row, ",\"pixels\":");
        harnessWriteNumber(&row, uint64_t(info->extent.x) * info->extent.y);
        char number[64];
        snprintf(number, sizeof(number), ",\"decode\":%.6f", decoded.millis);
        appendLiteral(&row, number);
        if (inflateMillis >= 0) {
            snprintf(number, sizeof(number), ",\"inflate\":%.6f", inflateMillis);
            appendLiteral(&row, number);
        }
        harnessCountsWrite(&counts, &row);
        appendLiteral(&row, "}");
        harnessWriteLine(textView(&row));
        row.text.reset();
        prImageDecoderClose(decoder);
    }
    file.text.reset();
    pixels.reset();
    stream.reset();
    inflated.reset();
    prImageDecoderDestroy(decoder);
    prImageCodecDestroy(imageCodec);
    prCompressCodecDestroy(compressCodec);
    return status;
}
