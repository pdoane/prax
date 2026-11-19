// LZ4 blocks.
//
// A block is a run of sequences. A sequence is a token, a run of literals, and a match: a two-byte distance back into
// the output and a length. The token's high four bits are the number of literals and its low four bits are the match's
// length less four, and a field of 15 is continued in bytes that add to it until one is less than 255. The last
// sequence of a block ends after its literals.

#include "pr/lz4.h"

#include "pr/alloc.h"
#include "pr/mem.h"
#include "pr/min_max.h"
#include "pr/raw_array.h"

namespace pr_impl {

namespace {

// How a decode ended, or what a read needs next.
enum {
    Lz4Result_Input,
    Lz4Result_Output,
    Lz4Result_Done,
    Lz4Result_Truncated,
    Lz4Result_Invalid,
};
typedef uint8_t Lz4Result;

// The shortest match, which a token's length field counts from.
constexpr size_t kMinMatch = 4;

// The most that one byte of a block decodes to: a continuation byte of a length.
constexpr uint64_t kMaxExpansion = 255;

// Reads the continuation of a length whose field in the token is 15. Returns false if the input ends inside it.
bool readLength(const uint8_t*& in, const uint8_t* end, size_t* length) {
    for (;;) {
        if (in == end)
            return false;
        uint8_t byte = *in++;
        *length += byte;
        if (byte != 255)
            return true;
    }
}

// A decoder of one block: the pieces of its input before the final one, and the output that reads have not taken.
struct Lz4Decoder {
    RawArray<uint8_t> input;    // the pieces before the final one
    RawArray<uint8_t> output;   // the decoded block, if a read had no room for all of it
    size_t taken;               // how much of `output` reads have returned
    bool decoded;               // true once the block has been decoded, into a read's output or into `output`
    Lz4Result ended;            // how the block ended, once `decoded`
};

Lz4Result decodeBlock(PrByteView block, uint8_t* PR_NULLABLE output, size_t capacity, size_t* written) {
    const uint8_t* in = block.data;
    const uint8_t* end = in + block.size;
    size_t at = 0;
    *written = 0;
    for (;;) {
        // A block ends after the literals of a sequence, so an end here is inside a sequence.
        if (in == end)
            return Lz4Result_Truncated;
        uint8_t token = *in++;
        size_t literals = token >> 4;
        if (literals == 15 && !readLength(in, end, &literals))
            return Lz4Result_Truncated;
        if (literals > size_t(end - in))
            return Lz4Result_Truncated;
        if (literals > capacity - at)
            return Lz4Result_Output;
        if (literals != 0)
            prMemCopy(output + at, in, literals);
        at += literals;
        in += literals;
        *written = at;
        if (in == end)
            return Lz4Result_Done;

        if (end - in < 2)
            return Lz4Result_Truncated;
        size_t distance = size_t(in[0]) | size_t(in[1]) << 8;
        in += 2;
        if (distance == 0 || distance > at)
            return Lz4Result_Invalid;
        size_t length = token & 15;
        if (length == 15 && !readLength(in, end, &length))
            return Lz4Result_Truncated;
        length += kMinMatch;
        if (length > capacity - at)
            return Lz4Result_Output;
        // A match longer than its distance repeats a pattern, so it copies forward.
        if (distance >= length) {
            prMemCopy(output + at, output + at - distance, length);
        } else {
            for (size_t i = 0; i < length; ++i)
                output[at + i] = output[at + i - distance];
        }
        at += length;
        *written = at;
    }
}

Lz4Result readBlock(Lz4Decoder& d, PrByteView bytes, bool final, size_t* consumed, uint8_t* PR_NULLABLE output,
        size_t capacity, size_t* written) {
    *consumed = 0;
    *written = 0;
    if (!d.decoded) {
        *consumed = bytes.size;
        if (!final) {
            d.input.append(bytes.data, uint32_t(bytes.size));
            return Lz4Result_Input;
        }
        PrByteView block = bytes;
        if (d.input.count != 0) {
            d.input.append(bytes.data, uint32_t(bytes.size));
            block = PrByteView{d.input.data, d.input.count};
        }
        d.decoded = true;
        // Most blocks arrive whole and fit, and decode into the read's own output.
        d.ended = decodeBlock(block, output, capacity, written);
        if (d.ended != Lz4Result_Output) {
            d.input.reset();
            return d.ended;
        }
        // The block is decoded again into storage that holds all of it, which the reads then drain.
        *written = 0;
        uint64_t most = min(uint64_t(block.size) * kMaxExpansion, uint64_t(kArrayMaxGrowth));
        for (uint64_t room = max(uint64_t(capacity) * 2, uint64_t(4096));; room *= 2) {
            d.output.resize(uint32_t(min(room, most)));
            size_t size = 0;
            d.ended = decodeBlock(block, d.output.data, d.output.count, &size);
            if (d.ended != Lz4Result_Output || room >= most) {
                d.output.resize(uint32_t(size));
                break;
            }
        }
        d.input.reset();
        if (d.ended == Lz4Result_Output)
            d.ended = Lz4Result_Invalid;
    }
    size_t count = min(capacity, size_t(d.output.count) - d.taken);
    if (count != 0)
        prMemCopy(output, d.output.data + d.taken, count);
    d.taken += count;
    *written = count;
    return d.taken < d.output.count ? Lz4Result_Output : d.ended;
}

// Writes how a block ended, or what a read needs, as the API reports it.
PrStatus statusOf(Lz4Result result, PrLz4Progress* progress) {
    switch (result) {
        case Lz4Result_Input: *progress = PrLz4Progress_Input; return PrStatus_Ok;
        case Lz4Result_Output: *progress = PrLz4Progress_Output; return PrStatus_Ok;
        case Lz4Result_Done: *progress = PrLz4Progress_Done; return PrStatus_Ok;
        case Lz4Result_Truncated: return PrStatus_Truncated;
        default: return PrStatus_InvalidData;
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrLz4Decoder : Lz4Decoder {};

PrStatus prLz4DecodeBlock(PrByteView block, uint8_t* PR_NULLABLE output, size_t capacity, PrLz4Read* out) {
    *out = PrLz4Read{};
    const Lz4Result result = decodeBlock(block, output, capacity, &out->written);
    if (result == Lz4Result_Done)
        out->consumed = block.size;
    return statusOf(result, &out->progress);
}

PrLz4Decoder* prLz4DecoderCreate(void) {
    return allocZeroed<PrLz4Decoder>();
}

void prLz4DecoderDestroy(PrLz4Decoder* PR_NULLABLE decoder) {
    if (decoder == nullptr)
        return;
    decoder->input.reset();
    decoder->output.reset();
    prMemoryFree(decoder);
}

void prLz4DecoderReset(PrLz4Decoder* decoder) {
    decoder->input.clear();
    decoder->output.clear();
    decoder->taken = 0;
    decoder->decoded = false;
    decoder->ended = Lz4Result_Input;
}

PrStatus prLz4DecoderRead(PrLz4Decoder* decoder, PrByteView input, bool final, uint8_t* PR_NULLABLE output,
        size_t capacity, PrLz4Read* out) {
    *out = PrLz4Read{};
    return statusOf(readBlock(*decoder, input, final, &out->consumed, output, capacity, &out->written), &out->progress);
}
