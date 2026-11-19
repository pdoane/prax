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
// `exr_compare --bench <path>...` writes a row of the benchmark for each file and engine: {"engine": "prax" or
// "openexr", "name": the file's name and its first part's compression, "time": milliseconds, "counts": ...}, the cost
// of decoding every level of every part, measured as the harness measures a benchmark (harnessMeasure).
//
// `exr_compare --repeat ENGINE <path>` decodes the file with one decoder, prax or openexr, over and over for
// kHarnessRepeatSeconds, for ./dev.sh profile to sample.

#include "pr/array.h"
#include "pr/count_of.h"
#include "pr/file.h"
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

int encode(const char* compression, const char* layout, const char* input, const char* output) {
    PrTextBuffer bytes = {};
    if (!harnessReadFile(&bytes, input))
        return 1;
    const PrByteView file = {reinterpret_cast<const uint8_t*>(bytes.data), bytes.size};
    exr::Image image;
    int result = 1;
    exr::Layout chosen = exr::Layout_ScanLines;
    PrStringView name(layout);
    if (name == "tiles")
        chosen = exr::Layout_Tiles;
    else if (name == "mip")
        chosen = exr::Layout_MipTiles;
    else if (name == "rip")
        chosen = exr::Layout_RipTiles;
    uint32_t value = 0;
    for (uint32_t i = 0; i < countOf(kCompressions); ++i) {
        if (PrStringView(compression) == kCompressions[i])
            value = i;
    }
    if (exr::referenceImage(file, &image)) {
        Array<uint8_t> written = exr::encode(image, value, chosen);
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
    if (argc >= 2 && PrStringView(argv[1]) == "--bench") {
        for (int i = 2; i < argc; ++i)
            bench(argv[i]);
        return 0;
    }
    for (int i = 1; i < argc; ++i)
        run(argv[i]);
    return 0;
}
