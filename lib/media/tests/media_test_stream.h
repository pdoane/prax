// A fragmented MP4 byte stream for the media tests: an initialization segment of an H.264 video track and an AAC audio
// track, and media segments of both.

#pragma once

#include "pr/array.h"
#include "pr/base.h"
#include "pr/mem.h"
#include "pr/span.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The bytes of a box or a stream.
using Bytes = Array<uint8_t>;

// Append a big-endian integer.
inline void put32(Bytes& out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push(uint8_t(value >> shift));
}
inline void put16(Bytes& out, uint16_t value) {
    out.push(uint8_t(value >> 8));
    out.push(uint8_t(value));
}

// A box of `type`, four letters, around its payload.
inline Bytes box(const char* type, const Bytes& payload) {
    Bytes out;
    put32(out, uint32_t(8 + payload.count()));
    out.append(reinterpret_cast<const uint8_t*>(type), 4);
    out.append(payload.span());
    return out;
}

// The parts one after another.
inline Bytes cat(Span<const Bytes> parts) {
    Bytes out;
    for (const Bytes& part : parts)
        out.append(part.span());
    return out;
}

// A full box's version and flags.
inline Bytes full(uint8_t version, uint32_t flags) {
    return Bytes({version, uint8_t(flags >> 16), uint8_t(flags >> 8), uint8_t(flags)});
}

// `count` zero bytes.
inline Bytes zeros(uint32_t count) {
    return Bytes(count);
}

// The stream's tracks: their IDs and timescales.
constexpr uint32_t kVideoTrack = 1;
constexpr uint32_t kAudioTrack = 2;
constexpr uint32_t kVideoTimescale = 90000;
constexpr uint32_t kAudioTimescale = 44100;

// A trak box of a track with `handler`, vide or soun, whose stsd holds `entry`.
inline Bytes track(uint32_t id, const char* handler, uint32_t timescale, const Bytes& entry) {
    Bytes tkhd = full(0, 3);
    Bytes fields = zeros(8);
    tkhd.append(fields.span());
    put32(tkhd, id);
    Bytes rest = zeros(72);
    tkhd.append(rest.span());
    const Bytes mdhdHead[] = {full(0, 0), zeros(8)};
    Bytes mdhd = cat(mdhdHead);
    put32(mdhd, timescale);
    put32(mdhd, 0);
    put16(mdhd, uint16_t(('e' - 0x60) << 10 | ('n' - 0x60) << 5 | ('g' - 0x60)));
    put16(mdhd, 0);
    const Bytes hdlrHead[] = {full(0, 0), zeros(4)};
    Bytes hdlr = cat(hdlrHead);
    hdlr.append(reinterpret_cast<const uint8_t*>(handler), 4);
    Bytes tail = zeros(13);
    hdlr.append(tail.span());
    Bytes stsd = full(0, 0);
    put32(stsd, 1);
    stsd.append(entry.span());
    Bytes stbl = box("stbl", box("stsd", stsd));
    const Bytes media[] = {box("mdhd", mdhd), box("hdlr", hdlr), box("minf", stbl)};
    const Bytes trak[] = {box("tkhd", tkhd), box("mdia", cat(media))};
    return box("trak", cat(trak));
}

// A sample entry of H.264 video of 320 by 240.
inline Bytes videoEntry() {
    Bytes fields = zeros(24);
    put16(fields, 320);
    put16(fields, 240);
    Bytes rest = zeros(50);
    fields.append(rest.span());
    Bytes avcc({1, 0x4d, 0x40, 0x1e, 0xff, 0xe0});
    fields.append(box("avcC", avcc).span());
    return box("avc1", fields);
}

// A sample entry of AAC audio at 44.1 kHz in two channels.
inline Bytes audioEntry() {
    Bytes fields = zeros(16);
    put16(fields, 2);
    put16(fields, 16);
    Bytes reserved = zeros(4);
    fields.append(reserved.span());
    put32(fields, kAudioTimescale << 16);
    // An ES descriptor holding a decoder config of AAC (0x40) and an AudioSpecificConfig of AAC LC (type 2).
    Bytes esds = full(0, 0);
    Bytes descriptor({3, 25, 0, 1, 0, 4, 17, 0x40, 0x15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 2, 0x12, 0x10, 6, 1, 2});
    esds.append(descriptor.span());
    fields.append(box("esds", esds).span());
    return box("mp4a", fields);
}

// The stream's initialization segment.
inline Bytes initSegment() {
    const Bytes mvhdHead[] = {full(0, 0), zeros(8)};
    Bytes mvhd = cat(mvhdHead);
    put32(mvhd, 1000);
    put32(mvhd, 0);
    Bytes rest = zeros(80);
    mvhd.append(rest.span());
    Bytes mvex;
    const uint32_t ids[] = {kVideoTrack, kAudioTrack};
    for (uint32_t id : ids) {
        Bytes trex = full(0, 0);
        put32(trex, id);
        put32(trex, 1);
        put32(trex, id == kVideoTrack ? 3000 : 1024);
        put32(trex, 0);
        put32(trex, id == kVideoTrack ? 0x10000 : 0);
        mvex.append(box("trex", trex).span());
    }
    Bytes ftyp({'i', 's', 'o', '5', 0, 0, 0, 1, 'i', 's', 'o', '5'});
    const Bytes movie[] = {
        box("mvhd", mvhd),
        track(kVideoTrack, "vide", kVideoTimescale, videoEntry()),
        track(kAudioTrack, "soun", kAudioTimescale, audioEntry()),
        box("mvex", mvex),
    };
    const Bytes stream[] = {box("ftyp", ftyp), box("moov", cat(movie))};
    return cat(stream);
}

// A media segment of `count` video samples of `size` bytes each, from decode time `start` in the video's timescale,
// whose first sample is a key frame and whose samples present one frame late, and of `audioCount` audio samples of 1024
// frames each from the same time.
inline Bytes mediaSegment(uint64_t start, uint32_t count, uint32_t size, uint32_t audioCount = 1) {
    Bytes videoTfhd = full(0, 0x20000);
    put32(videoTfhd, kVideoTrack);
    Bytes videoTfdt = full(1, 0);
    put32(videoTfdt, uint32_t(start >> 32));
    put32(videoTfdt, uint32_t(start));
    // A trun with a data offset, the first sample's flags, and each sample's size and composition offset.
    Bytes videoTrun = full(0, 0x1 | 0x4 | 0x200 | 0x800);
    put32(videoTrun, count);
    uint32_t dataOffsetAt = videoTrun.count();
    put32(videoTrun, 0);
    put32(videoTrun, 0);
    for (uint32_t i = 0; i < count; ++i) {
        put32(videoTrun, size);
        put32(videoTrun, 3000);
    }
    Bytes audioTfhd = full(0, 0x20000);
    put32(audioTfhd, kAudioTrack);
    Bytes audioTfdt = full(0, 0);
    put32(audioTfdt, uint32_t(start * kAudioTimescale / kVideoTimescale));
    Bytes audioTrun = full(0, 0x1 | 0x200);
    put32(audioTrun, audioCount);
    uint32_t audioOffsetAt = audioTrun.count();
    put32(audioTrun, 0);
    for (uint32_t i = 0; i < audioCount; ++i)
        put32(audioTrun, 7);
    Bytes mfhd = full(0, 0);
    put32(mfhd, 1);
    const Bytes videoTraf[] = {box("tfhd", videoTfhd), box("tfdt", videoTfdt), box("trun", videoTrun)};
    const Bytes audioTraf[] = {box("tfhd", audioTfhd), box("tfdt", audioTfdt), box("trun", audioTrun)};
    const Bytes fragment[] = {box("mfhd", mfhd), box("traf", cat(videoTraf)), box("traf", cat(audioTraf))};
    Bytes moof = box("moof", cat(fragment));
    // The data offsets are from the start of the moof to the samples in the mdat that follows.
    uint32_t videoData = uint32_t(moof.count() + 8);
    uint32_t audioData = videoData + count * size;
    auto patch = [&](const Bytes& run, uint32_t offsetAt, uint32_t value) {
        uint32_t at = 0;
        for (uint32_t i = 0; i + run.count() <= moof.count(); ++i) {
            if (prMemEquals(run.data(), moof.data() + i, run.count())) {
                at = i;
                break;
            }
        }
        for (int k = 0; k < 4; ++k)
            moof[at + offsetAt + uint32_t(k)] = uint8_t(value >> (24 - 8 * k));
    };
    patch(videoTrun, dataOffsetAt, videoData);
    patch(audioTrun, audioOffsetAt, audioData);
    Bytes data;
    for (uint32_t i = 0; i < count; ++i) {
        data.append(Bytes(size, uint8_t(i + 1)).span());
    }
    data.append(Bytes(7 * audioCount, uint8_t(0xaa)).span());
    moof.append(box("mdat", data).span());
    return moof;
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
