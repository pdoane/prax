// The exr harness, which tools/suite/exr.py and the exr benchmark drive: lib/exr's decoder beside OpenEXR's core
// library, over files.
//
// `exr_compare <path>...` decodes the file at each path with both, as exr_reference.h describes, and writes a line of
// JSON for each:
//
//   {"status": the name of lib/exr's status, or empty for Ok, "reference": whether OpenEXR decodes the file,
//    "parts": the parts, "compared": the samples compared, "differ": those that differ, "outside": those of the data
//    window outside the display window, which lib/exr's first level does not hold, "shapes": whether the decodes
//    have the same parts, levels and planes, "deep": the parts of deep data, which lib/exr does not read,
//    "compressions": the compression of each part}
//
// `exr_compare --encode COMPRESSION LAYOUT <input> <output>` writes the first level of the first part of `input`, as
// OpenEXR decodes it, as a file of one part in COMPRESSION, the value of the compression attribute, and LAYOUT, which
// is scanlines, tiles, mip or rip.
//
// `exr_compare --make CHANNELS WIDTH HEIGHT COMPRESSION LAYOUT <output>` writes an image of one part and one level,
// made here, of rgba (RGBA halves with a float depth and a uint id), chroma (luminance with chroma subsampled 2 by 2)
// or layers (layers of color, of a float alpha, and of perceptually linear channels), with smooth values and some
// noise.
//
// `exr_compare --bench <path>...` writes a row of the benchmark for each file and engine: {"engine": "prax" or
// "openexr", "name": the file's name and its first part's compression, "time": milliseconds, "counts": ...}, the cost
// of decoding every level of every part, measured as the harness measures a benchmark (harnessMeasure).
//
// `exr_compare --repeat ENGINE <path>` decodes the file with one decoder, prax or openexr, over and over for
// kHarnessRepeatSeconds, for ./dev.sh profile to sample.

#include "pr/array.h"
#include "pr/count_of.h"
#include "pr/file.h"
#include "pr/float16.h"
#include "pr/integer.h"
#include "pr/mem.h"
#include "pr/string_view.h"
#include "pr/text_buffer.h"

#include "tools/harness/harness.h"
#include "tools/suite/exr/exr_reference.h"

using namespace harness;
using namespace pr;

namespace {

// The names of the compressions, by the value of the compression attribute.
constexpr const char* kCompressions[] = {
    "none", "rle", "zips", "zip", "piz", "pxr24", "b44", "b44a", "dwaa", "dwab", "htj2k256", "htj2k32",
};

const char* compressionName(uint32_t compression) {
    return compression < countOf(kCompressions) ? kCompressions[compression] : "unknown";
}

void run(const char* path) {
    PrTextBuffer bytes = {};
    PrTextBuffer line = {};
    if (!harnessReadFile(&bytes, path)) {
        prTextBufferAppend(&line, "{\"status\": \"Io\", \"reference\": false}");
        harnessWriteLine(prTextBufferView(&line));
        prTextBufferReset(&line);
        return;
    }
    const PrByteView file = {reinterpret_cast<const uint8_t*>(bytes.data), bytes.size};
    exr::Image reference;
    exr::Image prax;
    const bool decoded = exr::referenceImage(file, &reference);
    const PrStatus status = exr::praxImage(file, &prax);
    exr::Comparison comparison = exr::compare(prax, reference);
    uint32_t deep = 0;
    for (uint32_t i = 0; i < reference.parts.count(); ++i)
        deep += reference.parts[i].deep ? 1 : 0;
    prTextBufferAppend(&line, "{\"status\": ");
    harnessWriteString(&line, status == PrStatus_Ok ? PrStringView("") : prStatusName(status));
    harnessAppendFormat(&line, ", \"reference\": %s, \"parts\": %u, \"compared\": %llu, \"differ\": %llu, "
        "\"outside\": %llu, \"shapes\": %s, \"deep\": %u, \"compressions\": [", decoded ? "true" : "false",
        reference.parts.count(),
        static_cast<unsigned long long>(comparison.compared), static_cast<unsigned long long>(comparison.differ),
        static_cast<unsigned long long>(comparison.outside), comparison.shapes ? "true" : "false", deep);
    for (uint32_t i = 0; i < reference.parts.count(); ++i)
        harnessAppendFormat(&line, "%s\"%s\"", i == 0 ? "" : ", ", compressionName(reference.parts[i].compression));
    prTextBufferAppend(&line, "]}");
    harnessWriteLine(prTextBufferView(&line));
    prTextBufferReset(&line);
    prTextBufferReset(&bytes);
}

// A sequence of pseudo-random numbers, the same on every run.
struct Sequence {
    uint32_t state = 0x2545F491u;

    uint32_t next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
};

// Returns a smooth value at pixel (x, y) of channel `channel`, with some noise.
float value(uint32_t channel, uint32_t x, uint32_t y, Sequence& noise) {
    float base = float((x * (channel + 3) + y * 7) % 97) / 40.0f + float(channel) * 0.25f;
    return base + float(noise.next() % 64) / 4096.0f;
}

// Adds a plane of `pixel` named `name` to `level`, of the level's extent sampled at `sampling`, with smooth values. A
// perceptually linear plane says so in its pLinear.
void addPlane(exr::Level* level, const char* name, uint32_t pixel, uint32_t sampling, Sequence& noise, bool linear = false) {
    exr::Plane plane;
    plane.name = name;
    plane.pixel = pixel;
    plane.linear = linear;
    plane.xSampling = sampling;
    plane.ySampling = sampling;
    plane.sampleBytes = pixel == 1 ? 2 : 4;
    plane.width = level->width / sampling;
    plane.height = level->height / sampling;
    plane.samples.resize(plane.width * plane.height * plane.sampleBytes);
    uint32_t channel = level->planes.count();
    for (uint32_t y = 0; y < plane.height; ++y) {
        for (uint32_t x = 0; x < plane.width; ++x) {
            uint8_t* at = plane.samples.data() + (size_t(y) * plane.width + x) * plane.sampleBytes;
            float f = value(channel, x, y, noise);
            if (pixel == 0) {
                uint32_t u = uint32_t(f * 1000);
                prMemCopy(at, &u, 4);
            } else if (pixel == 1) {
                PrFloat16 h = prFloat16FromFloat(f);
                prMemCopy(at, &h, 2);
            } else {
                prMemCopy(at, &f, 4);
            }
        }
    }
    level->planes.push(static_cast<exr::Plane&&>(plane));
}

// The channels of a test image.
enum {
    Channels_Rgba,          // RGBA halves with a float depth and a uint id
    Channels_Chroma,        // luminance with chroma subsampled 2 by 2
    Channels_Layers,        // layers of color, of a float alpha, and of perceptually linear channels
};
typedef uint8_t Channels;

// An image of one part and one level of `width` by `height` pixels, of `channels`.
exr::Image makeImage(uint32_t width, uint32_t height, Channels channels) {
    Sequence noise;
    exr::Level level;
    level.width = width;
    level.height = height;
    if (channels == Channels_Chroma) {
        addPlane(&level, "BY", 1, 2, noise);
        addPlane(&level, "RY", 1, 2, noise);
        addPlane(&level, "Y", 1, 1, noise);
    } else if (channels == Channels_Layers) {
        addPlane(&level, "diffuse.B", 1, 1, noise);
        addPlane(&level, "diffuse.G", 1, 1, noise);
        addPlane(&level, "diffuse.R", 1, 1, noise);
        addPlane(&level, "mask.A", 2, 1, noise);
        addPlane(&level, "spec.R", 2, 1, noise, true);
        addPlane(&level, "spec.Y", 1, 1, noise, true);
    } else {
        addPlane(&level, "A", 1, 1, noise);
        addPlane(&level, "B", 1, 1, noise);
        addPlane(&level, "G", 1, 1, noise);
        addPlane(&level, "R", 1, 1, noise);
        addPlane(&level, "Z", 2, 1, noise);
        addPlane(&level, "id", 0, 1, noise);
    }
    exr::Part part;
    part.displayWidth = width;
    part.displayHeight = height;
    part.levels.push(static_cast<exr::Level&&>(level));
    exr::Image image;
    image.parts.push(static_cast<exr::Part&&>(part));
    return image;
}

// Returns the decimal number that `text` starts with, or zero.
uint32_t numberOf(const char* text) {
    uint64_t value = 0;
    prU64ParsePrefix(text, PrStringView(text).size, &value, 10);
    return value > UINT32_MAX ? 0 : uint32_t(value);
}

// Returns the layout that `name` names: scanlines, tiles, mip or rip.
exr::Layout layoutOf(PrStringView name) {
    if (name == "tiles")
        return exr::Layout_Tiles;
    if (name == "mip")
        return exr::Layout_MipTiles;
    if (name == "rip")
        return exr::Layout_RipTiles;
    return exr::Layout_ScanLines;
}

// Returns the value of the compression attribute that `name` names, or zero, no compression.
uint32_t compressionOf(PrStringView name) {
    for (uint32_t i = 0; i < countOf(kCompressions); ++i) {
        if (name == kCompressions[i])
            return i;
    }
    return 0;
}

// Writes the image of `channels`, rgba, chroma or layers, of `width` by `height` pixels, in the compression and the
// layout, to `output`.
int make(const char* channels, uint32_t width, uint32_t height, const char* compression, const char* layout, const char* output) {
    PrStringView kind(channels);
    Channels chosen = kind == "chroma" ? Channels_Chroma : kind == "layers" ? Channels_Layers : Channels_Rgba;
    exr::Image image = makeImage(width, height, chosen);
    Array<uint8_t> written = exr::encode(image, compressionOf(compression), layoutOf(layout));
    if (written.count() == 0 || !prFileWrite(PrStringView(output), PrByteView{written.data(), written.count()}))
        return 1;
    return 0;
}

int encode(const char* compression, const char* layout, const char* input, const char* output) {
    PrTextBuffer bytes = {};
    if (!harnessReadFile(&bytes, input))
        return 1;
    const PrByteView file = {reinterpret_cast<const uint8_t*>(bytes.data), bytes.size};
    exr::Image image;
    int result = 1;
    if (exr::referenceImage(file, &image)) {
        Array<uint8_t> written = exr::encode(image, compressionOf(compression), layoutOf(layout));
        if (written.count() != 0 && prFileWrite(PrStringView(output), PrByteView{written.data(), written.count()}))
            result = 0;
    }
    prTextBufferReset(&bytes);
    return result;
}

void bench(const char* path) {
    PrTextBuffer bytes = {};
    if (!harnessReadFile(&bytes, path))
        return;
    const PrByteView file = {reinterpret_cast<const uint8_t*>(bytes.data), bytes.size};
    exr::Image image;
    if (!exr::referenceImage(file, &image) || image.parts.count() == 0) {
        prTextBufferReset(&bytes);
        return;
    }
    Array<uint8_t> scratch;
    if (exr::praxDecode(file, &scratch) != PrStatus_Ok) {
        prTextBufferReset(&bytes);
        return;
    }
    PrTextBuffer name = {};
    harnessAppendFormat(&name, "%.*s %s", int(harnessFileName(path).size), harnessFileName(path).data,
        compressionName(image.parts[0].compression));
    const char* engines[] = {"prax", "openexr"};
    for (const char* engine : engines) {
        bool ours = PrStringView(engine) == "prax";
        HarnessCounters cost = harnessMeasure([&] {
            if (ours)
                (void)exr::praxDecode(file, &scratch);
            else
                (void)exr::referenceDecode(file, &scratch);
        });
        HarnessCounts counts{};
        harnessCountsAdd(&counts, "time", cost);
        PrTextBuffer row = {};
        prTextBufferAppend(&row, "{\"engine\": ");
        harnessWriteString(&row, PrStringView(engine));
        prTextBufferAppend(&row, ", \"name\": ");
        harnessWriteString(&row, prTextBufferView(&name));
        harnessAppendFormat(&row, ", \"time\": %.6f", cost.millis);
        harnessCountsWrite(&counts, &row);
        prTextBufferAppend(&row, "}");
        harnessWriteLine(prTextBufferView(&row));
        prTextBufferReset(&row);
    }
    prTextBufferReset(&name);
    prTextBufferReset(&bytes);
}

// Decodes the file at `path` with one decoder, "prax" or "openexr", over and over, for a profiler to sample.
int repeat(const char* engine, const char* path) {
    PrTextBuffer bytes = {};
    if (!harnessReadFile(&bytes, path))
        return 1;
    const PrByteView file = {reinterpret_cast<const uint8_t*>(bytes.data), bytes.size};
    const bool ours = PrStringView(engine) == "prax";
    Array<uint8_t> scratch;
    harnessRepeat([&] {
        if (ours)
            (void)exr::praxDecode(file, &scratch);
        else
            (void)exr::referenceDecode(file, &scratch);
    });
    prTextBufferReset(&bytes);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 4 && PrStringView(argv[1]) == "--repeat")
        return repeat(argv[2], argv[3]);
    if (argc == 6 && PrStringView(argv[1]) == "--encode")
        return encode(argv[2], argv[3], argv[4], argv[5]);
    if (argc == 8 && PrStringView(argv[1]) == "--make")
        return make(argv[2], numberOf(argv[3]), numberOf(argv[4]), argv[5], argv[6], argv[7]);
    if (argc >= 2 && PrStringView(argv[1]) == "--bench") {
        for (int i = 2; i < argc; ++i)
            bench(argv[i]);
        return 0;
    }
    for (int i = 1; i < argc; ++i)
        run(argv[i]);
    return 0;
}
