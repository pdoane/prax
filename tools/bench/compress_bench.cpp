// compress_bench: what decoding a compressed stream costs in lib/compress's built-in codec, beside Apple's
// libcompression on the same streams where the platform has it. ./dev.sh bench compress runs it.
//
// Each input is compressed once in each format before anything is measured, and a case decodes one input's stream in
// one format whole, into a buffer that holds it. On Apple's platforms libcompression's encoder writes every stream, so
// that both engines decode the bytes that Apple's own tools write, including LZFSE's LZVN blocks for a small input.
// Elsewhere the built-in encoder writes the Deflate streams, and the formats that it does not encode have no cases. A
// row is one engine's cost of one decode: {"engine", "name", "time", "counts"}, written as a JSON array, where the name
// is "<input> <format>".
//
// Before a case is measured, each engine's output is checked against the input. A mismatch is reported, and fails the
// run.
//
// Usage: compress_bench [--input PATH]... [--small BYTES] [--repeat CASE [--engine NAME]] [CASE]...
//
// Each --input is a file, whose name names its cases. --small adds an input of the first BYTES bytes of the first file,
// named "<file>@<BYTES>". Measures the cases whose names start with one of the CASE arguments, or every case. --repeat
// runs one case on the named engine, prax by default, over and over for kHarnessRepeatSeconds, so that a profiler can
// sample it. It prints nothing.

#include "pr/assert.h"
#include "pr/compress.h"
#include "pr/compress_builtin.h"
#include "pr/text_buffer.h"

#include "tools/harness/harness.h"

#if defined(__APPLE__)
#include <compression.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace harness;

namespace {

struct Input {
    std::string name;
    std::vector<uint8_t> bytes;
};

struct Format {
    const char* name;
    PrCompressFormat format;
#if defined(__APPLE__)
    compression_algorithm algorithm;
#endif
};

// libcompression's ZLIB is bare Deflate, without zlib's framing.
#if defined(__APPLE__)
constexpr Format kFormats[] = {
    {"deflate", PrCompressFormat_Deflate, COMPRESSION_ZLIB},
    {"brotli", PrCompressFormat_Brotli, COMPRESSION_BROTLI},
    {"lzfse", PrCompressFormat_Lzfse, COMPRESSION_LZFSE},
};
#else
constexpr Format kFormats[] = {{"deflate", PrCompressFormat_Deflate}};
#endif

// A case: one input's stream in one format.
struct Case {
    std::string name;
    const Input* input;
    const Format* format;
    std::vector<uint8_t> stream;
};

// What each decode writes into: room for the largest input, and libcompression's scratch space, which is made once, as
// a client that decodes many streams keeps it.
struct Buffers {
    std::vector<uint8_t> output;
    std::vector<uint8_t> scratch;
};

// Returns the stream of `input` in `format`, or an empty stream if no encoder here writes the format.
std::vector<uint8_t> encode(PrCompressCodec* codec, const Format& format, const Input& input) {
    std::vector<uint8_t> stream;
#if defined(__APPLE__)
    (void)codec;
    // A stream of incompressible input can be a little larger than the input.
    stream.resize(input.bytes.size() + input.bytes.size() / 8 + 4096);
    size_t size = compression_encode_buffer(stream.data(), stream.size(), input.bytes.data(), input.bytes.size(),
        nullptr, format.algorithm);
    stream.resize(size);
#else
    size_t bound = prCompressCodecEncodeBound(codec, format.format, input.bytes.size());
    if (bound == 0)
        return stream;
    stream.resize(bound);
    size_t written = 0;
    PrStatus status = prCompressCodecEncode(codec, format.format, PrCompressLevel_Default,
        PrByteView(input.bytes), stream.data(), &written);
    stream.resize(status == PrStatus_Ok ? written : 0);
#endif
    return stream;
}

// Each engine's decode of a case's stream into the buffers' output. Returns false if the engine fails or writes other
// than the input's size.
bool decodePrax(PrCompressCodec* codec, const Case& test, Buffers* buffers) {
    PrCompressRead read{};
    PrStatus status = prCompressCodecDecode(codec, test.format->format, PrByteView(test.stream),
        buffers->output.data(), buffers->output.size(), &read);
    return status == PrStatus_Ok && read.written == test.input->bytes.size();
}

#if defined(__APPLE__)
bool decodeApple(const Case& test, Buffers* buffers) {
    size_t written = compression_decode_buffer(buffers->output.data(), buffers->output.size(), test.stream.data(),
        test.stream.size(), buffers->scratch.data(), test.format->algorithm);
    return written == test.input->bytes.size();
}
#endif

// The engines here, by name, which decode a case.
struct Engine {
    const char* name;
    bool (*decode)(PrCompressCodec* codec, const Case& test, Buffers* buffers);
};

#if defined(__APPLE__)
constexpr Engine kEngines[] = {
    {"prax", decodePrax},
    {"apple", [](PrCompressCodec*, const Case& test, Buffers* buffers) { return decodeApple(test, buffers); }},
};
#else
constexpr Engine kEngines[] = {{"prax", decodePrax}};
#endif

bool selected(const std::string& name, const std::vector<const char*>& prefixes) {
    if (prefixes.empty())
        return true;
    for (const char* prefix : prefixes) {
        if (name.compare(0, strlen(prefix), prefix) == 0)
            return true;
    }
    return false;
}

void writeRow(bool first, const char* engine, const std::string& name, const HarnessCounters& cost) {
    PrTextBuffer row{};
    prTextBufferAppend(&row, first ? "\n  {\"engine\": " : ",\n  {\"engine\": ");
    harnessWriteString(&row, engine);
    prTextBufferAppend(&row, ", \"name\": ");
    harnessWriteString(&row, name);
    harnessAppendFormat(&row, ", \"time\": %.9g", cost.millis);
    HarnessCounts counts{};
    harnessCountsAdd(&counts, "time", cost);
    harnessCountsWrite(&counts, &row);
    prTextBufferAppend(&row, "}");
    printf("%.*s", int(row.size), row.data);
    prTextBufferReset(&row);
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    const char* repeated = nullptr;
    const char* repeatedEngine = "prax";
    size_t small = 0;
    std::vector<const char*> paths;
    std::vector<const char*> prefixes;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc)
            paths.push_back(argv[++i]);
        else if (strcmp(argv[i], "--small") == 0 && i + 1 < argc)
            small = size_t(strtoull(argv[++i], nullptr, 10));
        else if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc)
            repeated = argv[++i];
        else if (strcmp(argv[i], "--engine") == 0 && i + 1 < argc)
            repeatedEngine = argv[++i];
        else
            prefixes.push_back(argv[i]);
    }

    std::vector<Input> inputs;
    PrTextBuffer file{};
    for (const char* path : paths) {
        if (!harnessReadFile(&file, path)) {
            fprintf(stderr, "cannot read %s\n", path);
            return 1;
        }
        PrStringView name = harnessFileName(path);
        const uint8_t* data = reinterpret_cast<const uint8_t*>(file.data);
        inputs.push_back(Input{std::string(name.data, name.size), std::vector<uint8_t>(data, data + file.size)});
    }
    prTextBufferReset(&file);
    if (small != 0 && !inputs.empty() && inputs[0].bytes.size() > small) {
        const std::vector<uint8_t>& whole = inputs[0].bytes;
        inputs.push_back(Input{
            inputs[0].name + "@" + std::to_string(small),
            std::vector<uint8_t>(whole.begin(), whole.begin() + ptrdiff_t(small)),
        });
    }

    PrCompressCodec* codec = prCompressBuiltinCreate();
    std::vector<Case> cases;
    Buffers buffers;
    for (const Input& input : inputs) {
        if (input.bytes.size() > buffers.output.size())
            buffers.output.resize(input.bytes.size());
        for (const Format& format : kFormats) {
            Case test{input.name + " " + format.name, &input, &format, encode(codec, format, input)};
            if (test.stream.empty())
                fprintf(stderr, "%s: no encoder here writes the stream\n", test.name.c_str());
            else
                cases.push_back(std::move(test));
        }
    }
#if defined(__APPLE__)
    for (const Format& format : kFormats) {
        size_t size = compression_decode_scratch_buffer_size(format.algorithm);
        if (size > buffers.scratch.size())
            buffers.scratch.resize(size);
    }
#endif

    if (repeated != nullptr) {
        for (const Case& test : cases) {
            for (const Engine& engine : kEngines) {
                if (test.name == repeated && strcmp(engine.name, repeatedEngine) == 0) {
                    harnessRepeat([&] { engine.decode(codec, test, &buffers); });
                    return 0;
                }
            }
        }
        fprintf(stderr, "no case named %s on engine %s\n", repeated, repeatedEngine);
        return 1;
    }

    bool failed = false;
    bool first = true;
    printf("[");
    for (const Case& test : cases) {
        if (!selected(test.name, prefixes))
            continue;
        for (const Engine& engine : kEngines) {
            memset(buffers.output.data(), 0, buffers.output.size());
            if (!engine.decode(codec, test, &buffers) ||
                    memcmp(buffers.output.data(), test.input->bytes.data(), test.input->bytes.size()) != 0) {
                fprintf(stderr, "%s: %s decodes the stream wrongly\n", test.name.c_str(), engine.name);
                failed = true;
                continue;
            }
            writeRow(first, engine.name, test.name, harnessMeasure([&] { engine.decode(codec, test, &buffers); }));
            first = false;
        }
    }
    printf("\n]\n");
    prCompressCodecDestroy(codec);
    return failed ? 1 : 0;
}
