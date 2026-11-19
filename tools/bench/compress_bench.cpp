// compress_bench: what decoding and encoding cost in lib/compress's built-in codec, beside the reference libraries of
// each format (zlib, Brotli and Zstandard) and Apple's libcompression where the platform has it. ./dev.sh bench
// compress runs it over a stylesheet and a font, and ./dev.sh bench compress-corpus over the Silesia corpus with its
// encoders.
//
// Each input is compressed once in each format before anything is measured, by the format's reference encoder, and by
// libcompression for LZFSE, so that every engine decodes the same bytes. A decode case decodes one input's stream in
// one format whole, into a buffer that holds it. An encode case, with --encode, encodes one input whole at one of
// lib/compress's levels, and each reference library at its level of that name: zlib's 1, 6 and 9 for Deflate framed as
// zlib, and Zstandard's 1, 3 and 9. A checksum case, with --encode too, computes one input's CRC-32 or Adler-32, beside
// zlib's, or its XXH64, beside zstd's. A row is one engine's cost of one case: {"engine", "name", "time", "counts",
// "bytes"}, where "bytes" is the input's size, and an encode row has "size", the bytes it wrote. The rows are a JSON
// array, and the name is "<input> <case>". With more than one input, the rows named "all <case>" hold each engine's
// total over the inputs.
//
// Before a case is measured, each engine's output is checked: what a decoder writes against the input, and what an
// encoder writes by decoding it with the other side. A mismatch is reported, and fails the run.
//
// Usage: compress_bench [--input PATH]... [--small BYTES] [--encode] [--cache DIR] [--repeat CASE [--engine NAME]]
//                       [CASE]...
//
// Each --input is a file, whose name names its cases. --small adds an input of the first BYTES bytes of the first file,
// named "<file>@<BYTES>". --cache keeps the reference encoders' streams in DIR between runs, since the slowest of them
// take minutes over a large corpus. Measures the cases whose names start with one of the CASE arguments, or every case.
// --repeat runs one case on the named engine, prax by default, over and over for kHarnessRepeatSeconds, so that a
// profiler can sample it. It prints nothing.

#include "pr/assert.h"
#include "pr/checksum.h"
#include "pr/compress.h"
#include "pr/compress_select.h"
#include "pr/count_of.h"
#include "pr/text_buffer.h"
#include "pr/xxh64.h"

#include "tools/harness/harness.h"

#include "brotli/decode.h"
#include "brotli/encode.h"
#include "common/xxhash.h"
#include "zlib.h"
#include "zstd.h"

#if defined(__APPLE__)
#include <compression.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace harness;
using namespace pr;

namespace {

struct Input {
    std::string name;
    std::vector<uint8_t> bytes;
};

// The engines, which each case runs on those of them that read or write its format.
enum {
    Engine_Prax,
    Engine_Apple,
    Engine_Zlib,
    Engine_Brotli,
    Engine_Zstd,
    Engine_Count,
};
typedef uint8_t Engine;

constexpr const char* kEngineNames[Engine_Count] = {"prax", "apple", "zlib", "brotli", "zstd"};

// What a case does: decode a stream that a reference encoder wrote at `level`, encode at lib/compress's `level`, or
// compute a checksum.
enum {
    Work_Decode,
    Work_Encode,
    Work_Crc32,
    Work_Adler32,
    Work_Xxh64,
};
typedef uint8_t Work;

struct Kind {
    const char* name;
    PrCompressFormat format;
    Work work;
    PrCompressLevel level;      // of an encode case
    Engine writer;              // the engine whose encoder writes a decode case's stream
    int writerLevel;            // its level
    Engine reference;           // the reference engine that runs the case beside prax
    bool apple;                 // whether libcompression decodes the case too
};

constexpr Kind kKinds[] = {
    {"deflate", PrCompressFormat_Deflate, Work_Decode, 0, Engine_Zlib, 6, Engine_Zlib, true},
    {"gzip", PrCompressFormat_Gzip, Work_Decode, 0, Engine_Zlib, 6, Engine_Zlib, false},
    {"brotli-5", PrCompressFormat_Brotli, Work_Decode, 0, Engine_Brotli, 5, Engine_Brotli, true},
    {"brotli-11", PrCompressFormat_Brotli, Work_Decode, 0, Engine_Brotli, 11, Engine_Brotli, true},
#if defined(__APPLE__)
    {"lzfse", PrCompressFormat_Lzfse, Work_Decode, 0, Engine_Apple, 0, Engine_Apple, false},
#endif
    {"zstd-3", PrCompressFormat_Zstd, Work_Decode, 0, Engine_Zstd, 3, Engine_Zstd, false},
    {"zstd-19", PrCompressFormat_Zstd, Work_Decode, 0, Engine_Zstd, 19, Engine_Zstd, false},
    {"deflate-encode fast", PrCompressFormat_Zlib, Work_Encode, PrCompressLevel_Fast, 0, 0, Engine_Zlib, false},
    {"deflate-encode default", PrCompressFormat_Zlib, Work_Encode, PrCompressLevel_Default, 0, 0, Engine_Zlib, false},
    {"deflate-encode small", PrCompressFormat_Zlib, Work_Encode, PrCompressLevel_Small, 0, 0, Engine_Zlib, false},
    {"zstd-encode fast", PrCompressFormat_Zstd, Work_Encode, PrCompressLevel_Fast, 0, 0, Engine_Zstd, false},
    {"zstd-encode default", PrCompressFormat_Zstd, Work_Encode, PrCompressLevel_Default, 0, 0, Engine_Zstd, false},
    {"zstd-encode small", PrCompressFormat_Zstd, Work_Encode, PrCompressLevel_Small, 0, 0, Engine_Zstd, false},
    {"crc32", PrCompressFormat_Gzip, Work_Crc32, 0, 0, 0, Engine_Zlib, false},
    {"adler32", PrCompressFormat_Zlib, Work_Adler32, 0, 0, 0, Engine_Zlib, false},
    {"xxh64", PrCompressFormat_Zstd, Work_Xxh64, 0, 0, 0, Engine_Zstd, false},
};

// The reference encoders' level for each of lib/compress's levels, by format.
constexpr int kZlibLevels[] = {1, 6, 9};
constexpr int kZstdLevels[] = {1, 3, 9};

// A case: one input in one kind, and for a decode case, the stream that every engine decodes.
struct Case {
    std::string name;
    const Input* input;
    const Kind* kind;
    std::vector<uint8_t> stream;
};

// What each run writes into: room for the largest input or its largest stream, and libcompression's scratch space,
// which is made once, as a client that decodes many streams keeps it.
struct Buffers {
    std::vector<uint8_t> output;
    std::vector<uint8_t> scratch;
};

#if defined(__APPLE__)
compression_algorithm appleAlgorithm(PrCompressFormat format) {
    if (format == PrCompressFormat_Deflate)
        return COMPRESSION_ZLIB;
    return format == PrCompressFormat_Brotli ? COMPRESSION_BROTLI : COMPRESSION_LZFSE;
}
#endif

// zlib's stream interface over whole buffers, in the framing that `windowBits` chooses: negative for bare Deflate, 15
// for zlib, and 16 more for gzip. Each returns the bytes written, or zero if it failed or the output did not fit.
size_t zlibEncode(int level, int windowBits, const std::vector<uint8_t>& input, uint8_t* out, size_t capacity) {
    z_stream z{};
    if (deflateInit2(&z, level, Z_DEFLATED, windowBits, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return 0;
    z.next_in = input.data();
    z.avail_in = uInt(input.size());
    z.next_out = out;
    z.avail_out = uInt(capacity);
    int status = deflate(&z, Z_FINISH);
    size_t size = z.total_out;
    deflateEnd(&z);
    return status == Z_STREAM_END ? size : 0;
}

size_t zlibDecode(int windowBits, const std::vector<uint8_t>& stream, uint8_t* out, size_t capacity) {
    z_stream z{};
    if (inflateInit2(&z, windowBits) != Z_OK)
        return 0;
    z.next_in = stream.data();
    z.avail_in = uInt(stream.size());
    z.next_out = out;
    z.avail_out = uInt(capacity);
    int status = inflate(&z, Z_FINISH);
    size_t size = z.total_out;
    inflateEnd(&z);
    return status == Z_STREAM_END ? size : 0;
}

int zlibWindowBits(PrCompressFormat format) {
    return format == PrCompressFormat_Deflate ? -15 : format == PrCompressFormat_Gzip ? 16 + 15 : 15;
}

// Writes a decode case's stream with its kind's writer.
std::vector<uint8_t> writeStream(const Kind& kind, const Input& input) {
    std::vector<uint8_t> stream(input.bytes.size() + input.bytes.size() / 8 + 4096);
    size_t size = 0;
    if (kind.writer == Engine_Zlib) {
        size = zlibEncode(kind.writerLevel, zlibWindowBits(kind.format), input.bytes, stream.data(), stream.size());
    } else if (kind.writer == Engine_Brotli) {
        stream.resize(BrotliEncoderMaxCompressedSize(input.bytes.size()));
        size = stream.size();
        if (!BrotliEncoderCompress(kind.writerLevel, BROTLI_DEFAULT_WINDOW, BROTLI_MODE_GENERIC, input.bytes.size(),
                input.bytes.data(), &size, stream.data()))
            size = 0;
    } else if (kind.writer == Engine_Zstd) {
        stream.resize(ZSTD_compressBound(input.bytes.size()));
        size = ZSTD_compress(stream.data(), stream.size(), input.bytes.data(), input.bytes.size(), kind.writerLevel);
        if (ZSTD_isError(size))
            size = 0;
    } else {
#if defined(__APPLE__)
        size = compression_encode_buffer(stream.data(), stream.size(), input.bytes.data(), input.bytes.size(), nullptr,
            appleAlgorithm(kind.format));
#endif
    }
    stream.resize(size);
    return stream;
}

// Returns the stream of a decode case, from the cache in `cache` if it is there, and otherwise written and kept there.
std::vector<uint8_t> stream(const Kind& kind, const Input& input, const char* PR_NULLABLE cache) {
    std::string path;
    if (cache != nullptr) {
        path = std::string(cache) + "/" + input.name + "-" + std::to_string(input.bytes.size()) + "." + kind.name;
        PrTextBuffer file{};
        if (harnessReadFile(&file, path.c_str())) {
            const uint8_t* data = reinterpret_cast<const uint8_t*>(file.data);
            std::vector<uint8_t> kept(data, data + file.size);
            prTextBufferReset(&file);
            return kept;
        }
    }
    std::vector<uint8_t> written = writeStream(kind, input);
    if (cache != nullptr && !written.empty()) {
        FILE* out = fopen(path.c_str(), "wb");
        if (out != nullptr) {
            fwrite(written.data(), 1, written.size(), out);
            fclose(out);
        }
    }
    return written;
}

// Decodes a case's stream with `engine` into the buffers' output. Returns false if the engine fails or writes other
// than the input's size.
bool decode(const PrCompressCodecSet* codec, Engine engine, const Case& test, Buffers* buffers) {
    const std::vector<uint8_t>& s = test.stream;
    uint8_t* out = buffers->output.data();
    size_t capacity = buffers->output.size();
    size_t written = 0;
    switch (engine) {
        case Engine_Prax: {
            PrCompressRead read{};
            PrStatus status = prCompressCodecSetDecode(codec, test.kind->format, PrByteView(s), out, capacity, &read);
            written = status == PrStatus_Ok && read.progress == PrCompressProgress_Done ? read.written : 0;
            break;
        }
        case Engine_Zlib:
            written = zlibDecode(zlibWindowBits(test.kind->format), s, out, capacity);
            break;
        case Engine_Brotli:
            written = capacity;
            if (BrotliDecoderDecompress(s.size(), s.data(), &written, out) != BROTLI_DECODER_RESULT_SUCCESS)
                written = 0;
            break;
        case Engine_Zstd:
            written = ZSTD_decompress(out, capacity, s.data(), s.size());
            if (ZSTD_isError(written))
                written = 0;
            break;
        default:
#if defined(__APPLE__)
            written = compression_decode_buffer(out, capacity, s.data(), s.size(), buffers->scratch.data(),
                appleAlgorithm(test.kind->format));
#endif
            break;
    }
    return written == test.input->bytes.size();
}

// Encodes a case's input with `engine` into the buffers' output. Returns the bytes written, or zero if it failed.
size_t encode(const PrCompressCodecSet* codec, Engine engine, const Case& test, Buffers* buffers) {
    const std::vector<uint8_t>& in = test.input->bytes;
    uint8_t* out = buffers->output.data();
    size_t capacity = buffers->output.size();
    const Kind& kind = *test.kind;
    if (engine == Engine_Prax) {
        size_t written = 0;
        if (prCompressCodecSetEncodeBound(codec, kind.format, in.size()) > capacity)
            return 0;
        PrStatus status = prCompressCodecSetEncode(codec, kind.format, kind.level, PrByteView(in), out, &written);
        return status == PrStatus_Ok ? written : 0;
    }
    if (engine == Engine_Zlib)
        return zlibEncode(kZlibLevels[kind.level], zlibWindowBits(kind.format), in, out, capacity);
    size_t size = ZSTD_compress(out, capacity, in.data(), in.size(), kZstdLevels[kind.level]);
    return ZSTD_isError(size) ? 0 : size;
}

// Computes a checksum case's checksum of its input with `engine`.
uint64_t checksum(Engine engine, const Case& test) {
    const std::vector<uint8_t>& in = test.input->bytes;
    if (test.kind->work == Work_Crc32)
        return engine == Engine_Prax ? prCrc32Update(0, PrByteView(in)) : crc32_z(0, in.data(), in.size());
    if (test.kind->work == Work_Xxh64)
        return engine == Engine_Prax ? prXxh64(PrByteView(in), 0) : XXH64(in.data(), in.size(), 0);
    return engine == Engine_Prax ? prAdler32Update(1, PrByteView(in)) : adler32_z(1, in.data(), in.size());
}

// Returns true if what an encoder wrote at the start of the buffers' output decodes to the input, by the other side:
// the reference decoder for prax's stream, and prax for the reference's.
bool encodedRight(const PrCompressCodecSet* codec, Engine engine, const Case& test, Buffers* buffers, size_t size) {
    Case check{
        test.name, test.input, test.kind, std::vector<uint8_t>(buffers->output.begin(),
            buffers->output.begin() + ptrdiff_t(size)),
    };
    Buffers decoded{std::vector<uint8_t>(test.input->bytes.size() + 1), {}};
    Engine decoder = engine == Engine_Prax ? test.kind->reference : Engine_Prax;
    return decode(codec, decoder, check, &decoded) &&
        memcmp(decoded.output.data(), test.input->bytes.data(), test.input->bytes.size()) == 0;
}

// The engines that run a case.
std::vector<Engine> engines(const Case& test) {
    std::vector<Engine> out{Engine_Prax, test.kind->reference};
#if defined(__APPLE__)
    if (test.kind->apple)
        out.push_back(Engine_Apple);
#endif
    return out;
}

bool selected(const std::string& name, const std::vector<const char*>& prefixes) {
    if (prefixes.empty())
        return true;
    for (const char* prefix : prefixes) {
        if (name.compare(0, strlen(prefix), prefix) == 0)
            return true;
    }
    return false;
}

void writeRow(bool first, const char* engine, const std::string& name, const HarnessCounters& cost, size_t bytes, size_t size) {
    PrTextBuffer row{};
    prTextBufferAppend(&row, first ? "\n  {\"engine\": " : ",\n  {\"engine\": ");
    harnessWriteString(&row, engine);
    prTextBufferAppend(&row, ", \"name\": ");
    harnessWriteString(&row, name);
    harnessAppendFormat(&row, ", \"time\": %.9g, \"bytes\": %zu", cost.millis, bytes);
    if (size != 0)
        harnessAppendFormat(&row, ", \"size\": %zu", size);
    HarnessCounts counts{};
    harnessCountsAdd(&counts, "time", cost);
    harnessCountsWrite(&counts, &row);
    prTextBufferAppend(&row, "}");
    printf("%.*s", int(row.size), row.data);
    prTextBufferReset(&row);
}

// An engine's total over the inputs of one kind of case.
struct Total {
    HarnessCounters cost;
    size_t bytes;
    size_t size;
    uint32_t cases;
};

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    const char* repeated = nullptr;
    const char* repeatedEngine = "prax";
    const char* cache = nullptr;
    bool encoding = false;
    size_t small = 0;
    std::vector<const char*> paths;
    std::vector<const char*> prefixes;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc)
            paths.push_back(argv[++i]);
        else if (strcmp(argv[i], "--small") == 0 && i + 1 < argc)
            small = size_t(strtoull(argv[++i], nullptr, 10));
        else if (strcmp(argv[i], "--cache") == 0 && i + 1 < argc)
            cache = argv[++i];
        else if (strcmp(argv[i], "--encode") == 0)
            encoding = true;
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

    PrCompressCodecSet* codec = prCompressCodecSetCreateDefault();
    if (codec == nullptr)
        return 1;
    std::vector<Case> cases;
    Buffers buffers;
    for (const Input& input : inputs) {
        // Room for the input, or for the stream of an input that does not compress.
        size_t room = input.bytes.size() + input.bytes.size() / 8 + 4096;
        if (room > buffers.output.size())
            buffers.output.resize(room);
        for (const Kind& kind : kKinds) {
            Case test{input.name + " " + kind.name, &input, &kind, {}};
            if (kind.work != Work_Decode) {
                if (encoding)
                    cases.push_back(std::move(test));
                continue;
            }
            if (!selected(test.name, prefixes) && !selected("all " + std::string(kind.name), prefixes) &&
                    (repeated == nullptr || test.name != repeated))
                continue;
            test.stream = stream(kind, input, cache);
            if (test.stream.empty())
                fprintf(stderr, "%s: no encoder here writes the stream\n", test.name.c_str());
            else
                cases.push_back(std::move(test));
        }
    }
#if defined(__APPLE__)
    for (const Kind& kind : kKinds) {
        if (kind.apple || kind.writer == Engine_Apple) {
            size_t size = compression_decode_scratch_buffer_size(appleAlgorithm(kind.format));
            if (size > buffers.scratch.size())
                buffers.scratch.resize(size);
        }
    }
#endif

    // What the checksums come to, so that computing them is not optimized away.
    uint64_t sums = 0;
    auto run = [&](Engine engine, const Case& test) {
        if (test.kind->work == Work_Encode)
            encode(codec, engine, test, &buffers);
        else if (test.kind->work == Work_Decode)
            decode(codec, engine, test, &buffers);
        else
            sums += checksum(engine, test);
    };

    if (repeated != nullptr) {
        for (const Case& test : cases) {
            for (Engine engine : engines(test)) {
                if (test.name == repeated && strcmp(kEngineNames[engine], repeatedEngine) == 0) {
                    harnessRepeat([&] { run(engine, test); });
                    return 0;
                }
            }
        }
        fprintf(stderr, "no case named %s on engine %s\n", repeated, repeatedEngine);
        return 1;
    }

    bool failed = false;
    bool first = true;
    // The totals of each engine by kind.
    std::vector<Total> totals(countOf(kKinds) * Engine_Count, Total{});
    printf("[");
    for (const Case& test : cases) {
        bool all = selected("all " + std::string(test.kind->name), prefixes);
        if (!selected(test.name, prefixes) && !all)
            continue;
        for (Engine engine : engines(test)) {
            size_t size = 0;
            if (test.kind->work >= Work_Crc32) {
                if (checksum(engine, test) != checksum(test.kind->reference, test)) {
                    fprintf(stderr, "%s: %s computes the checksum wrongly\n", test.name.c_str(), kEngineNames[engine]);
                    failed = true;
                    continue;
                }
            } else if (test.kind->work == Work_Encode) {
                size = encode(codec, engine, test, &buffers);
                if (size == 0 && engine == Engine_Prax)
                    continue;   // a format that lib/compress does not encode
                if (size == 0 || !encodedRight(codec, engine, test, &buffers, size)) {
                    fprintf(stderr, "%s: %s encodes the input wrongly\n", test.name.c_str(), kEngineNames[engine]);
                    failed = true;
                    continue;
                }
            } else {
                memset(buffers.output.data(), 0, buffers.output.size());
                if (!decode(codec, engine, test, &buffers) ||
                        memcmp(buffers.output.data(), test.input->bytes.data(), test.input->bytes.size()) != 0) {
                    fprintf(stderr, "%s: %s decodes the stream wrongly\n", test.name.c_str(), kEngineNames[engine]);
                    failed = true;
                    continue;
                }
            }
            HarnessCounters cost = harnessMeasure([&] { run(engine, test); });
            if (selected(test.name, prefixes)) {
                writeRow(first, kEngineNames[engine], test.name, cost, test.input->bytes.size(), size);
                first = false;
            }
            Total& total = totals[size_t(test.kind - kKinds) * Engine_Count + engine];
            total.cost = harnessSum(total.cost, cost);
            total.bytes += test.input->bytes.size();
            total.size += size;
            total.cases += 1;
        }
    }
    if (inputs.size() > 1) {
        for (size_t k = 0; k < countOf(kKinds); ++k) {
            for (Engine engine = 0; engine < Engine_Count; ++engine) {
                const Total& total = totals[k * Engine_Count + engine];
                if (total.cases < 2)
                    continue;
                writeRow(first, kEngineNames[engine], "all " + std::string(kKinds[k].name), total.cost, total.bytes,
                    total.size);
                first = false;
            }
        }
    }
    printf("\n]\n");
    if (sums == 1)
        fprintf(stderr, "every checksum came to one\n");
    prCompressCodecSetDestroy(codec);
    return failed ? 1 : 0;
}
