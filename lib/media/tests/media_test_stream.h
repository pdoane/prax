// A fragmented MP4 byte stream for the media tests: an initialization segment of an H.264 video track and an AAC audio
// track, and media segments of both.

#pragma once

#include "pr/base.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The bytes of a box or a stream.
using Bytes = std::vector<uint8_t>;

// Append a big-endian integer.
inline void put32(Bytes& out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(uint8_t(value >> shift));
}
inline void put16(Bytes& out, uint16_t value) {
    out.push_back(uint8_t(value >> 8));
    out.push_back(uint8_t(value));
}

// A box of `type`, four letters, around its payload.
inline Bytes box(const char* type, const Bytes& payload) {
    Bytes out;
    put32(out, uint32_t(8 + payload.size()));
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// The parts one after another.
inline Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes& part : parts)
        out.insert(out.end(), part.begin(), part.end());
    return out;
}

// A full box's version and flags.
inline Bytes full(uint8_t version, uint32_t flags) {
    return {version, uint8_t(flags >> 16), uint8_t(flags >> 8), uint8_t(flags)};
}

// `count` zero bytes.
inline Bytes zeros(size_t count) {
    return Bytes(count, 0);
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
    tkhd.insert(tkhd.end(), fields.begin(), fields.end());
    put32(tkhd, id);
    Bytes rest = zeros(72);
    tkhd.insert(tkhd.end(), rest.begin(), rest.end());
    Bytes mdhd = cat({full(0, 0), zeros(8)});
    put32(mdhd, timescale);
    put32(mdhd, 0);
    put16(mdhd, uint16_t(('e' - 0x60) << 10 | ('n' - 0x60) << 5 | ('g' - 0x60)));
    put16(mdhd, 0);
    Bytes hdlr = cat({full(0, 0), zeros(4)});
    hdlr.insert(hdlr.end(), handler, handler + 4);
    Bytes tail = zeros(13);
    hdlr.insert(hdlr.end(), tail.begin(), tail.end());
    Bytes stsd = full(0, 0);
    put32(stsd, 1);
    stsd = cat({stsd, entry});
    Bytes stbl = box("stbl", box("stsd", stsd));
    return box("trak", cat({
        box("tkhd", tkhd), box("mdia", cat({box("mdhd", mdhd), box("hdlr", hdlr), box("minf", stbl)})),
    }));
}

// A sample entry of H.264 video of 320 by 240.
inline Bytes videoEntry() {
    Bytes fields = zeros(24);
    put16(fields, 320);
    put16(fields, 240);
    Bytes rest = zeros(50);
    fields.insert(fields.end(), rest.begin(), rest.end());
    Bytes avcc = {1, 0x4d, 0x40, 0x1e, 0xff, 0xe0};
    return box("avc1", cat({fields, box("avcC", avcc)}));
}

// A sample entry of AAC audio at 44.1 kHz in two channels.
inline Bytes audioEntry() {
    Bytes fields = zeros(16);
    put16(fields, 2);
    put16(fields, 16);
    Bytes reserved = zeros(4);
    fields.insert(fields.end(), reserved.begin(), reserved.end());
    put32(fields, kAudioTimescale << 16);
    // An ES descriptor holding a decoder config of AAC (0x40) and an AudioSpecificConfig of AAC LC (type 2).
    Bytes esds = full(0, 0);
    Bytes descriptor = {3, 25, 0, 1, 0, 4, 17, 0x40, 0x15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 2, 0x12, 0x10, 6, 1, 2};
    esds.insert(esds.end(), descriptor.begin(), descriptor.end());
    return box("mp4a", cat({fields, box("esds", esds)}));
}

// The stream's initialization segment.
inline Bytes initSegment() {
    Bytes mvhd = cat({full(0, 0), zeros(8)});
    put32(mvhd, 1000);
    put32(mvhd, 0);
    Bytes rest = zeros(80);
    mvhd.insert(mvhd.end(), rest.begin(), rest.end());
    Bytes mvex;
    for (uint32_t id : {kVideoTrack, kAudioTrack}) {
        Bytes trex = full(0, 0);
        put32(trex, id);
        put32(trex, 1);
        put32(trex, id == kVideoTrack ? 3000 : 1024);
        put32(trex, 0);
        put32(trex, id == kVideoTrack ? 0x10000 : 0);
        mvex = cat({mvex, box("trex", trex)});
    }
    Bytes ftyp = {'i', 's', 'o', '5', 0, 0, 0, 1, 'i', 's', 'o', '5'};
    return cat({
        box("ftyp", ftyp), box("moov", cat({
            box("mvhd", mvhd), track(kVideoTrack, "vide", kVideoTimescale,
                videoEntry()), track(kAudioTrack, "soun", kAudioTimescale, audioEntry()), box("mvex", mvex),
        })),
    });
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
    size_t dataOffsetAt = videoTrun.size();
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
    size_t audioOffsetAt = audioTrun.size();
    put32(audioTrun, 0);
    for (uint32_t i = 0; i < audioCount; ++i)
        put32(audioTrun, 7);
    Bytes moof = box("moof", cat({
        box("mfhd", cat({full(0, 0), Bytes{0, 0, 0, 1}})),
        box("traf", cat({box("tfhd", videoTfhd), box("tfdt", videoTfdt), box("trun", videoTrun)})),
        box("traf", cat({box("tfhd", audioTfhd), box("tfdt", audioTfdt), box("trun", audioTrun)})),
    }));
    // The data offsets are from the start of the moof to the samples in the mdat that follows.
    uint32_t videoData = uint32_t(moof.size() + 8);
    uint32_t audioData = videoData + count * size;
    auto patch = [&](const Bytes& run, size_t offsetAt, uint32_t value) {
        size_t at = 0;
        for (size_t i = 0; i + run.size() <= moof.size(); ++i) {
            if (std::equal(run.begin(), run.end(), moof.begin() + long(i))) {
                at = i;
                break;
            }
        }
        for (int k = 0; k < 4; ++k)
            moof[at + offsetAt + size_t(k)] = uint8_t(value >> (24 - 8 * k));
    };
    patch(videoTrun, dataOffsetAt, videoData);
    patch(audioTrun, audioOffsetAt, audioData);
    Bytes data;
    for (uint32_t i = 0; i < count; ++i) {
        Bytes sample(size, uint8_t(i + 1));
        data.insert(data.end(), sample.begin(), sample.end());
    }
    Bytes audio(7 * size_t(audioCount), 0xaa);
    data.insert(data.end(), audio.begin(), audio.end());
    return cat({moof, box("mdat", data)});
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
