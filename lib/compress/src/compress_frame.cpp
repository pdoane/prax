// The zlib and gzip frames around Deflate: their headers, their checksums and gzip's members.
//
// rfc1950 - https://www.rfc-editor.org/rfc/rfc1950
// rfc1952 - https://www.rfc-editor.org/rfc/rfc1952

#include "compress/src/compress_frame.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/checksum.h"
#include "pr/mem.h"

namespace pr_impl {

namespace {

// Where a decoder is in the frame.
enum {
    FramePhase_ZlibHeader,
    FramePhase_GzipHeader,
    FramePhase_ExtraLength,
    FramePhase_Extra,
    FramePhase_Name,
    FramePhase_Comment,
    FramePhase_HeaderCrc,
    FramePhase_Data,
    FramePhase_Trailer,         // the trailer that follows the Deflate data
    FramePhase_Member,          // after a gzip member: the next one, or the end
    FramePhase_Done,
    FramePhase_Invalid,
};
typedef uint8_t FramePhase;

// What a read needs next, or how it ended.
enum {
    FrameStep_Input,
    FrameStep_Output,
    FrameStep_Done,
    FrameStep_Failed,   // the status says how
};
typedef uint8_t FrameStep;

// The gzip header's flags (rfc 1952 2.3.1).
enum {
    GzipFlag_HeaderCrc = 2,
    GzipFlag_Extra = 4,
    GzipFlag_Name = 8,
    GzipFlag_Comment = 16,
    GzipFlag_Reserved = 0xe0,
};
typedef uint8_t GzipFlag;

// The zlib header's flag for a preset dictionary (rfc 1950 2.2).
constexpr uint8_t kZlibDictionary = 32;

// The bytes at the end of the input that a Deflate decoder that may read past its stream's end is given one at a time,
// so that it ends exactly where its stream does. They are more than a trailer, and fewer than the smallest gzip member,
// so a stream that ends before them has another member or other bytes after it.
constexpr uint32_t kHeld = 16;

struct FrameDecoder : PrCompressDecoder {
    const PrCompressCodec* codec;
    PrCompressDecoder* PR_NULLABLE inner;   // the Deflate of the current gzip member, or of the zlib stream
    PrCompressFormat format;
    FramePhase phase;
    uint8_t flags;              // the gzip flags of the fields not yet read
    uint8_t header[10];         // a field read so far
    uint32_t headerSize;
    uint32_t headerCrc;
    uint32_t checksum;          // of the output: Adler-32 for zlib, CRC-32 for gzip
    uint32_t size;              // of a gzip member's output, modulo 2^32
    uint32_t remaining;         // of the gzip extra field
    uint8_t held[kHeld];        // read from the input, and not yet given to a decoder that may read past its end
    uint32_t heldSize;
    bool flushing;              // the decoder filled the output, and is given no input until it has room left
};

// The caller's spans, and the progress through them.
struct FrameSpan {
    PrByteView input;
    size_t consumed;
    uint8_t* PR_NULLABLE output;
    size_t capacity;
    size_t written;
};

uint32_t trailerSize(PrCompressFormat format) {
    return format == PrCompressFormat_Zlib ? 4 : 8;
}

// Reads the bytes of a field of `size` into the header. Returns false if the input ends first.
bool readField(FrameDecoder& d, FrameSpan& span, uint32_t size) {
    while (d.headerSize < size) {
        if (span.consumed == span.input.size)
            return false;
        d.header[d.headerSize++] = span.input.data[span.consumed++];
    }
    d.headerSize = 0;
    return true;
}

// Adds the bytes of the header that this step read, from `first`, to the header's CRC.
void checksumHeader(FrameDecoder& d, const FrameSpan& span, size_t first) {
    const PrByteView bytes{span.input.data != nullptr ? span.input.data + first : nullptr, span.consumed - first};
    d.headerCrc = prCrc32Update(d.headerCrc, bytes);
}

// Returns true if `trailer` holds the checksum, and for gzip the size, of the output.
bool trailerMatches(const FrameDecoder& d, const uint8_t* trailer) {
    if (d.format == PrCompressFormat_Zlib)
        return prMemReadBeU32(trailer) == d.checksum;
    return prMemReadLeU32(trailer) == d.checksum && prMemReadLeU32(trailer + 4) == d.size;
}

// Starts the Deflate data, with a decoder of its own.
PrStatus startData(FrameDecoder& d) {
    prCompressDecoderDestroy(d.inner);
    d.inner = nullptr;
    d.checksum = d.format == PrCompressFormat_Zlib ? 1 : 0;
    d.size = 0;
    d.heldSize = 0;
    d.phase = FramePhase_Data;
    PrCompressDecoder* inner = nullptr;
    PrStatus status = d.codec->decoderCreateFn(d.codec, PrCompressFormat_Deflate, &inner);
    d.inner = inner;
    return status;
}

// Gives `chunk` to the Deflate decoder, and adds what it decodes to the output and the checksum. Writes how much of the
// chunk it read to `*consumed`.
PrStatus decodeData(FrameDecoder& d, FrameSpan& span, PrByteView chunk, bool final, size_t* consumed,
        PrCompressProgress* progress) {
    uint8_t* to = span.output != nullptr ? span.output + span.written : nullptr;
    PrCompressRead read{0, 0, PrCompressProgress_Input};
    PrStatus status = d.inner->readFn(d.inner, chunk, final, to, span.capacity - span.written, &read);
    const PrByteView decoded{to, read.written};
    if (d.format == PrCompressFormat_Zlib)
        d.checksum = prAdler32Update(d.checksum, decoded);
    else
        d.checksum = prCrc32Update(d.checksum, decoded);
    d.size += uint32_t(read.written);
    span.written += read.written;
    *consumed = read.consumed;
    *progress = read.progress;
    return status;
}

// Removes the first `count` held bytes.
void dropHeld(FrameDecoder& d, size_t count) {
    prMemMove(d.held, d.held + count, d.heldSize - count);
    d.heldSize -= uint32_t(count);
}

// Reads the Deflate data with a decoder that may read past its end, and then the trailer, which must end the input. The
// decoder is given the input but its last kHeld bytes in pieces, and those one at a time.
FrameStep readHeldData(FrameDecoder& d, FrameSpan& span, bool final, PrStatus* status) {
    for (;;) {
        const size_t rest = span.input.size - span.consumed;
        const size_t available = d.heldSize + rest;
        const size_t kept = available < kHeld ? available : kHeld;
        PrByteView chunk{};
        bool fromHeld = d.heldSize != 0;
        bool one = false;
        if (d.flushing) {
            // The decoder may have read the end of its stream, so it is given nothing more until its output is out.
            fromHeld = false;
            one = available <= kept;
        } else if (available > kept) {
            if (fromHeld)
                chunk = PrByteView{d.held, d.heldSize < available - kept ? d.heldSize : available - kept};
            else
                chunk = PrByteView{span.input.data + span.consumed, available - kept};
        } else {
            // The rest of the input is held until it is final, and then given a byte at a time.
            if (rest != 0)
                prMemCopy(d.held + d.heldSize, span.input.data + span.consumed, rest);
            d.heldSize += uint32_t(rest);
            span.consumed = span.input.size;
            if (!final)
                return FrameStep_Input;
            if (d.heldSize == 0) {
                *status = PrStatus_Truncated;
                return FrameStep_Failed;
            }
            fromHeld = true;
            one = true;
            chunk = PrByteView{d.held, 1};
        }
        // The decoder's input is final only when this chunk is all that is left of it.
        const bool last = final && chunk.size == available;
        size_t consumed = 0;
        PrCompressProgress progress = PrCompressProgress_Input;
        *status = decodeData(d, span, chunk, last, &consumed, &progress);
        if (fromHeld)
            dropHeld(d, consumed);
        else
            span.consumed += consumed;
        if (*status != PrStatus_Ok)
            return FrameStep_Failed;
        d.flushing = progress == PrCompressProgress_Output;
        if (progress == PrCompressProgress_Output)
            return FrameStep_Output;
        if (progress == PrCompressProgress_Done) {
            // Ended before the held bytes, the stream has more than a trailer after it, which the decoder may have
            // read. Ended in them, it ended exactly, and the trailer must be the rest of the input.
            const uint32_t size = trailerSize(d.format);
            if (!one || d.heldSize > size) {
                *status = PrStatus_NotSupported;
                return FrameStep_Failed;
            }
            if (d.heldSize < size) {
                *status = PrStatus_Truncated;
                return FrameStep_Failed;
            }
            if (!trailerMatches(d, d.held)) {
                *status = PrStatus_InvalidData;
                return FrameStep_Failed;
            }
            d.heldSize = 0;
            d.phase = FramePhase_Done;
            return FrameStep_Done;
        }
        if (last) {
            *status = PrStatus_Truncated;
            return FrameStep_Failed;
        }
    }
}

// Reads the frame from the span until the read needs something, or the stream ends or fails.
FrameStep readFrame(FrameDecoder& d, FrameSpan& span, bool final, PrStatus* status) {
    for (;;) {
        const size_t first = span.consumed;
        switch (d.phase) {
            case FramePhase_ZlibHeader:
                if (!readField(d, span, 2))
                    return FrameStep_Input;
                if ((d.header[0] & 15) != 8 || (d.header[0] >> 4) > 7 || prMemReadBeU16(d.header) % 31 != 0) {
                    *status = PrStatus_InvalidData;
                    return FrameStep_Failed;
                }
                // A set has no means to give a preset dictionary.
                if ((d.header[1] & kZlibDictionary) != 0) {
                    *status = PrStatus_NotSupported;
                    return FrameStep_Failed;
                }
                *status = startData(d);
                if (*status != PrStatus_Ok)
                    return FrameStep_Failed;
                break;
            case FramePhase_GzipHeader: {
                const bool ready = readField(d, span, 10);
                checksumHeader(d, span, first);
                if (!ready)
                    return FrameStep_Input;
                if (d.header[0] != 0x1f || d.header[1] != 0x8b || d.header[2] != 8 || (d.header[3] & GzipFlag_Reserved) != 0) {
                    *status = PrStatus_InvalidData;
                    return FrameStep_Failed;
                }
                d.flags = d.header[3];
                d.phase = FramePhase_ExtraLength;
                break;
            }
            case FramePhase_ExtraLength:
                d.remaining = 0;
                if ((d.flags & GzipFlag_Extra) != 0) {
                    const bool ready = readField(d, span, 2);
                    checksumHeader(d, span, first);
                    if (!ready)
                        return FrameStep_Input;
                    d.remaining = prMemReadLeU16(d.header);
                }
                d.phase = FramePhase_Extra;
                break;
            case FramePhase_Extra: {
                const size_t available = span.input.size - span.consumed;
                const uint32_t count = available < d.remaining ? uint32_t(available) : d.remaining;
                span.consumed += count;
                d.remaining -= count;
                checksumHeader(d, span, first);
                if (d.remaining != 0)
                    return FrameStep_Input;
                d.phase = FramePhase_Name;
                break;
            }
            case FramePhase_Name:
            case FramePhase_Comment: {
                // The name and the comment end with a zero byte, and are skipped.
                const uint8_t flag = d.phase == FramePhase_Name ? GzipFlag_Name : GzipFlag_Comment;
                while ((d.flags & flag) != 0 && span.consumed < span.input.size) {
                    if (span.input.data[span.consumed++] == 0)
                        d.flags = uint8_t(d.flags & ~flag);
                }
                checksumHeader(d, span, first);
                if ((d.flags & flag) != 0)
                    return FrameStep_Input;
                d.phase = d.phase == FramePhase_Name ? FramePhase_Comment : FramePhase_HeaderCrc;
                break;
            }
            case FramePhase_HeaderCrc:
                if ((d.flags & GzipFlag_HeaderCrc) != 0) {
                    if (!readField(d, span, 2))
                        return FrameStep_Input;
                    if (prMemReadLeU16(d.header) != (d.headerCrc & 0xffff)) {
                        *status = PrStatus_InvalidData;
                        return FrameStep_Failed;
                    }
                }
                *status = startData(d);
                if (*status != PrStatus_Ok)
                    return FrameStep_Failed;
                break;
            case FramePhase_Data: {
                if (d.inner->readsPastEnd)
                    return readHeldData(d, span, final, status);
                const PrByteView rest{
                    span.input.data != nullptr ? span.input.data + span.consumed : nullptr,
                    span.input.size - span.consumed,
                };
                size_t consumed = 0;
                PrCompressProgress progress = PrCompressProgress_Input;
                *status = decodeData(d, span, rest, final, &consumed, &progress);
                span.consumed += consumed;
                if (*status != PrStatus_Ok)
                    return FrameStep_Failed;
                if (progress == PrCompressProgress_Input)
                    return FrameStep_Input;
                if (progress == PrCompressProgress_Output)
                    return FrameStep_Output;
                d.phase = FramePhase_Trailer;
                break;
            }
            case FramePhase_Trailer:
                if (!readField(d, span, trailerSize(d.format)))
                    return FrameStep_Input;
                if (!trailerMatches(d, d.header)) {
                    *status = PrStatus_InvalidData;
                    return FrameStep_Failed;
                }
                d.phase = d.format == PrCompressFormat_Zlib ? FramePhase_Done : FramePhase_Member;
                break;
            case FramePhase_Member:
                if (span.consumed == span.input.size) {
                    if (!final)
                        return FrameStep_Input;
                    d.phase = FramePhase_Done;
                    break;
                }
                d.headerCrc = 0;
                d.phase = FramePhase_GzipHeader;
                break;
            case FramePhase_Done:
                return FrameStep_Done;
            case FramePhase_Invalid:
                *status = PrStatus_InvalidData;
                return FrameStep_Failed;
        }
    }
}

void resetFrame(PrCompressDecoder* decoder) {
    FrameDecoder* frame = static_cast<FrameDecoder*>(decoder);
    prCompressDecoderDestroy(frame->inner);
    frame->inner = nullptr;
    frame->phase = frame->format == PrCompressFormat_Zlib ? FramePhase_ZlibHeader : FramePhase_GzipHeader;
    frame->headerSize = frame->headerCrc = frame->heldSize = 0;
    frame->flushing = false;
}

void destroyFrame(PrCompressDecoder* decoder) {
    FrameDecoder* frame = static_cast<FrameDecoder*>(decoder);
    prCompressDecoderDestroy(frame->inner);
    prMemoryFree(frame);
}

PrStatus readFrameStream(PrCompressDecoder* decoder, PrByteView input, bool final, uint8_t* PR_NULLABLE output,
        size_t capacity, PrCompressRead* out) {
    FrameDecoder& d = *static_cast<FrameDecoder*>(decoder);
    FrameSpan span{input, 0, output, capacity, 0};
    PrStatus status = PrStatus_Ok;
    FrameStep step = readFrame(d, span, final, &status);
    if (step == FrameStep_Input && final) {
        step = FrameStep_Failed;
        status = PrStatus_Truncated;
    }
    out->consumed = span.consumed;
    out->written = span.written;
    switch (step) {
        case FrameStep_Input: out->progress = PrCompressProgress_Input; return PrStatus_Ok;
        case FrameStep_Output: out->progress = PrCompressProgress_Output; return PrStatus_Ok;
        case FrameStep_Done: out->progress = PrCompressProgress_Done; return PrStatus_Ok;
        default:
            // After a failure, the decoder reads no further.
            d.phase = FramePhase_Invalid;
            return status;
    }
}

} // namespace

PrStatus compressFrameDecoderCreate(PrCompressFormat format, const PrCompressCodec* codec,
        PrCompressDecoder* PR_NULLABLE* PR_NONNULL out) {
    PR_ASSERT(compressIsFramed(format));
    FrameDecoder* decoder = allocZeroed<FrameDecoder>();
    decoder->destroyFn = destroyFrame;
    decoder->resetFn = resetFrame;
    decoder->readFn = readFrameStream;
    decoder->codec = codec;
    decoder->format = format;
    decoder->phase = format == PrCompressFormat_Zlib ? FramePhase_ZlibHeader : FramePhase_GzipHeader;
    *out = decoder;
    return PrStatus_Ok;
}

size_t compressFrameSize(PrCompressFormat format) {
    PR_ASSERT(compressIsFramed(format));
    return format == PrCompressFormat_Zlib ? 2 + 4 : 10 + 8;
}

size_t compressFrameWriteHeader(PrCompressFormat format, PrCompressLevel level, uint8_t* output) {
    PR_ASSERT(compressIsFramed(format));
    if (format == PrCompressFormat_Zlib) {
        // rfc 1950 2.2: Deflate over a 32K window, the level, and the check bits that make the header a multiple of 31.
        static constexpr uint8_t kLevelBits[] = {1, 2, 3};
        const uint32_t header = 0x7800u | uint32_t(kLevelBits[level]) << 6;
        const uint32_t checked = header + (31 - header % 31) % 31;
        output[0] = uint8_t(checked >> 8);
        output[1] = uint8_t(checked);
        return 2;
    }
    // rfc 1952 2.3: the Deflate method, no flags or time, the effort, and an unknown operating system.
    static constexpr uint8_t kEffortFlags[] = {4, 0, 2};
    const uint8_t header[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, kEffortFlags[level], 255};
    prMemCopy(output, header, sizeof(header));
    return sizeof(header);
}

size_t compressFrameWriteTrailer(PrCompressFormat format, PrByteView input, uint8_t* output) {
    PR_ASSERT(compressIsFramed(format));
    if (format == PrCompressFormat_Zlib) {
        prMemWriteBeU32(output, prAdler32Update(1, input));
        return 4;
    }
    prMemWriteLeU32(output, prCrc32Update(0, input));
    prMemWriteLeU32(output + 4, uint32_t(input.size));
    return 8;
}

} // namespace pr_impl
