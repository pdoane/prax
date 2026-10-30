// Charsets: the encodings of the Encoding Standard, decoded to UTF-8 and encoded from it.

#include "pr/charset.h"

#include "pr/assert.h"
#include "pr/bit_cast.h"
#include "pr/bits.h"
#include "pr/bitset.h"
#include "pr/count_of.h"
#include "pr/mem.h"
#include "pr/mime.h"
#include "pr/search.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/utf8.h"

#include "charset/src/charset_tables.h"

namespace pr_impl {

namespace {

// What a decoder's handler does with one byte (encoding/#concept-encoding-run).
enum {
    CharsetResult_Continue,  // the byte was read, and any code points it completes were written
    CharsetResult_Error,
    CharsetResult_Finished,  // the decoder reads no more of the input
};
typedef uint8_t CharsetResult;

// The item that a handler reads after the last byte of a final input (encoding/#concept-encoding-run).
constexpr int32_t kEndOfQueue = -1;

// The states of the ISO-2022-JP decoder (encoding/#iso-2022-jp-decoder).
enum {
    Iso2022JpState_Ascii,
    Iso2022JpState_Roman,
    Iso2022JpState_Katakana,
    Iso2022JpState_LeadByte,
    Iso2022JpState_TrailByte,
    Iso2022JpState_EscapeStart,
    Iso2022JpState_Escape,
};
typedef uint8_t Iso2022JpState;

// The bytes of one part of the input, after the bytes that a handler restored to its front
// (encoding/#concept-stream-prepend). A handler restores at most three bytes, and reads them before any other.
struct CharsetQueue {
    const uint8_t* bytes;
    size_t size;
    size_t at;
    bool final;
    uint8_t front[3];
    uint8_t frontAt;
    uint8_t frontCount;
};

// Returns the next byte, kEndOfQueue at the end of a final input, or kNoByte at the end of a partial one.
constexpr int32_t kNoByte = -2;

int32_t queueRead(CharsetQueue* queue) {
    if (queue->frontAt < queue->frontCount)
        return queue->front[queue->frontAt++];
    if (queue->at < queue->size)
        return queue->bytes[queue->at++];
    return queue->final ? kEndOfQueue : kNoByte;
}

// Restores bytes to the front of the queue, to be read before what remains. The end of the queue needs no restoring,
// because reading past the end reads it again. A handler restores only bytes that it has read since the front last
// emptied, so the restored bytes go at the front in the order of the calls.
void queueRestore(CharsetQueue* queue, int32_t byte) {
    if (byte == kEndOfQueue)
        return;
    if (queue->frontAt == queue->frontCount) {
        queue->frontAt = 0;
        queue->frontCount = 0;
    }
    PR_ASSERT(queue->frontCount < sizeof(queue->front));
    queue->front[queue->frontCount++] = uint8_t(byte);
}

bool queueFrontIsEmpty(const CharsetQueue* queue) {
    return queue->frontAt == queue->frontCount;
}

// The text that a decode writes.
struct CharsetWriter {
    PrCharsetDecoder* decoder;
    char* out;
    size_t size;
};

bool removesBom(const PrCharsetDecoder* decoder) {
    return (decoder->flags & PrCharsetDecoderFlag_KeepBom) == 0 &&
        (decoder->charset == PrCharset_Utf8 || decoder->charset == PrCharset_Utf16Be ||
            decoder->charset == PrCharset_Utf16Le);
}

// Writes `codePoint`. The first code point of the input is dropped if it is a byte order mark that the decoder removes
// (encoding/#dom-textdecoder-decode).
void emit(CharsetWriter* w, uint32_t codePoint) {
    if (!w->decoder->bomSeen) {
        w->decoder->bomSeen = true;
        if (codePoint == 0xfeff && removesBom(w->decoder))
            return;
    }
    char* out = w->out + w->size;
    if (codePoint < 0x80) {
        out[0] = char(codePoint);
        w->size += 1;
        return;
    }
    w->size += prUtf8Encode(codePoint, out);
}

// Writes bytes that decode to themselves: ASCII, or well-formed UTF-8 in a UTF-8 decoder.
void emitBytes(CharsetWriter* w, const uint8_t* bytes, size_t size) {
    if (size == 0)
        return;
    if (!w->decoder->bomSeen) {
        w->decoder->bomSeen = true;
        if (size >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf && removesBom(w->decoder)) {
            bytes += 3;
            size -= 3;
        }
    }
    memCopy(w->out + w->size, bytes, size);
    w->size += size;
}

constexpr bool isAscii(int32_t byte) {
    return byte >= 0 && byte < 0x80;
}

constexpr bool inRange(int32_t byte, int32_t low, int32_t high) {
    return byte >= low && byte <= high;
}

// Returns the length of the run of ASCII bytes that starts `bytes`. Eight bytes are read at a time, and the lowest high
// bit of a word that has one is the end of the run.
size_t asciiRun(const uint8_t* bytes, size_t size) {
    size_t at = 0;
    for (; size - at >= 8; at += 8) {
        uint64_t high = loadUnaligned<uint64_t>(bytes + at) & 0x8080808080808080ull;
        if (high != 0)
            return at + lowestBitU64(high) / 8;
    }
    while (at < size && bytes[at] < 0x80)
        ++at;
    return at;
}

// A validating automaton of UTF-8 (encoding/#utf-8-decoder), whose state is a shift amount. The row for a byte holds
// six bits for each state, giving the next state after that byte. The bounds of each second byte exclude the overlong
// forms, the surrogates and the values past U+10FFFF.
enum : uint64_t {
    Utf8State_Accept = 0 * 6,  // between sequences
    Utf8State_One = 1 * 6,     // one continuation byte left
    Utf8State_Two = 2 * 6,     // two left
    Utf8State_Three = 3 * 6,   // three left
    Utf8State_E0 = 4 * 6,      // after 0xe0, whose second byte is 0xa0 to 0xbf
    Utf8State_Ed = 5 * 6,      // after 0xed, whose second byte is 0x80 to 0x9f
    Utf8State_F0 = 6 * 6,      // after 0xf0, whose second byte is 0x90 to 0xbf
    Utf8State_F4 = 7 * 6,      // after 0xf4, whose second byte is 0x80 to 0x8f
    Utf8State_Error = 8 * 6,   // an ill-formed sequence has been read
    Utf8State_Count = 9,
};
typedef uint64_t Utf8State;

constexpr Utf8State utf8Next(Utf8State state, uint32_t byte) {
    bool continuation = (byte & 0xc0) == 0x80;
    switch (state) {
        case Utf8State_Accept:
            if (byte < 0x80)
                return Utf8State_Accept;
            if (byte >= 0xc2 && byte <= 0xdf)
                return Utf8State_One;
            if (byte == 0xe0)
                return Utf8State_E0;
            if (byte == 0xed)
                return Utf8State_Ed;
            if (byte >= 0xe1 && byte <= 0xef)
                return Utf8State_Two;
            if (byte == 0xf0)
                return Utf8State_F0;
            if (byte >= 0xf1 && byte <= 0xf3)
                return Utf8State_Three;
            if (byte == 0xf4)
                return Utf8State_F4;
            return Utf8State_Error;
        case Utf8State_One:   return continuation ? Utf8State_Accept : Utf8State_Error;
        case Utf8State_Two:   return continuation ? Utf8State_One : Utf8State_Error;
        case Utf8State_Three: return continuation ? Utf8State_Two : Utf8State_Error;
        case Utf8State_E0:    return byte >= 0xa0 && byte <= 0xbf ? Utf8State_One : Utf8State_Error;
        case Utf8State_Ed:    return byte >= 0x80 && byte <= 0x9f ? Utf8State_One : Utf8State_Error;
        case Utf8State_F0:    return byte >= 0x90 && byte <= 0xbf ? Utf8State_Two : Utf8State_Error;
        case Utf8State_F4:    return byte >= 0x80 && byte <= 0x8f ? Utf8State_Two : Utf8State_Error;
        default:              return Utf8State_Error;
    }
}

struct Utf8Table {
    uint64_t row[256];
};

constexpr Utf8Table makeUtf8Table() {
    Utf8Table table = {};
    for (uint32_t b = 0; b < 256; ++b) {
        for (Utf8State s = 0; s < Utf8State_Count; ++s)
            table.row[b] |= utf8Next(s * 6, b) << (s * 6);
    }
    return table;
}

constexpr Utf8Table kUtf8 = makeUtf8Table();

// Returns the length of the run of well-formed UTF-8 that starts `bytes`, which ends before an ill-formed or incomplete
// sequence. The automaton reads sixteen bytes at a time, and skips a block of ASCII between sequences whole. It tracks
// only the last block boundary that falls between sequences. A run that ends within a sequence is read again from that
// boundary, to find where its last complete sequence ends.
size_t utf8Run(const uint8_t* bytes, size_t size) {
    Utf8State state = Utf8State_Accept;
    size_t between = 0;    // the last block boundary between sequences
    size_t at = 0;
    for (; size - at >= 16; at += 16) {
        uint64_t words = loadUnaligned<uint64_t>(bytes + at) | loadUnaligned<uint64_t>(bytes + at + 8);
        if (state == Utf8State_Accept && (words & 0x8080808080808080ull) == 0)
            continue;
        if (state == Utf8State_Accept)
            between = at;
        for (uint32_t k = 0; k < 16; ++k)
            state = (kUtf8.row[bytes[at + k]] >> state) & 63;
        if (state == Utf8State_Error)
            break;
    }
    if (state != Utf8State_Error) {
        for (; at < size; ++at) {
            if (state == Utf8State_Accept)
                between = at;
            state = (kUtf8.row[bytes[at]] >> state) & 63;
        }
        if (state == Utf8State_Accept)
            return size;
    }
    // The run ends within a sequence after `between`, which the automaton reaches between sequences.
    size_t boundary = between;
    state = Utf8State_Accept;
    for (size_t i = between; i < size; ++i) {
        state = (kUtf8.row[bytes[i]] >> state) & 63;
        if (state == Utf8State_Error)
            break;
        if (state == Utf8State_Accept)
            boundary = i + 1;
    }
    return boundary;
}

// ---------------------------------------------------------------------------------------------------------------------
// Handlers

CharsetResult utf8Handler(PrCharsetDecoder* d, CharsetQueue* queue, int32_t byte, CharsetWriter* w) {
    if (byte == kEndOfQueue) {
        if (d->needed == 0)
            return CharsetResult_Finished;
        d->needed = 0;
        return CharsetResult_Error;
    }
    if (d->needed == 0) {
        if (isAscii(byte)) {
            emit(w, uint32_t(byte));
        } else if (inRange(byte, 0xc2, 0xdf)) {
            d->needed = 1;
            d->codePoint = uint32_t(byte) & 0x1f;
        } else if (inRange(byte, 0xe0, 0xef)) {
            if (byte == 0xe0)
                d->lower = 0xa0;
            if (byte == 0xed)
                d->upper = 0x9f;
            d->needed = 2;
            d->codePoint = uint32_t(byte) & 0xf;
        } else if (inRange(byte, 0xf0, 0xf4)) {
            if (byte == 0xf0)
                d->lower = 0x90;
            if (byte == 0xf4)
                d->upper = 0x8f;
            d->needed = 3;
            d->codePoint = uint32_t(byte) & 0x7;
        } else {
            return CharsetResult_Error;
        }
        return CharsetResult_Continue;
    }
    if (!inRange(byte, d->lower, d->upper)) {
        d->codePoint = 0;
        d->needed = 0;
        d->seen = 0;
        d->lower = 0x80;
        d->upper = 0xbf;
        queueRestore(queue, byte);
        return CharsetResult_Error;
    }
    d->lower = 0x80;
    d->upper = 0xbf;
    d->codePoint = (d->codePoint << 6) | (uint32_t(byte) & 0x3f);
    if (++d->seen != d->needed)
        return CharsetResult_Continue;
    uint32_t codePoint = d->codePoint;
    d->codePoint = 0;
    d->needed = 0;
    d->seen = 0;
    emit(w, codePoint);
    return CharsetResult_Continue;
}

CharsetResult xUserDefinedHandler(int32_t byte, CharsetWriter* w) {
    if (byte == kEndOfQueue)
        return CharsetResult_Finished;
    emit(w, isAscii(byte) ? uint32_t(byte) : 0xf780 + uint32_t(byte) - 0x80);
    return CharsetResult_Continue;
}

// Returns the code point of `pointer` in index gb18030 ranges, or zero for none
// (encoding/#index-gb18030-ranges-code-point).
uint32_t gb18030RangesCodePoint(uint32_t pointer) {
    if ((pointer > 39419 && pointer < 189000) || pointer > 1237575)
        return 0;
    if (pointer == 7457)
        return 0xe7c7;
    // The last range from the second on that starts at or before `pointer`, or the first.
    uint32_t low = partitionPoint(1, uint32_t(countOf(kCharsetGb18030Ranges)),
        [&](uint32_t i) { return kCharsetGb18030Ranges[i].pointer <= pointer; }) - 1;
    const CharsetGb18030Range& range = kCharsetGb18030Ranges[low];
    return range.codePoint + pointer - range.pointer;
}

// The decoder of gb18030, which GBK shares (encoding/#gb18030-decoder). Its first, second and third bytes are lead[0]
// to lead[2].
CharsetResult gb18030Handler(PrCharsetDecoder* d, CharsetQueue* queue, int32_t byte, CharsetWriter* w) {
    uint8_t& first = d->lead[0];
    uint8_t& second = d->lead[1];
    uint8_t& third = d->lead[2];
    if (byte == kEndOfQueue) {
        if (first == 0 && second == 0 && third == 0)
            return CharsetResult_Finished;
        first = second = third = 0;
        return CharsetResult_Error;
    }
    if (third != 0) {
        if (!inRange(byte, 0x30, 0x39)) {
            queueRestore(queue, second);
            queueRestore(queue, third);
            queueRestore(queue, byte);
            first = second = third = 0;
            return CharsetResult_Error;
        }
        uint32_t pointer = (uint32_t(first) - 0x81) * (10 * 126 * 10) + (uint32_t(second) - 0x30) * (10 * 126) +
            (uint32_t(third) - 0x81) * 10 + uint32_t(byte) - 0x30;
        first = second = third = 0;
        uint32_t codePoint = gb18030RangesCodePoint(pointer);
        if (codePoint == 0)
            return CharsetResult_Error;
        emit(w, codePoint);
        return CharsetResult_Continue;
    }
    if (second != 0) {
        if (inRange(byte, 0x81, 0xfe)) {
            third = uint8_t(byte);
            return CharsetResult_Continue;
        }
        queueRestore(queue, second);
        queueRestore(queue, byte);
        first = second = 0;
        return CharsetResult_Error;
    }
    if (first != 0) {
        if (inRange(byte, 0x30, 0x39)) {
            second = uint8_t(byte);
            return CharsetResult_Continue;
        }
        uint32_t lead = first;
        first = 0;
        uint32_t offset = byte < 0x7f ? 0x40 : 0x41;
        uint32_t codePoint = 0;
        if (inRange(byte, 0x40, 0x7e) || inRange(byte, 0x80, 0xfe))
            codePoint = kCharsetIndexGb18030[(lead - 0x81) * 190 + (uint32_t(byte) - offset)];
        if (codePoint != 0) {
            emit(w, codePoint);
            return CharsetResult_Continue;
        }
        if (isAscii(byte))
            queueRestore(queue, byte);
        return CharsetResult_Error;
    }
    if (isAscii(byte)) {
        emit(w, uint32_t(byte));
        return CharsetResult_Continue;
    }
    if (byte == 0x80) {
        emit(w, 0x20ac);
        return CharsetResult_Continue;
    }
    if (inRange(byte, 0x81, 0xfe)) {
        first = uint8_t(byte);
        return CharsetResult_Continue;
    }
    return CharsetResult_Error;
}

// Returns the code point of `pointer` in index Big5, or zero for none.
uint32_t big5CodePoint(uint32_t pointer) {
    if (pointer >= countOf(kCharsetIndexBig5))
        return 0;
    uint32_t low = kCharsetIndexBig5[pointer];
    return bitsetTest(kCharsetIndexBig5Plane2, pointer) ? 0x20000 | low : low;
}

CharsetResult big5Handler(PrCharsetDecoder* d, CharsetQueue* queue, int32_t byte, CharsetWriter* w) {
    uint8_t& lead = d->lead[0];
    if (byte == kEndOfQueue) {
        if (lead == 0)
            return CharsetResult_Finished;
        lead = 0;
        return CharsetResult_Error;
    }
    if (lead != 0) {
        uint32_t l = lead;
        lead = 0;
        uint32_t offset = byte < 0x7f ? 0x40 : 0x62;
        uint32_t codePoint = 0;
        if (inRange(byte, 0x40, 0x7e) || inRange(byte, 0xa1, 0xfe)) {
            uint32_t pointer = (l - 0x81) * 157 + (uint32_t(byte) - offset);
            // The pointers that decode to two code points.
            switch (pointer) {
                case 1133:
                    emit(w, 0x00ca);
                    emit(w, 0x0304);
                    return CharsetResult_Continue;
                case 1135:
                    emit(w, 0x00ca);
                    emit(w, 0x030c);
                    return CharsetResult_Continue;
                case 1164:
                    emit(w, 0x00ea);
                    emit(w, 0x0304);
                    return CharsetResult_Continue;
                case 1166:
                    emit(w, 0x00ea);
                    emit(w, 0x030c);
                    return CharsetResult_Continue;
                default:
                    break;
            }
            codePoint = big5CodePoint(pointer);
        }
        if (codePoint != 0) {
            emit(w, codePoint);
            return CharsetResult_Continue;
        }
        if (isAscii(byte))
            queueRestore(queue, byte);
        return CharsetResult_Error;
    }
    if (isAscii(byte)) {
        emit(w, uint32_t(byte));
        return CharsetResult_Continue;
    }
    if (inRange(byte, 0x81, 0xfe)) {
        lead = uint8_t(byte);
        return CharsetResult_Continue;
    }
    return CharsetResult_Error;
}

// The decoder of EUC-JP. lead[1] is set while the lead byte is one of JIS X 0212.
CharsetResult eucJpHandler(PrCharsetDecoder* d, CharsetQueue* queue, int32_t byte, CharsetWriter* w) {
    uint8_t& lead = d->lead[0];
    uint8_t& jis0212 = d->lead[1];
    if (byte == kEndOfQueue) {
        if (lead == 0)
            return CharsetResult_Finished;
        lead = 0;
        return CharsetResult_Error;
    }
    if (lead == 0x8e && inRange(byte, 0xa1, 0xdf)) {
        lead = 0;
        emit(w, 0xff61 - 0xa1 + uint32_t(byte));
        return CharsetResult_Continue;
    }
    if (lead == 0x8f && inRange(byte, 0xa1, 0xfe)) {
        jis0212 = 1;
        lead = uint8_t(byte);
        return CharsetResult_Continue;
    }
    if (lead != 0) {
        uint32_t l = lead;
        lead = 0;
        uint32_t codePoint = 0;
        if (inRange(int32_t(l), 0xa1, 0xfe) && inRange(byte, 0xa1, 0xfe)) {
            uint32_t pointer = (l - 0xa1) * 94 + uint32_t(byte) - 0xa1;
            codePoint = jis0212 != 0 ? kCharsetIndexJis0212[pointer] : kCharsetIndexJis0208[pointer];
        }
        jis0212 = 0;
        if (codePoint != 0) {
            emit(w, codePoint);
            return CharsetResult_Continue;
        }
        if (isAscii(byte))
            queueRestore(queue, byte);
        return CharsetResult_Error;
    }
    if (isAscii(byte)) {
        emit(w, uint32_t(byte));
        return CharsetResult_Continue;
    }
    if (byte == 0x8e || byte == 0x8f || inRange(byte, 0xa1, 0xfe)) {
        lead = uint8_t(byte);
        return CharsetResult_Continue;
    }
    return CharsetResult_Error;
}

// The decoder of ISO-2022-JP. Its decoder state is `state`, its output state `outputState`, and its lead byte lead[0].
CharsetResult iso2022JpHandler(PrCharsetDecoder* d, CharsetQueue* queue, int32_t byte, CharsetWriter* w) {
    uint8_t& lead = d->lead[0];
    switch (d->state) {
        case Iso2022JpState_Ascii:
        case Iso2022JpState_Roman:
        case Iso2022JpState_Katakana:
        case Iso2022JpState_LeadByte:
            if (byte == 0x1b) {
                d->state = Iso2022JpState_EscapeStart;
                return CharsetResult_Continue;
            }
            if (byte == kEndOfQueue)
                return CharsetResult_Finished;
            d->outputFlag = false;
            switch (d->state) {
                case Iso2022JpState_Ascii:
                    if (isAscii(byte) && byte != 0x0e && byte != 0x0f) {
                        emit(w, uint32_t(byte));
                        return CharsetResult_Continue;
                    }
                    return CharsetResult_Error;
                case Iso2022JpState_Roman:
                    if (byte == 0x5c) {
                        emit(w, 0x00a5);
                        return CharsetResult_Continue;
                    }
                    if (byte == 0x7e) {
                        emit(w, 0x203e);
                        return CharsetResult_Continue;
                    }
                    if (isAscii(byte) && byte != 0x0e && byte != 0x0f) {
                        emit(w, uint32_t(byte));
                        return CharsetResult_Continue;
                    }
                    return CharsetResult_Error;
                case Iso2022JpState_Katakana:
                    if (inRange(byte, 0x21, 0x5f)) {
                        emit(w, 0xff61 - 0x21 + uint32_t(byte));
                        return CharsetResult_Continue;
                    }
                    return CharsetResult_Error;
                default:
                    if (inRange(byte, 0x21, 0x7e)) {
                        lead = uint8_t(byte);
                        d->state = Iso2022JpState_TrailByte;
                        return CharsetResult_Continue;
                    }
                    return CharsetResult_Error;
            }
        case Iso2022JpState_TrailByte:
            if (byte == 0x1b) {
                d->state = Iso2022JpState_EscapeStart;
                return CharsetResult_Error;
            }
            d->state = Iso2022JpState_LeadByte;
            if (inRange(byte, 0x21, 0x7e)) {
                uint32_t codePoint = kCharsetIndexJis0208[(uint32_t(lead) - 0x21) * 94 + uint32_t(byte) - 0x21];
                if (codePoint == 0)
                    return CharsetResult_Error;
                emit(w, codePoint);
                return CharsetResult_Continue;
            }
            return CharsetResult_Error;
        case Iso2022JpState_EscapeStart:
            if (byte == 0x24 || byte == 0x28) {
                lead = uint8_t(byte);
                d->state = Iso2022JpState_Escape;
                return CharsetResult_Continue;
            }
            queueRestore(queue, byte);
            d->outputFlag = false;
            d->state = d->outputState;
            return CharsetResult_Error;
        default: {
            uint32_t l = lead;
            lead = 0;
            Iso2022JpState state = 0xff;
            if (l == 0x28 && byte == 0x42)
                state = Iso2022JpState_Ascii;
            else if (l == 0x28 && byte == 0x4a)
                state = Iso2022JpState_Roman;
            else if (l == 0x28 && byte == 0x49)
                state = Iso2022JpState_Katakana;
            else if (l == 0x24 && (byte == 0x40 || byte == 0x42))
                state = Iso2022JpState_LeadByte;
            if (state != 0xff) {
                d->state = state;
                d->outputState = state;
                bool output = d->outputFlag;
                d->outputFlag = true;
                return output ? CharsetResult_Error : CharsetResult_Continue;
            }
            queueRestore(queue, int32_t(l));
            queueRestore(queue, byte);
            d->outputFlag = false;
            d->state = d->outputState;
            return CharsetResult_Error;
        }
    }
}

CharsetResult shiftJisHandler(PrCharsetDecoder* d, CharsetQueue* queue, int32_t byte, CharsetWriter* w) {
    uint8_t& lead = d->lead[0];
    if (byte == kEndOfQueue) {
        if (lead == 0)
            return CharsetResult_Finished;
        lead = 0;
        return CharsetResult_Error;
    }
    if (lead != 0) {
        uint32_t l = lead;
        lead = 0;
        uint32_t offset = byte < 0x7f ? 0x40 : 0x41;
        uint32_t leadOffset = l < 0xa0 ? 0x81 : 0xc1;
        uint32_t codePoint = 0;
        if (inRange(byte, 0x40, 0x7e) || inRange(byte, 0x80, 0xfc)) {
            uint32_t pointer = (l - leadOffset) * 188 + uint32_t(byte) - offset;
            // The pointers of the user-defined area.
            if (pointer >= 8836 && pointer <= 10715) {
                emit(w, 0xe000 - 8836 + pointer);
                return CharsetResult_Continue;
            }
            if (pointer < countOf(kCharsetIndexJis0208))
                codePoint = kCharsetIndexJis0208[pointer];
        }
        if (codePoint != 0) {
            emit(w, codePoint);
            return CharsetResult_Continue;
        }
        if (isAscii(byte))
            queueRestore(queue, byte);
        return CharsetResult_Error;
    }
    if (isAscii(byte) || byte == 0x80) {
        emit(w, uint32_t(byte));
        return CharsetResult_Continue;
    }
    if (inRange(byte, 0xa1, 0xdf)) {
        emit(w, 0xff61 - 0xa1 + uint32_t(byte));
        return CharsetResult_Continue;
    }
    if (inRange(byte, 0x81, 0x9f) || inRange(byte, 0xe0, 0xfc)) {
        lead = uint8_t(byte);
        return CharsetResult_Continue;
    }
    return CharsetResult_Error;
}

CharsetResult eucKrHandler(PrCharsetDecoder* d, CharsetQueue* queue, int32_t byte, CharsetWriter* w) {
    uint8_t& lead = d->lead[0];
    if (byte == kEndOfQueue) {
        if (lead == 0)
            return CharsetResult_Finished;
        lead = 0;
        return CharsetResult_Error;
    }
    if (lead != 0) {
        uint32_t l = lead;
        lead = 0;
        uint32_t codePoint = 0;
        if (inRange(byte, 0x41, 0xfe))
            codePoint = kCharsetIndexEucKr[(l - 0x81) * 190 + (uint32_t(byte) - 0x41)];
        if (codePoint != 0) {
            emit(w, codePoint);
            return CharsetResult_Continue;
        }
        if (isAscii(byte))
            queueRestore(queue, byte);
        return CharsetResult_Error;
    }
    if (isAscii(byte)) {
        emit(w, uint32_t(byte));
        return CharsetResult_Continue;
    }
    if (inRange(byte, 0x81, 0xfe)) {
        lead = uint8_t(byte);
        return CharsetResult_Continue;
    }
    return CharsetResult_Error;
}

// The decoder of replacement, whose error returned flag is `outputFlag`.
CharsetResult replacementHandler(PrCharsetDecoder* d, int32_t byte) {
    if (byte == kEndOfQueue || d->outputFlag)
        return CharsetResult_Finished;
    d->outputFlag = true;
    return CharsetResult_Error;
}

// The shared UTF-16 decoder (encoding/#shared-utf-16-decoder). Its lead byte is lead[0] while `needed` is set, and its
// lead surrogate is `surrogate`, or zero for none.
CharsetResult utf16Handler(PrCharsetDecoder* d, CharsetQueue* queue, int32_t byte, CharsetWriter* w) {
    if (byte == kEndOfQueue) {
        if (d->needed == 0 && d->surrogate == 0)
            return CharsetResult_Finished;
        d->needed = 0;
        d->surrogate = 0;
        return CharsetResult_Error;
    }
    if (d->needed == 0) {
        d->lead[0] = uint8_t(byte);
        d->needed = 1;
        return CharsetResult_Continue;
    }
    d->needed = 0;
    bool big = d->charset == PrCharset_Utf16Be;
    uint32_t unit = big ? (uint32_t(d->lead[0]) << 8) | uint32_t(byte) : (uint32_t(byte) << 8) | d->lead[0];
    if (d->surrogate != 0) {
        uint32_t lead = d->surrogate;
        d->surrogate = 0;
        if (unit >= 0xdc00 && unit <= 0xdfff) {
            emit(w, 0x10000 + ((lead - 0xd800) << 10) + (unit - 0xdc00));
            return CharsetResult_Continue;
        }
        queueRestore(queue, big ? int32_t(unit >> 8) : int32_t(unit & 0xff));
        queueRestore(queue, big ? int32_t(unit & 0xff) : int32_t(unit >> 8));
        return CharsetResult_Error;
    }
    if (unit >= 0xd800 && unit <= 0xdbff) {
        d->surrogate = uint16_t(unit);
        return CharsetResult_Continue;
    }
    if (unit >= 0xdc00 && unit <= 0xdfff)
        return CharsetResult_Error;
    emit(w, unit);
    return CharsetResult_Continue;
}

bool isSingleByte(PrCharset charset) {
    return charset >= kCharsetSingleByteFirst && charset <= kCharsetSingleByteLast;
}

// Returns true if the decoder holds no part of a sequence, so that the end of the input is no error.
bool holdsNothing(const PrCharsetDecoder* d) {
    switch (d->charset) {
        case PrCharset_Utf8:
            return d->needed == 0;
        case PrCharset_Gbk:
        case PrCharset_Gb18030:
        case PrCharset_Big5:
        case PrCharset_EucJp:
        case PrCharset_ShiftJis:
        case PrCharset_EucKr:
            return d->lead[0] == 0 && d->lead[1] == 0 && d->lead[2] == 0;
        case PrCharset_Iso2022Jp:
            return d->state <= Iso2022JpState_LeadByte;
        case PrCharset_Utf16Be:
        case PrCharset_Utf16Le:
            return d->needed == 0 && d->surrogate == 0;
        default:
            return true;
    }
}

// Returns true if a run of bytes can decode to itself in the charset: well-formed UTF-8 in UTF-8, and ASCII in the
// other charsets whose every byte below 0x80 decodes to itself.
bool copiesRuns(PrCharset charset) {
    switch (charset) {
        case PrCharset_Iso2022Jp:
        case PrCharset_Replacement:
        case PrCharset_Utf16Be:
        case PrCharset_Utf16Le:
            return false;
        default:
            return true;
    }
}

// Returns the length of the run of bytes at the start of `bytes` that decode to themselves when the decoder holds
// nothing. The charset copies runs.
size_t runLength(PrCharset charset, const uint8_t* bytes, size_t size) {
    return charset == PrCharset_Utf8 ? utf8Run(bytes, size) : asciiRun(bytes, size);
}

// Decodes the queue of a single-byte charset into `w`. Returns false at the first error of a fatal decoder.
bool runSingleByte(PrCharsetDecoder* d, CharsetQueue* queue, CharsetWriter* w) {
    const uint32_t* table = kCharsetSingleByte[d->charset - kCharsetSingleByteFirst];
    bool fatal = (d->flags & PrCharsetDecoderFlag_Fatal) != 0;
    const uint8_t* bytes = queue->bytes;
    size_t at = queue->at;
    while (at < queue->size) {
        // A block of sixteen ASCII bytes is copied whole. Any other block is read a byte at a time, with ASCII packed
        // as the table packs the rest, so that no branch depends on which a byte is.
        if (queue->size - at >= 16) {
            uint64_t words = loadUnaligned<uint64_t>(bytes + at) | loadUnaligned<uint64_t>(bytes + at + 8);
            if ((words & 0x8080808080808080ull) == 0) {
                memCopy(w->out + w->size, bytes + at, 16);
                w->size += 16;
                at += 16;
                continue;
            }
        }
        size_t end = queue->size - at >= 16 ? at + 16 : queue->size;
        for (; at < end; ++at) {
            uint32_t byte = bytes[at];
            uint32_t packed = byte < 0x80 ? byte | 0x01000000 : table[byte - 0x80];
            if (packed == 0) {
                if (fatal) {
                    queue->at = at;
                    return false;
                }
                packed = 0x03bdbfef;   // U+FFFD
            }
            char* out = w->out + w->size;
            out[0] = char(packed);
            out[1] = char(packed >> 8);
            out[2] = char(packed >> 16);
            w->size += packed >> 24;
        }
    }
    queue->at = at;
    return true;
}

// Decodes the queue into `w`. Returns false at the first error of a fatal decoder.
bool run(PrCharsetDecoder* d, CharsetQueue* queue, CharsetWriter* w) {
    if (isSingleByte(d->charset))
        return runSingleByte(d, queue, w);
    bool fatal = (d->flags & PrCharsetDecoderFlag_Fatal) != 0;
    bool copies = copiesRuns(d->charset);
    for (;;) {
        if (copies && queueFrontIsEmpty(queue) && queue->at < queue->size && holdsNothing(d)) {
            size_t length = runLength(d->charset, queue->bytes + queue->at, queue->size - queue->at);
            emitBytes(w, queue->bytes + queue->at, length);
            queue->at += length;
        }
        int32_t byte = queueRead(queue);
        if (byte == kNoByte)
            return true;
        CharsetResult result;
        switch (d->charset) {
            case PrCharset_Utf8:
                result = utf8Handler(d, queue, byte, w);
                break;
            case PrCharset_Gbk:
            case PrCharset_Gb18030:
                result = gb18030Handler(d, queue, byte, w);
                break;
            case PrCharset_Big5:
                result = big5Handler(d, queue, byte, w);
                break;
            case PrCharset_EucJp:
                result = eucJpHandler(d, queue, byte, w);
                break;
            case PrCharset_Iso2022Jp:
                result = iso2022JpHandler(d, queue, byte, w);
                break;
            case PrCharset_ShiftJis:
                result = shiftJisHandler(d, queue, byte, w);
                break;
            case PrCharset_EucKr:
                result = eucKrHandler(d, queue, byte, w);
                break;
            case PrCharset_Replacement:
                result = replacementHandler(d, byte);
                break;
            case PrCharset_Utf16Be:
            case PrCharset_Utf16Le:
                result = utf16Handler(d, queue, byte, w);
                break;
            default:
                result = xUserDefinedHandler(byte, w);
                break;
        }
        if (result == CharsetResult_Finished)
            return true;
        if (result == CharsetResult_Error) {
            if (fatal)
                return false;
            emit(w, kPrUtf8Replacement);
        }
    }
}

// Returns true if `bytes` decode to themselves, and sets `*text` to them without the byte order mark that the decoder
// removes. The decoder is then past them, as if it had decoded them.
bool decodesToItself(PrCharsetDecoder* d, PrStringView bytes, PrStringView* text) {
    if (!holdsNothing(d))
        return false;
    *text = bytes.data != nullptr ? bytes : PrStringView{""};
    if (bytes.size == 0)
        return true;
    if (!copiesRuns(d->charset) && d->charset != PrCharset_Iso2022Jp)
        return false;
    const uint8_t* data = reinterpret_cast<const uint8_t*>(bytes.data);
    if (d->charset == PrCharset_Iso2022Jp) {
        if (d->state != Iso2022JpState_Ascii)
            return false;
        for (size_t i = 0; i < bytes.size; ++i) {
            if (data[i] >= 0x80 || data[i] == 0x0e || data[i] == 0x0f || data[i] == 0x1b)
                return false;
        }
        d->outputFlag = false;
        return true;
    }
    if (runLength(d->charset, data, bytes.size) != bytes.size)
        return false;
    if (!d->bomSeen && removesBom(d) && stringStartsWith(bytes, "\xEF\xBB\xBF"))
        *text = stringSlice(bytes, 3, bytes.size - 3);
    d->bomSeen = true;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Encoders

// The states of the ISO-2022-JP encoder (encoding/#iso-2022-jp-encoder).
enum {
    Iso2022JpEncoderState_Ascii,
    Iso2022JpEncoderState_Roman,
    Iso2022JpEncoderState_Jis0208,
};
typedef uint8_t Iso2022JpEncoderState;

// A pointer that encodes nothing.
constexpr uint32_t kNoPointer = 0xffff;

// Returns true if an encoder's table holds `codePoint`, and writes the pointer beside it.
template <typename CodePoint, size_t N>
bool findPointer(const CodePoint (&codePoints)[N], const uint16_t (&pointers)[N], uint32_t codePoint, uint32_t* pointer) {
    uint32_t low = lowerBound(codePoints, uint32_t(N), codePoint);
    if (low == N || codePoints[low] != codePoint)
        return false;
    *pointer = pointers[low];
    return true;
}

// Returns the pointer beside `codePoint` in an encoder's table, or kNoPointer if the table does not hold it.
template <typename CodePoint, size_t N>
uint32_t pointerOf(const CodePoint (&codePoints)[N], const uint16_t (&pointers)[N], uint32_t codePoint) {
    uint32_t pointer = kNoPointer;
    return findPointer(codePoints, pointers, codePoint, &pointer) ? pointer : kNoPointer;
}

// Returns the pointer of `codePoint` in index gb18030 ranges (encoding/#index-gb18030-ranges-pointer).
uint32_t gb18030RangesPointer(uint32_t codePoint) {
    if (codePoint == 0xe7c7)
        return 7457;
    // The last range from the second on that starts at or before `codePoint`, or the first.
    uint32_t low = partitionPoint(1, uint32_t(countOf(kCharsetGb18030Ranges)),
        [&](uint32_t i) { return kCharsetGb18030Ranges[i].codePoint <= codePoint; }) - 1;
    const CharsetGb18030Range& range = kCharsetGb18030Ranges[low];
    return range.pointer + codePoint - range.codePoint;
}

// The bytes of a encode, written two at a time where a charset needs them.
struct CharsetBytes {
    char* out;
    size_t size;

    void put(uint32_t byte) { out[size++] = char(byte); }
    void put(uint32_t first, uint32_t second) {
        put(first);
        put(second);
    }
};

// The private use code points that gb18030 encodes as GB18030-2005 did (encoding/#gb18030-encoder), and their bytes.
struct Gb18030Private {
    uint16_t codePoint;
    uint8_t first;
    uint8_t second;
};

constexpr Gb18030Private kGb18030Private[] = {
    {0xe78d, 0xa6, 0xd9}, {0xe78e, 0xa6, 0xda}, {0xe78f, 0xa6, 0xdb}, {0xe790, 0xa6, 0xdc}, {0xe791, 0xa6, 0xdd},
    {0xe792, 0xa6, 0xde}, {0xe793, 0xa6, 0xdf}, {0xe794, 0xa6, 0xec}, {0xe795, 0xa6, 0xed}, {0xe796, 0xa6, 0xf3},
    {0xe81e, 0xfe, 0x59}, {0xe826, 0xfe, 0x61}, {0xe82b, 0xfe, 0x66}, {0xe82c, 0xfe, 0x67}, {0xe832, 0xfe, 0x6d},
    {0xe843, 0xfe, 0x7e}, {0xe854, 0xfe, 0x90}, {0xe864, 0xfe, 0xa0},
};

// Encodes one code point that is not ASCII, or ISO-2022-JP's every code point, in the encoder's charset. Returns false
// if the charset cannot encode it, and sets `*error` to the code point that the error reports.
bool encodeOne(PrCharsetEncoder* e, uint32_t codePoint, CharsetBytes* b, uint32_t* error) {
    *error = codePoint;
    switch (e->charset) {
        case PrCharset_Gbk:
        case PrCharset_Gb18030: {
            bool gbk = e->charset == PrCharset_Gbk;
            if (codePoint == 0xe5e5)
                return false;
            if (gbk && codePoint == 0x20ac) {
                b->put(0x80);
                return true;
            }
            for (const Gb18030Private& entry : kGb18030Private) {
                if (entry.codePoint == codePoint) {
                    b->put(entry.first, entry.second);
                    return true;
                }
            }
            uint32_t pointer = pointerOf(kCharsetEncodeGb18030CodePoints, kCharsetEncodeGb18030Pointers, codePoint);
            if (pointer != kNoPointer) {
                uint32_t trailing = pointer % 190;
                b->put(pointer / 190 + 0x81, trailing + (trailing < 0x3f ? 0x40 : 0x41));
                return true;
            }
            if (gbk)
                return false;
            pointer = gb18030RangesPointer(codePoint);
            b->put(pointer / (10 * 126 * 10) + 0x81, pointer % (10 * 126 * 10) / (10 * 126) + 0x30);
            b->put(pointer % (10 * 126) / 10 + 0x81, pointer % 10 + 0x30);
            return true;
        }
        case PrCharset_Big5: {
            uint32_t pointer = pointerOf(kCharsetEncodeBig5CodePoints, kCharsetEncodeBig5Pointers, codePoint);
            if (pointer == kNoPointer)
                return false;
            uint32_t trailing = pointer % 157;
            b->put(pointer / 157 + 0x81, trailing + (trailing < 0x3f ? 0x40 : 0x62));
            return true;
        }
        case PrCharset_EucJp: {
            if (codePoint == 0xa5) {
                b->put(0x5c);
                return true;
            }
            if (codePoint == 0x203e) {
                b->put(0x7e);
                return true;
            }
            if (codePoint >= 0xff61 && codePoint <= 0xff9f) {
                b->put(0x8e, codePoint - 0xff61 + 0xa1);
                return true;
            }
            if (codePoint == 0x2212)
                codePoint = 0xff0d;
            uint32_t pointer = pointerOf(kCharsetEncodeJis0208CodePoints, kCharsetEncodeJis0208Pointers, codePoint);
            if (pointer == kNoPointer)
                return false;
            b->put(pointer / 94 + 0xa1, pointer % 94 + 0xa1);
            return true;
        }
        case PrCharset_ShiftJis: {
            if (codePoint == 0x80) {
                b->put(0x80);
                return true;
            }
            if (codePoint == 0xa5) {
                b->put(0x5c);
                return true;
            }
            if (codePoint == 0x203e) {
                b->put(0x7e);
                return true;
            }
            if (codePoint >= 0xff61 && codePoint <= 0xff9f) {
                b->put(codePoint - 0xff61 + 0xa1);
                return true;
            }
            if (codePoint == 0x2212)
                codePoint = 0xff0d;
            // encoding/#index-shift_jis-pointer: jis0208's pointer, unless it is one that Shift_JIS skips.
            uint32_t pointer = kNoPointer;
            if (!findPointer(kCharsetEncodeShiftJisCodePoints, kCharsetEncodeShiftJisPointers, codePoint, &pointer))
                pointer = pointerOf(kCharsetEncodeJis0208CodePoints, kCharsetEncodeJis0208Pointers, codePoint);
            if (pointer == kNoPointer)
                return false;
            uint32_t leading = pointer / 188;
            uint32_t trailing = pointer % 188;
            b->put(leading + (leading < 0x1f ? 0x81 : 0xc1), trailing + (trailing < 0x3f ? 0x40 : 0x41));
            return true;
        }
        case PrCharset_EucKr: {
            uint32_t pointer = pointerOf(kCharsetEncodeEucKrCodePoints, kCharsetEncodeEucKrPointers, codePoint);
            if (pointer == kNoPointer)
                return false;
            b->put(pointer / 190 + 0x81, pointer % 190 + 0x41);
            return true;
        }
        case PrCharset_XUserDefined:
            if (codePoint < 0xf780 || codePoint > 0xf7ff)
                return false;
            b->put(codePoint - 0xf780 + 0x80);
            return true;
        case PrCharset_Iso2022Jp:
            // The handler restores the code point to the queue after each escape, so it runs again in the new state.
            for (;;) {
                Iso2022JpEncoderState state = e->state;
                bool ascii = codePoint < 0x80;
                if (state != Iso2022JpEncoderState_Jis0208 && (codePoint == 0x0e || codePoint == 0x0f || codePoint == 0x1b)) {
                    *error = kPrUtf8Replacement;
                    return false;
                }
                if (state == Iso2022JpEncoderState_Ascii && ascii) {
                    b->put(codePoint);
                    return true;
                }
                if (state == Iso2022JpEncoderState_Roman &&
                        ((ascii && codePoint != 0x5c && codePoint != 0x7e) || codePoint == 0xa5 || codePoint == 0x203e)) {
                    b->put(codePoint == 0xa5 ? 0x5c : codePoint == 0x203e ? 0x7e : codePoint);
                    return true;
                }
                if (ascii) {
                    b->put(0x1b, 0x28);
                    b->put(0x42);
                    e->state = Iso2022JpEncoderState_Ascii;
                    continue;
                }
                if (codePoint == 0xa5 || codePoint == 0x203e) {
                    b->put(0x1b, 0x28);
                    b->put(0x4a);
                    e->state = Iso2022JpEncoderState_Roman;
                    continue;
                }
                if (codePoint == 0x2212)
                    codePoint = 0xff0d;
                if (codePoint >= 0xff61 && codePoint <= 0xff9f)
                    codePoint = kCharsetIndexIso2022JpKatakana[codePoint - 0xff61];
                *error = codePoint;
                uint32_t pointer = pointerOf(kCharsetEncodeJis0208CodePoints, kCharsetEncodeJis0208Pointers, codePoint);
                if (pointer == kNoPointer) {
                    if (state != Iso2022JpEncoderState_Jis0208)
                        return false;
                    b->put(0x1b, 0x28);
                    b->put(0x42);
                    e->state = Iso2022JpEncoderState_Ascii;
                    continue;
                }
                if (state != Iso2022JpEncoderState_Jis0208) {
                    b->put(0x1b, 0x24);
                    b->put(0x42);
                    e->state = Iso2022JpEncoderState_Jis0208;
                    continue;
                }
                b->put(pointer / 94 + 0x21, pointer % 94 + 0x21);
                return true;
            }
        default: {
            // A single-byte charset, whose table holds each byte's code point as UTF-8.
            char utf8[kPrUtf8MaxBytes];
            uint32_t width = prUtf8Encode(codePoint, utf8);
            if (width > 3)
                return false;
            uint32_t packed = width << 24;
            for (uint32_t i = 0; i < width; ++i)
                packed |= uint32_t(uint8_t(utf8[i])) << (8 * i);
            const uint32_t* table = kCharsetSingleByte[e->charset - kCharsetSingleByteFirst];
            for (uint32_t byte = 0; byte < 128; ++byte) {
                if (table[byte] == packed) {
                    b->put(byte + 0x80);
                    return true;
                }
            }
            return false;
        }
    }
}

} // namespace
} // namespace pr_impl

using namespace pr_impl;

PrCharset prCharsetFromLabel(PrStringView label) {
    return charsetOfLabel(stringTrim(label));
}

PrCharset prCharsetFromMimeType(PrStringView type) {
    PrString label = prStringNull();
    if (!prMimeTypeParameter(type, "charset", &label))
        return PrCharset_None;
    PrCharset charset = prCharsetFromLabel(prStringView(&label));
    prStringReset(&label);
    return charset;
}

PrStringView prCharsetName(PrCharset charset) {
    PR_ASSERT(charset < PrCharset_Count);
    if (charset == PrCharset_None)
        return PrStringView{nullptr, 0};
    return charsetName(charset);
}

PrCharset prCharsetOutput(PrCharset charset) {
    switch (charset) {
        case PrCharset_Replacement:
        case PrCharset_Utf16Be:
        case PrCharset_Utf16Le:
            return PrCharset_Utf8;
        default:
            return charset;
    }
}

PrCharset prCharsetSniffBom(PrStringView bytes) {
    if (stringStartsWith(bytes, "\xEF\xBB\xBF"))
        return PrCharset_Utf8;
    if (stringStartsWith(bytes, "\xFE\xFF"))
        return PrCharset_Utf16Be;
    if (stringStartsWith(bytes, "\xFF\xFE"))
        return PrCharset_Utf16Le;
    return PrCharset_None;
}

bool prCharsetIsBomDecided(PrStringView bytes) {
    if (bytes.size >= 3)
        return true;
    if (bytes.size == 0)
        return false;
    uint8_t first = uint8_t(bytes.data[0]);
    if (first == 0xfe || first == 0xff)
        return bytes.size == 2;
    return first != 0xef;
}

PrCharsetDecoder prCharsetDecoderMake(PrCharset charset, PrCharsetDecoderFlags flags) {
    PR_ASSERT(charset != PrCharset_None && charset < PrCharset_Count);
    PrCharsetDecoder decoder = {};
    decoder.charset = charset;
    decoder.flags = flags;
    decoder.lower = 0x80;
    decoder.upper = 0xbf;
    return decoder;
}

size_t prCharsetDecodeMaxSize(const PrCharsetDecoder* decoder, size_t size) {
    (void)decoder;
    // Each byte writes at most three bytes of UTF-8, counting the bytes that the decoder holds from earlier parts. A
    // handler that writes a code point of four bytes reads at least two bytes for it.
    return 3 * (size + 4);
}

bool prCharsetDecode(PrCharsetDecoder* decoder, PrStringView bytes, PrCharsetInput input, char* out, size_t* outSize) {
    CharsetWriter w = {decoder, out, 0};
    CharsetQueue queue = {};
    queue.bytes = reinterpret_cast<const uint8_t*>(bytes.data);
    queue.size = bytes.size;
    queue.final = input == PrCharsetInput_Final;
    bool ok = run(decoder, &queue, &w);
    *outSize = w.size;
    if (input == PrCharsetInput_Final)
        *decoder = prCharsetDecoderMake(decoder->charset, decoder->flags);
    return ok;
}

PrStringView prCharsetDecodeView(PrCharsetDecoder* decoder, PrStringView bytes, PrCharsetInput input, PrString* out) {
    PrStringView same;
    if (decodesToItself(decoder, bytes, &same)) {
        if (input == PrCharsetInput_Final)
            *decoder = prCharsetDecoderMake(decoder->charset, decoder->flags);
        return same;
    }
    TextBuffer text;
    text.text.resize(uint32_t(prCharsetDecodeMaxSize(decoder, bytes.size)));
    size_t size = 0;
    if (!prCharsetDecode(decoder, bytes, input, text.text.data, &size)) {
        textReset(&text);
        return PrStringView{nullptr, 0};
    }
    text.text.count = uint32_t(size);
    size_t before = prStringView(out).size;
    if (before == 0) {
        prStringReset(out);
        *out = textTake(&text);
    } else {
        prStringAppend(out, textView(&text));
        textReset(&text);
    }
    PrStringView all = prStringView(out);
    return stringSlice(all, before, all.size - before);
}

PrCharsetEncoder prCharsetEncoderMake(PrCharset charset) {
    PR_ASSERT(charset != PrCharset_None && charset < PrCharset_Count && prCharsetOutput(charset) == charset);
    PrCharsetEncoder encoder = {};
    encoder.charset = charset;
    return encoder;
}

size_t prCharsetEncodeMaxSize(const PrCharsetEncoder* encoder, size_t size) {
    (void)encoder;
    // A code point takes at least one byte of UTF-8 and at most five bytes in any charset: ISO-2022-JP's escape and two
    // bytes of jis0208. A final part may close with one more escape.
    return 5 * size + 3;
}

size_t prCharsetEncode(PrCharsetEncoder* encoder, PrStringView text, PrCharsetInput input, char* out, size_t* outSize,
        uint32_t* outUnencoded) {
    *outUnencoded = kPrCharsetUnencodedNone;
    if (encoder->charset == PrCharset_Utf8) {
        memCopy(out, text.data, text.size);
        *outSize = text.size;
        return text.size;
    }
    CharsetBytes b = {out, 0};
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text.data);
    bool iso2022Jp = encoder->charset == PrCharset_Iso2022Jp;
    size_t at = 0;
    while (at < text.size) {
        // ASCII encodes as itself, except in ISO-2022-JP, whose state decides what a byte means.
        if (!iso2022Jp) {
            size_t length = asciiRun(bytes + at, text.size - at);
            memCopy(out + b.size, bytes + at, length);
            b.size += length;
            at += length;
            if (at == text.size)
                break;
        }
        uint32_t codePoint = 0;
        at += prUtf8Decode(text, at, &codePoint);
        uint32_t error = 0;
        if (!encodeOne(encoder, codePoint, &b, &error)) {
            *outUnencoded = error;
            *outSize = b.size;
            return at;
        }
    }
    if (input == PrCharsetInput_Final && encoder->state != Iso2022JpEncoderState_Ascii) {
        b.put(0x1b, 0x28);
        b.put(0x42);
        encoder->state = Iso2022JpEncoderState_Ascii;
    }
    *outSize = b.size;
    return at;
}
