// The decode_compare harness, which the suites of image corpora drive (tools/suite/decode_compare.py): lib/image's
// decoders over files, each beside the PNGs that hold its picture.
//
// `decode_compare <case>...` reads each case, `path` or `path=reference,reference...`, decodes the file at `path`
// through the set of every codec as straight-alpha Rgba8 in its displayed orientation, decodes each reference the same
// way, and writes a line of JSON for each case:
//
//   {"status": the name of the decode's status, or "" for Ok, "width": and "height": of the displayed image,
//    "references": [the largest difference of one byte from each reference, or -1 for one that does not decode or
//    has another extent]}

#include "pr/array.h"
#include "pr/string_ops.h"
#include "pr/string_view.h"
#include "pr/text_buffer.h"

#include "tools/harness/harness.h"
#include "tools/harness/image_test.h"

using namespace harness;
using namespace pr;

namespace {

// A decoded image: the status of its decode, and its displayed Rgba8 pixels.
struct Decoded {
    PrStatus status = PrStatus_Ok;
    uint32_t width = 0;
    uint32_t height = 0;
    Array<uint8_t> rgba;
};

Decoded decode(const char* path) {
    Decoded decoded;
    PrTextBuffer bytes = {};
    if (!harnessReadFile(&bytes, path)) {
        decoded.status = PrStatus_Io;
        return decoded;
    }
    PrImageSource source{};
    source.memory = PrByteView{reinterpret_cast<const uint8_t*>(bytes.data), bytes.size};
    PrImageDecoder* decoder = nullptr;
    decoded.status = testDecoderOpen(testCodec(), source, nullptr, &decoder);
    if (decoder != nullptr) {
        const PrImageInfo& info = *prImageDecoderInfo(decoder);
        const bool swap = prImageOrientationSwapsAxes(info.orientation);
        decoded.width = swap ? info.extent.y : info.extent.x;
        decoded.height = swap ? info.extent.x : info.extent.y;
        decoded.rgba.resize(decoded.width * decoded.height * 4);
        const PrStatus read = testReadRgba8(decoder, 0, true, PrImageAlpha_Straight, decoded.rgba.data(),
            decoded.rgba.count());
        if (decoded.status == PrStatus_Ok)
            decoded.status = read;
        prImageDecoderDestroy(decoder);
    }
    prTextBufferReset(&bytes);
    return decoded;
}

// Returns the largest difference of one byte between `a` and `b`, or -1 if `b` did not decode or has another extent.
// The color of a pixel that both make transparent does not count.
int32_t largest(const Decoded& a, const Decoded& b) {
    if ((b.status != PrStatus_Ok && b.status != PrStatus_Truncated) || a.width != b.width || a.height != b.height ||
            a.rgba.count() != b.rgba.count())
        return -1;
    int32_t most = 0;
    for (uint32_t i = 0; i < a.rgba.count(); i += 4) {
        const bool clear = a.rgba[i + 3] == 0 && b.rgba[i + 3] == 0;
        for (uint32_t c = clear ? 3 : 0; c < 4; ++c) {
            const int32_t delta = int32_t(a.rgba[i + c]) - int32_t(b.rgba[i + c]);
            most = delta > most ? delta : -delta > most ? -delta : most;
        }
    }
    return most;
}

void run(PrStringView spec) {
    const size_t equals = prStringFindByte(spec, '=');
    const Array<char> path = [&] {
        Array<char> out;
        out.append(spec.data, uint32_t(equals));
        out.push(0);
        return out;
    }();
    const Decoded decoded = decode(path.data());
    PrTextBuffer line = {};
    prTextBufferAppend(&line, "{\"status\": ");
    harnessWriteString(&line, decoded.status == PrStatus_Ok ? PrStringView("") : prStatusName(decoded.status));
    harnessAppendFormat(&line, ", \"width\": %u, \"height\": %u, \"references\": [", decoded.width, decoded.height);
    PrStringView rest =
        equals < spec.size ? PrStringView{spec.data + equals + 1, spec.size - equals - 1} : PrStringView{};
    bool first = true;
    while (rest.size != 0) {
        const size_t comma = prStringFindByte(rest, ',');
        Array<char> reference;
        reference.append(rest.data, uint32_t(comma));
        reference.push(0);
        const bool decodes = decoded.status == PrStatus_Ok || decoded.status == PrStatus_Truncated;
        const int32_t difference = decodes ? largest(decoded, decode(reference.data())) : -1;
        harnessAppendFormat(&line, "%s%d", first ? "" : ", ", difference);
        first = false;
        rest = comma < rest.size ? PrStringView{rest.data + comma + 1, rest.size - comma - 1} : PrStringView{};
    }
    prTextBufferAppend(&line, "]}");
    harnessWriteLine(prTextBufferView(&line));
    prTextBufferReset(&line);
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        run(PrStringView(argv[i]));
    return 0;
}
