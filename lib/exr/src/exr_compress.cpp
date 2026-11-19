// The compressions of OpenEXR's pixel data that are not PIZ or DWA: RLE, ZIPS, ZIP, PXR24, B44 and B44A.

#include "exr/src/exr_compress.h"

#include "pr/float.h"
#include "pr/float16.h"
#include "pr/mem.h"
#include "pr/memory.h"

#include "exr/src/exr_dwa.h"
#include "exr/src/exr_piz.h"

namespace pr_impl {

namespace {

// Decodes RLE's runs from `packed` into exactly `size` bytes at `out`. A signed count below zero copies that many
// bytes, and any other repeats the next byte one more time than the count. Returns false for runs that end early or
// pass `size`.
bool runsDecode(PrByteView packed, uint8_t* out, size_t size) {
    size_t at = 0;
    size_t written = 0;
    while (at < packed.size) {
        int8_t count = int8_t(packed.data[at++]);
        if (count < 0) {
            size_t copied = size_t(-int32_t(count));
            if (copied > packed.size - at || copied > size - written)
                return false;
            prMemCopy(out + written, packed.data + at, copied);
            at += copied;
            written += copied;
        } else {
            size_t repeated = size_t(count) + 1;
            if (at == packed.size || repeated > size - written)
                return false;
            prMemFill(out + written, packed.data[at++], repeated);
            written += repeated;
        }
    }
    return written == size;
}

// Returns the sum of `count` bytes modulo 256.
uint8_t byteSum(const uint8_t* bytes, size_t count) {
    uint32_t sum = 0;
    for (size_t i = 0; i < count; ++i)
        sum += bytes[i];
    return uint8_t(sum);
}

// Inflates a PXR24 chunk and rebuilds its samples. PXR24 stores each line of each channel as planes of the bytes of the
// differences between successive samples, most significant first: four planes of a uint, two of a half, and three of a
// float, whose least significant byte it drops.
PrStatus pxr24Decompress(ExrCodec& codec, const ExrBlock& block, PrByteView packed, uint8_t* out, size_t size) {
    uint64_t planes = 0;
    for (uint32_t c = 0; c < block.channelCount; ++c) {
        const ExrBlockChannel& channel = block.channels[c];
        uint32_t bytes = channel.pixel == ExrPixel_Float ? 3 : exrPixelBytes(channel.pixel);
        planes += uint64_t(channel.width) * channel.height * bytes;
    }
    if (planes > 0xffffffffu)
        return PrStatus_InvalidData;
    codec.scratch.resize(uint32_t(planes));
    PrStatus status = exrInflate(codec, packed, codec.scratch.data, size_t(planes));
    if (status != PrStatus_Ok)
        return status;
    const uint8_t* in = codec.scratch.data;
    size_t written = 0;
    for (uint32_t row = 0; row < block.height; ++row) {
        int32_t y = block.y + int32_t(row);
        for (uint32_t c = 0; c < block.channelCount; ++c) {
            const ExrBlockChannel& channel = block.channels[c];
            if (channel.height == 0 || !exrChannelSamples(channel, y))
                continue;
            uint32_t width = channel.width;
            uint32_t pixelBytes = exrPixelBytes(channel.pixel);
            if (uint64_t(width) * pixelBytes > size - written)
                return PrStatus_InvalidData;
            uint8_t* line = out + written;
            uint32_t pixel = 0;
            if (channel.pixel == ExrPixel_Half) {
                const uint8_t* high = in;
                const uint8_t* low = in + width;
                for (uint32_t x = 0; x < width; ++x) {
                    pixel += uint32_t(high[x]) << 8 | low[x];
                    prMemWriteLeU16(line + size_t(x) * 2, uint16_t(pixel));
                }
                in += size_t(width) * 2;
            } else {
                const uint8_t* p0 = in;
                const uint8_t* p1 = in + width;
                const uint8_t* p2 = in + size_t(width) * 2;
                const uint8_t* p3 = in + size_t(width) * 3;
                bool full = channel.pixel == ExrPixel_Uint;
                for (uint32_t x = 0; x < width; ++x) {
                    uint32_t difference = uint32_t(p0[x]) << 24 | uint32_t(p1[x]) << 16 | uint32_t(p2[x]) << 8;
                    pixel += full ? difference | p3[x] : difference;
                    prMemWriteLeU32(line + size_t(x) * 4, pixel);
                }
                in += size_t(width) * (full ? 4 : 3);
            }
            written += size_t(width) * pixelBytes;
        }
    }
    return written == size ? PrStatus_Ok : PrStatus_InvalidData;
}

// Returns the bits of the half that is 8 ln(x) for the half of bits `bits`: B44's inverse of the exponential that it
// applies to a pLinear channel before it compresses one. Zero gives negative infinity, and a negative value, an
// infinity or a NaN gives zero. The logarithm is rounded to a float and then to a half.
uint16_t b44Log(uint16_t bits) {
    uint32_t exponent = bits >> 10 & 31;
    if (exponent == 31 || (bits & 0x8000) != 0)
        return (bits & 0x7fff) == 0 ? 0xfc00 : 0;
    if (bits == 0)
        return 0xfc00;
    PrFloat16 half{};
    prMemCopy(&half, &bits, sizeof(half));
    float logarithm = float(8 * prF64Log(double(prFloat16ToFloat(half))));
    PrFloat16 result = prFloat16FromFloat(logarithm);
    uint16_t out = 0;
    prMemCopy(&out, &result, sizeof(out));
    return out;
}

// Unpacks one of B44's blocks of 4 by 4 halves from its 14 bytes: the first sample, then a shift and fifteen 6-bit
// differences, each down a column of the block from the sample above and across the first row from the one to the left.
// The samples are in an ordered form that the end of the unpacking maps back to halves.
void b44Unpack14(const uint8_t* b, uint16_t* s) {
    s[0] = uint16_t(b[0] << 8 | b[1]);
    uint32_t shift = b[2] >> 2;
    uint32_t bias = 0x20u << shift;
    s[4] = uint16_t(s[0] + ((((uint32_t(b[2]) << 4) | (b[3] >> 4)) & 0x3f) << shift) - bias);
    s[8] = uint16_t(s[4] + ((((uint32_t(b[3]) << 2) | (b[4] >> 6)) & 0x3f) << shift) - bias);
    s[12] = uint16_t(s[8] + ((uint32_t(b[4]) & 0x3f) << shift) - bias);
    s[1] = uint16_t(s[0] + ((uint32_t(b[5]) >> 2) << shift) - bias);
    s[5] = uint16_t(s[4] + ((((uint32_t(b[5]) << 4) | (b[6] >> 4)) & 0x3f) << shift) - bias);
    s[9] = uint16_t(s[8] + ((((uint32_t(b[6]) << 2) | (b[7] >> 6)) & 0x3f) << shift) - bias);
    s[13] = uint16_t(s[12] + ((uint32_t(b[7]) & 0x3f) << shift) - bias);
    s[2] = uint16_t(s[1] + ((uint32_t(b[8]) >> 2) << shift) - bias);
    s[6] = uint16_t(s[5] + ((((uint32_t(b[8]) << 4) | (b[9] >> 4)) & 0x3f) << shift) - bias);
    s[10] = uint16_t(s[9] + ((((uint32_t(b[9]) << 2) | (b[10] >> 6)) & 0x3f) << shift) - bias);
    s[14] = uint16_t(s[13] + ((uint32_t(b[10]) & 0x3f) << shift) - bias);
    s[3] = uint16_t(s[2] + ((uint32_t(b[11]) >> 2) << shift) - bias);
    s[7] = uint16_t(s[6] + ((((uint32_t(b[11]) << 4) | (b[12] >> 4)) & 0x3f) << shift) - bias);
    s[11] = uint16_t(s[10] + ((((uint32_t(b[12]) << 2) | (b[13] >> 6)) & 0x3f) << shift) - bias);
    s[15] = uint16_t(s[14] + ((uint32_t(b[13]) & 0x3f) << shift) - bias);
    for (uint32_t i = 0; i < 16; ++i)
        s[i] = (s[i] & 0x8000) != 0 ? uint16_t(s[i] & 0x7fff) : uint16_t(~s[i]);
}

// Decompresses a B44 or B44A chunk. Each channel of halves is stored whole, as blocks of 4 by 4 samples from the top
// left, in 14 bytes each or, for a block of one value, in B44A's 3 bytes, whose shift byte is out of B44's range. A
// channel of another type is stored as it is. The chunk's lines are then put back together from the channels.
PrStatus b44Decompress(ExrCodec& codec, const ExrBlock& block, PrByteView packed, uint8_t* out, size_t size) {
    uint64_t planes = 0;
    for (uint32_t c = 0; c < block.channelCount; ++c) {
        const ExrBlockChannel& channel = block.channels[c];
        planes += uint64_t(channel.width) * channel.height * exrPixelBytes(channel.pixel);
    }
    if (planes != size)
        return PrStatus_InvalidData;
    codec.scratch.resize(uint32_t(planes));
    uint8_t* scratch = codec.scratch.data;
    const uint8_t* in = packed.data;
    const uint8_t* end = packed.data + packed.size;
    for (uint32_t c = 0; c < block.channelCount; ++c) {
        const ExrBlockChannel& channel = block.channels[c];
        uint32_t nx = channel.width;
        uint32_t ny = channel.height;
        size_t bytes = size_t(nx) * ny * exrPixelBytes(channel.pixel);
        if (bytes == 0)
            continue;
        if (channel.pixel != ExrPixel_Half) {
            if (bytes > size_t(end - in))
                return PrStatus_InvalidData;
            prMemCopy(scratch, in, bytes);
            in += bytes;
            scratch += bytes;
            continue;
        }
        if (channel.linear && codec.b44Log == nullptr) {
            codec.b44Log = static_cast<uint16_t*>(prMemoryAlloc(65536 * sizeof(uint16_t)));
            for (uint32_t i = 0; i < 65536; ++i)
                codec.b44Log[i] = b44Log(uint16_t(i));
        }
        for (uint32_t y = 0; y < ny; y += 4) {
            for (uint32_t x = 0; x < nx; x += 4) {
                uint16_t s[16];
                if (end - in < 3)
                    return PrStatus_InvalidData;
                if (in[2] >= 13 << 2) {
                    uint16_t value = uint16_t(in[0] << 8 | in[1]);
                    value = (value & 0x8000) != 0 ? uint16_t(value & 0x7fff) : uint16_t(~value);
                    for (uint16_t& sample : s)
                        sample = value;
                    in += 3;
                } else {
                    if (end - in < 14)
                        return PrStatus_InvalidData;
                    b44Unpack14(in, s);
                    in += 14;
                }
                if (channel.linear) {
                    for (uint16_t& sample : s)
                        sample = codec.b44Log[sample];
                }
                uint32_t columns = nx - x < 4 ? nx - x : 4;
                uint32_t rows = ny - y < 4 ? ny - y : 4;
                for (uint32_t r = 0; r < rows; ++r) {
                    uint8_t* row = scratch + (size_t(y + r) * nx + x) * 2;
                    for (uint32_t k = 0; k < columns; ++k)
                        prMemWriteLeU16(row + k * 2, s[r * 4 + k]);
                }
            }
        }
        scratch += bytes;
    }
    // The channels' planes become the chunk's lines.
    size_t written = 0;
    for (uint32_t row = 0; row < block.height; ++row) {
        int32_t y = block.y + int32_t(row);
        const uint8_t* plane = codec.scratch.data;
        for (uint32_t c = 0; c < block.channelCount; ++c) {
            const ExrBlockChannel& channel = block.channels[c];
            size_t lineBytes = size_t(channel.width) * exrPixelBytes(channel.pixel);
            if (channel.height != 0 && exrChannelSamples(channel, y)) {
                uint32_t line = channel.ySampling == 1 ? row : uint32_t(row / channel.ySampling);
                if (line >= channel.height || lineBytes > size - written)
                    return PrStatus_InvalidData;
                prMemCopy(out + written, plane + size_t(line) * lineBytes, lineBytes);
                written += lineBytes;
            }
            plane += lineBytes * channel.height;
        }
    }
    return written == size ? PrStatus_Ok : PrStatus_InvalidData;
}

} // namespace

uint32_t exrSchemeLines(ExrScheme compression) {
    switch (compression) {
        case ExrScheme_Zip:
        case ExrScheme_Pxr24:
            return 16;
        case ExrScheme_Piz:
        case ExrScheme_B44:
        case ExrScheme_B44a:
        case ExrScheme_Dwaa:
            return 32;
        case ExrScheme_Dwab:
            return 256;
        default:
            return 1;
    }
}

void exrCodecReset(ExrCodec& codec) {
    prCompressDecoderDestroy(codec.zlib);
    codec.zlib = nullptr;
    codec.scratch.reset();
    codec.scratch2.reset();
    codec.words.reset();
    prMemoryFree(codec.huffman);
    codec.huffman = nullptr;
    prMemoryFree(codec.b44Log);
    codec.b44Log = nullptr;
    prMemoryFree(codec.dwaToLinear);
    codec.dwaToLinear = nullptr;
}

PrStatus exrInflate(ExrCodec& codec, PrByteView packed, uint8_t* out, size_t size) {
    if (codec.zlib == nullptr) {
        PrCompressDecoder* decoder = nullptr;
        if (prCompressDecoderCreate(codec.compress, PrCompressFormat_Zlib, &decoder) != PrStatus_Ok)
            return PrStatus_NotSupported;
        codec.zlib = decoder;
    } else {
        prCompressDecoderReset(codec.zlib);
    }
    PrCompressRead read{};
    PrStatus status = prCompressDecoderRead(codec.zlib, packed, true, out, size, &read);
    if (status != PrStatus_Ok || read.progress != PrCompressProgress_Done || read.written != size)
        return status == PrStatus_NotSupported ? status : PrStatus_InvalidData;
    return PrStatus_Ok;
}

void exrUnpredict(uint8_t* bytes, uint8_t* out, size_t count) {
    if (count < 2) {
        if (count == 1)
            out[0] = bytes[0];
        return;
    }
    // Each byte is the previous one plus its delta, offset by 128. The first half of the bytes are those at even
    // offsets, and the second half those at odd offsets, which continue the first half's sums. The second half's start
    // is known from the first half's sum, so the two halves run side by side.
    size_t half = (count + 1) / 2;
    uint8_t first = bytes[0];
    uint8_t second = uint8_t(byteSum(bytes, half) - uint32_t(128) * uint32_t(half - 1));
    out[0] = first;
    size_t pairs = count / 2;
    if (pairs != 0) {
        second = uint8_t(second + bytes[half] - 128);
        out[1] = second;
    }
    // Four pairs at a time are stored as one word.
    size_t i = 1;
    const uint8_t* odd = bytes + half;
    for (; i + 4 <= pairs; i += 4) {
        uint64_t word = 0;
        for (uint32_t k = 0; k < 4; ++k) {
            first = uint8_t(first + bytes[i + k] - 128);
            second = uint8_t(second + odd[i + k] - 128);
            word |= (uint64_t(first) | uint64_t(second) << 8) << (16 * k);
        }
        prMemWriteLeU64(out + 2 * i, word);
    }
    for (; i < pairs; ++i) {
        first = uint8_t(first + bytes[i] - 128);
        second = uint8_t(second + odd[i] - 128);
        out[2 * i] = first;
        out[2 * i + 1] = second;
    }
    if (half != pairs)
        out[count - 1] = uint8_t(first + bytes[half - 1] - 128);
}

PrStatus exrDecompress(ExrCodec& codec, ExrScheme compression, const ExrBlock& block, PrByteView packed,
        uint8_t* out, size_t size) {
    switch (compression) {
        case ExrScheme_Rle: {
            if (size > 0xffffffffu)
                return PrStatus_InvalidData;
            codec.scratch.resize(uint32_t(size));
            if (!runsDecode(packed, codec.scratch.data, size))
                return PrStatus_InvalidData;
            exrUnpredict(codec.scratch.data, out, size);
            return PrStatus_Ok;
        }
        case ExrScheme_Zips:
        case ExrScheme_Zip: {
            if (size > 0xffffffffu)
                return PrStatus_InvalidData;
            codec.scratch.resize(uint32_t(size));
            PrStatus status = exrInflate(codec, packed, codec.scratch.data, size);
            if (status == PrStatus_Ok)
                exrUnpredict(codec.scratch.data, out, size);
            return status;
        }
        case ExrScheme_Piz:
            return exrPizDecompress(codec, block, packed, out, size);
        case ExrScheme_Pxr24:
            return pxr24Decompress(codec, block, packed, out, size);
        case ExrScheme_B44:
        case ExrScheme_B44a:
            return b44Decompress(codec, block, packed, out, size);
        case ExrScheme_Dwaa:
        case ExrScheme_Dwab:
            return exrDwaDecompress(codec, block, packed, out, size);
        default:
            return PrStatus_NotSupported;
    }
}

} // namespace pr_impl
