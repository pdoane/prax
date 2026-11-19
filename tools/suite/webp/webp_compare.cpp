// The webp harness, which tools/suite/webp.py drives: lib/webp's decoder beside libwebp's, over files.
//
// `webp_compare [--time] <path>...` decodes the file at each path with both, as webp_reference.h describes, and writes
// a line of JSON for each:
//
//   {"status": the name of lib/webp's status, "reference": whether libwebp decodes the file, "frames": its frames,
//    "bytes": the Rgba8 bytes compared, "differ": the bytes that differ, "largest": the largest difference of one,
//    "planes": "equal", "differ" or "none", for a still lossy bitstream's planes, "time": {name: milliseconds},
//    "instructions": {name: instructions retired}, "cycles": {name: cycles}}
//
// A file that one decoder refuses has no bytes compared. "time" is present with --time: what one decode of every frame
// takes with each decoder, measured as the harness measures a benchmark (harnessMeasure).
//
// `webp_compare --repeat ENGINE <path>` decodes the file with one decoder, prax or libwebp, over and over for
// kHarnessRepeatSeconds, for ./dev.sh profile to sample.

#include "pr/array.h"
#include "pr/string_view.h"
#include "pr/text_buffer.h"

#include "tools/harness/harness.h"
#include "tools/suite/webp/webp_reference.h"

using namespace harness;
using namespace pr;

namespace {

void run(const char* path, bool times) {
    PrTextBuffer bytes = {};
    PrTextBuffer line = {};
    if (!harnessReadFile(&bytes, path)) {
        prTextBufferAppend(&line, "{\"status\": \"Io\", \"reference\": false}");
        harnessWriteLine(prTextBufferView(&line));
        prTextBufferReset(&line);
        return;
    }
    const PrByteView file = {reinterpret_cast<const uint8_t*>(bytes.data), bytes.size};
    webp::Frames reference;
    webp::Frames prax;
    const bool decoded = webp::referenceFrames(file, &reference);
    const PrStatus status = webp::praxFrames(file, &prax);
    uint64_t compared = 0;
    uint64_t differ = 0;
    uint32_t largest = 0;
    uint64_t hidden = 0;
    if (decoded && status == PrStatus_Ok && prax.rgba.count() == reference.rgba.count() &&
            prax.durations == reference.durations && prax.playCount == reference.playCount) {
        for (uint32_t i = 0; i < prax.rgba.count(); ++i) {
            // An image that says it has no alpha is opaque, whatever alpha its bitstream decodes to.
            if (!prax.alpha && i % 4 == 3) {
                hidden += reference.rgba[i] != 0xFF ? 1 : 0;
                continue;
            }
            compared += 1;
            const uint32_t a = prax.rgba[i];
            const uint32_t b = reference.rgba[i];
            const uint32_t delta = a > b ? a - b : b - a;
            differ += delta != 0 ? 1 : 0;
            largest = delta > largest ? delta : largest;
        }
    } else if (decoded && status == PrStatus_Ok) {
        differ = 1;
    }
    const char* planes = "none";
    webp::Planes referencePlanes;
    webp::Planes praxPlanes;
    if (reference.durations.count() == 1 && webp::referencePlanes(file, &referencePlanes)) {
        planes = "differ";
        const PrStatus decodedPlanes = webp::praxPlanes(file, &praxPlanes);
        if (decodedPlanes == PrStatus_NotSupported)
            planes = "none";
        else if (decodedPlanes == PrStatus_Ok && praxPlanes.y == referencePlanes.y &&
                praxPlanes.u == referencePlanes.u && praxPlanes.v == referencePlanes.v)
            planes = "equal";
    }
    prTextBufferAppend(&line, "{\"status\": ");
    harnessWriteString(&line, status == PrStatus_Ok ? PrStringView("") : prStatusName(status));
    harnessAppendFormat(&line, ", \"reference\": %s, \"frames\": %u, \"bytes\": %llu, \"differ\": %llu, "
        "\"largest\": %u, \"hidden\": %llu, \"planes\": \"%s\"", decoded ? "true" : "false", prax.durations.count(),
        static_cast<unsigned long long>(compared), static_cast<unsigned long long>(differ), largest,
        static_cast<unsigned long long>(hidden), planes);
    if (times) {
        Array<uint8_t> scratch;
        const HarnessCounters ours = harnessMeasure([&] { (void)webp::praxDecode(file, &scratch); });
        const HarnessCounters theirs = harnessMeasure([&] { (void)webp::referenceDecode(file, &scratch); });
        harnessAppendFormat(&line, ", \"time\": {\"prax\": %.3f, \"libwebp\": %.3f}, \"instructions\": {\"prax\": %.0f, \"libwebp\": %.0f}, \"cycles\": {\"prax\": %.0f, \"libwebp\": %.0f}",
            ours.millis, theirs.millis, ours.instructions, theirs.instructions, ours.cycles, theirs.cycles);
    }
    prTextBufferAppend(&line, "}");
    harnessWriteLine(prTextBufferView(&line));
    prTextBufferReset(&line);
    prTextBufferReset(&bytes);
}

// Decodes the file at `path` with one decoder, "prax" or "libwebp", over and over, for a profiler to sample.
int repeat(const char* engine, const char* path) {
    PrTextBuffer bytes = {};
    if (!harnessReadFile(&bytes, path))
        return 1;
    const PrByteView file = {reinterpret_cast<const uint8_t*>(bytes.data), bytes.size};
    const bool ours = PrStringView(engine) == "prax";
    Array<uint8_t> scratch;
    harnessRepeat([&] {
        if (ours)
            (void)webp::praxDecode(file, &scratch);
        else
            (void)webp::referenceDecode(file, &scratch);
    });
    prTextBufferReset(&bytes);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 4 && PrStringView(argv[1]) == "--repeat")
        return repeat(argv[2], argv[3]);
    bool times = false;
    int first = 1;
    if (first < argc && PrStringView(argv[first]) == "--time") {
        times = true;
        ++first;
    }
    for (int i = first; i < argc; ++i)
        run(argv[i], times);
    return 0;
}
