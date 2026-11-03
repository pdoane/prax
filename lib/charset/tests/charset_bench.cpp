// The engine's own decoding measurements, without a shell.
//
// ./dev.sh bench text-decode runs this and prints its rows beside the rows that a browser reports. Its utf8 and
// windows1252 measures decode the same bytes as tools/bench/scripts/text-decode.js does, into a buffer, without the
// script's string. Two measures belong to this binary alone: the decode of UTF-8 that hands back the bytes themselves,
// which an HTML load makes, and the speed of reading the input once, which is the bound against which every pass over
// the bytes is measured.
//
// encoding - https://encoding.spec.whatwg.org/

#include "pr/assert.h"
#include "pr/charset.h"

#include "tools/harness/harness.h"

#include <stdio.h>

using namespace harness;

using namespace pr_impl;

namespace {

// The value that harnessReadBound folded the input into.
volatile uint8_t sink = 0;

// Decodes `bytes` whole in `charset` into `out`, which holds the most that the decode can write.
void decodeInto(PrCharset charset, PrStringView bytes, char* out) {
    PrCharsetDecoder decoder = prCharsetDecoderMake(charset, 0);
    size_t size = 0;
    prCharsetDecode(&decoder, bytes, PrCharsetInput_Final, out, &size);
    sink = uint8_t(size);
}

// Decodes `bytes` whole as UTF-8, as an HTML load does: the result is the bytes themselves if they are well-formed.
void decodeView(PrStringView bytes) {
    PrCharsetDecoder decoder = prCharsetDecoderMake(PrCharset_Utf8, 0);
    PrRawString scratch = {};
    PrStringView text = prCharsetDecodeView(&decoder, bytes, PrCharsetInput_Final, &scratch);
    sink = uint8_t(text.size);
    prRawStringReset(&scratch);
}

void measure(PrStringView name, PrStringView bytes) {
    PrCharsetDecoder sizing = prCharsetDecoderMake(PrCharset_Utf8, 0);
    PrTextBuffer out = {};
    prTextBufferResize(&out, uint32_t(prCharsetDecodeMaxSize(&sizing, bytes.size)));
    HarnessCounters utf8 = harnessMeasure([&] { decodeInto(PrCharset_Utf8, bytes, out.data); });
    HarnessCounters view = harnessMeasure([&] { decodeView(bytes); });
    HarnessCounters windows1252 = harnessMeasure([&] { decodeInto(PrCharset_Windows1252, bytes, out.data); });
    HarnessCounters bound = harnessMeasure([&] { sink = harnessReadBound(bytes); });
    prTextBufferReset(&out);

    HarnessCounts counts{};
    harnessCountsAdd(&counts, "utf8", utf8);
    harnessCountsAdd(&counts, "view", view);
    harnessCountsAdd(&counts, "windows1252", windows1252);
    harnessCountsAdd(&counts, "bound", bound);
    PrTextBuffer row{};
    prTextBufferAppend(&row, "{\"name\": ");
    harnessWriteString(&row, name);
    harnessAppendFormat(&row, ", \"bytes\": %zu, \"utf8\": %.4f, \"view\": %.4f, \"windows1252\": %.4f, \"bound\": %.4f",
        bytes.size, utf8.millis, view.millis, windows1252.millis, bound.millis);
    harnessCountsWrite(&counts, &row);
    prTextBufferAppend(&row, "}");
    harnessWriteLine(prTextBufferView(&row));
    prTextBufferReset(&row);
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    if (argc < 2) {
        fprintf(stderr, "usage: charset_bench <page>...\n");
        return 2;
    }

    for (int i = 1; i < argc; ++i) {
        PrTextBuffer page = {};
        if (!harnessReadFile(&page, argv[i])) {
            fprintf(stderr, "cannot read %s\n", argv[i]);
            return 2;
        }
        measure(harnessFileName(argv[i]), prTextBufferView(&page));
        prTextBufferReset(&page);
    }
    return 0;
}
