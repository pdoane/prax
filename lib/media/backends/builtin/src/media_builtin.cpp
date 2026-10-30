// The built-in media codec: the metadata of a resource in the ISO base media file format (ISO/IEC 14496-12).

#include "pr/media_builtin.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/float.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/mime.h"

namespace pr_impl {

namespace {

struct BuiltinDecoder : PrMediaDecoder {};

uint64_t readU64(const uint8_t* bytes) {
    return uint64_t(readBeU32(bytes)) << 32 | readBeU32(bytes + 4);
}

// Passes each box of [data, end) to `visit`, with its type, its payload, and the payload's size.
template <typename Visit>
void eachBox(const uint8_t* data, const uint8_t* end, Visit visit) {
    while (end - data >= 8) {
        uint64_t size = readBeU32(data);
        uint32_t header = 8;
        if (size == 1 && end - data >= 16) {
            size = readU64(data + 8);
            header = 16;
        } else if (size == 0) {
            size = uint64_t(end - data);
        }
        if (size < header || size > uint64_t(end - data))
            return;
        visit(readBeU32(data + 4), data + header, size_t(size - header));
        data += size;
    }
}

constexpr uint32_t fourCc(const char (&code)[5]) {
    return uint32_t(uint8_t(code[0])) << 24 | uint32_t(uint8_t(code[1])) << 16 | uint32_t(uint8_t(code[2])) << 8 |
        uint32_t(uint8_t(code[3]));
}

// Reads the duration of a movie (its mvhd box) and the extent of its first video track (a trak whose handler is vide,
// with the extent that its tkhd box gives), and whether it has an audio track (a handler of soun). Returns false for a
// resource that is not a movie.
bool readMovie(PrByteView resource, PrMediaInfo* out) {
    const uint8_t* data = resource.data;
    const uint8_t* end = data + resource.size;
    bool typed = false;
    bool timed = false;
    eachBox(data, end, [&](uint32_t type, const uint8_t* payload, size_t size) {
        if (type == fourCc("ftyp"))
            typed = true;
        if (type != fourCc("moov"))
            return;
        eachBox(payload, payload + size, [&](uint32_t child, const uint8_t* body, size_t length) {
            if (child == fourCc("mvhd") && length >= 32) {
                bool wide = body[0] == 1;
                uint32_t scale = readBeU32(body + (wide ? 20 : 12));
                uint64_t units = wide ? readU64(body + 24) : readBeU32(body + 16);
                if (scale != 0) {
                    out->duration = double(units) / scale;
                    timed = true;
                }
            } else if (child == fourCc("trak")) {
                PrUVec2 extent{};
                uint32_t handler = 0;
                eachBox(body, body + length, [&](uint32_t part, const uint8_t* bytes, size_t count) {
                    if (part == fourCc("tkhd")) {
                        size_t at = bytes[0] == 1 ? 88 : 76;
                        if (count >= at + 8)
                            extent = PrUVec2{readBeU32(bytes + at) >> 16, readBeU32(bytes + at + 4) >> 16};
                    } else if (part == fourCc("mdia")) {
                        eachBox(bytes, bytes + count, [&](uint32_t inner, const uint8_t* named, size_t n) {
                            if (inner == fourCc("hdlr") && n >= 12)
                                handler = readBeU32(named + 8);
                        });
                    }
                });
                if (handler == fourCc("vide") && !out->hasVideo) {
                    out->hasVideo = true;
                    out->videoExtent = extent;
                }
                out->hasAudio |= handler == fourCc("soun");
            }
        });
    });
    return typed && timed;
}

PrMediaSupport supportsType(PrMediaCodec*, PrStringView type) {
    PrStringView essence = prMimeTypeEssence(type);
    if (asciiEqualsFold(essence, "video/mp4") || asciiEqualsFold(essence, "audio/mp4"))
        return PrMediaSupport_Maybe;
    return PrMediaSupport_No;
}

PrStatus decoderCreate(PrMediaCodec*, PrByteView bytes, PrMediaDecoder* PR_NULLABLE* PR_NONNULL out, PrMediaInfo* info) {
    info->duration = kNanF64;
    if (!readMovie(bytes, info))
        return PrStatus_NotSupported;
    *out = allocZeroed<BuiltinDecoder>();
    return PrStatus_Ok;
}

void decoderDestroy(PrMediaCodec*, PrMediaDecoder* decoder) {
    prMemoryFree(static_cast<BuiltinDecoder*>(decoder));
}

constexpr PrMediaCodec kBuiltin = {
    .supportsType = supportsType,
    .decoderCreate = decoderCreate,
    .decoderDestroy = decoderDestroy,
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrMediaCodec* prMediaBuiltinCreate(void) {
    // The codec holds no state, so every caller shares one table.
    return const_cast<PrMediaCodec*>(&kBuiltin);
}
