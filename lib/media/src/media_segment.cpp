// Media segments: the byte streams that Media Source Extensions append, parsed into initialization segments and coded
// frames.
//
// mse-byte-stream-format-isobmff: an initialization segment is an ftyp box and a moov box, and a media segment is a
// moof box with the mdat box that holds its samples. The parser keeps the bytes of a box until the box is whole, and of
// a moof until its mdat is whole, and reports a fragment's samples once both are.

#include "pr/media_segment.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/float.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/mime.h"

namespace pr_impl {

namespace {

constexpr uint32_t fourCc(const char (&code)[5]) {
    return uint32_t(uint8_t(code[0])) << 24 | uint32_t(uint8_t(code[1])) << 16 | uint32_t(uint8_t(code[2])) << 8 |
        uint32_t(uint8_t(code[3]));
}

// The longest box that the parser keeps whole, in bytes. A longer box is taken as a corrupt stream.
constexpr uint64_t kMaxBoxBytes = uint64_t(1) << 30;

// Reads big-endian fields from a span, and remembers whether any read ran past its end.
struct Reader {
    const uint8_t* data;
    size_t size;
    size_t at = 0;
    bool ok = true;

    bool has(size_t count) {
        ok = ok && count <= size - at && at <= size;
        return ok;
    }
    uint8_t read8() { return has(1) ? data[at++] : 0; }
    uint16_t read16() {
        if (!has(2))
            return 0;
        uint16_t value = uint16_t(data[at] << 8 | data[at + 1]);
        at += 2;
        return value;
    }
    uint32_t read32() {
        if (!has(4))
            return 0;
        uint32_t value = prMemReadBeU32(data + at);
        at += 4;
        return value;
    }
    uint64_t read64() {
        uint64_t high = read32();
        return high << 32 | read32();
    }
    void skip(size_t count) {
        if (has(count))
            at += count;
    }
    size_t left() const { return ok && at <= size ? size - at : 0; }
};

// A box: its type and payload, within a span.
struct Box {
    uint32_t type;
    const uint8_t* payload;
    size_t size;
};

// Reads the header of the box at the start of [data, data + size). Returns 0 if the header is incomplete, -1 if it is
// invalid, and otherwise writes the box's whole size and its header's size.
int boxHeader(const uint8_t* data, size_t size, uint64_t* total, uint32_t* header) {
    if (size < 8)
        return 0;
    uint64_t length = prMemReadBeU32(data);
    *header = 8;
    if (length == 1) {
        if (size < 16)
            return 0;
        length = uint64_t(prMemReadBeU32(data + 8)) << 32 | prMemReadBeU32(data + 12);
        *header = 16;
    }
    // A box that runs to the end of the file has no end in a stream.
    if (length < *header || length > kMaxBoxBytes)
        return -1;
    *total = length;
    return 1;
}

// Calls `visit` with each whole box of a span. Returns false if a box runs past the span's end.
template <typename Visit>
bool eachBox(const uint8_t* data, size_t size, Visit visit) {
    size_t at = 0;
    while (at < size) {
        uint64_t total = 0;
        uint32_t header = 0;
        if (boxHeader(data + at, size - at, &total, &header) != 1 || total > size - at)
            return false;
        visit(Box{prMemReadBeU32(data + at + 4), data + at + header, size_t(total - header)});
        at += size_t(total);
    }
    return true;
}

// Returns the first child of `type` within a box's payload, which starts `skip` bytes in, or a box of size zero and a
// null payload if there is none.
Box childOf(const Box& box, uint32_t type, size_t skip = 0) {
    Box found{0, nullptr, 0};
    if (skip > box.size)
        return found;
    eachBox(box.payload + skip, box.size - skip, [&](const Box& child) {
        if (found.payload == nullptr && child.type == type)
            found = child;
    });
    return found;
}

// What a track of the initialization segment says about its samples.
struct Mp4Track {
    uint32_t id;
    PrMediaTrackKind kind;
    uint32_t timescale;
    uint32_t defaultDuration;       // trex's default sample duration, in the track's timescale
    uint32_t defaultSize;
    uint32_t defaultFlags;
    uint32_t codecAt;               // the track's codec string in the parser's text, as an offset and a length
    uint32_t codecSize;
    uint32_t languageAt;
    uint32_t languageSize;
    uint32_t configAt;              // its configuration record in the parser's configs
    uint32_t configSize;
    PrUVec2 extent;
    uint32_t sampleRate;
    uint32_t channels;

    // The track's edit list (ISO/IEC 14496-12, 8.6.6): the duration of its empty edits before its first edit of media,
    // in the movie's timescale, and the media time at which that edit starts, in the track's. Together they give
    // `timeOffset`, in seconds, which moves the track's timestamps.
    uint64_t emptyDuration;
    int64_t mediaStart;
    double timeOffset;
};

// A sample of a track fragment.
struct Mp4Sample {
    uint64_t offset;                // from the start of the moof
    uint32_t size;
    uint32_t duration;
    int64_t compositionOffset;
    uint32_t flags;
};

// The samples' sample_is_non_sync_sample flag (ISO/IEC 14496-12, 8.8.3).
constexpr uint32_t kSampleNonSync = 0x10000;

struct MediaSegmentParser {
    PrMediaByteStream format;
    Array<uint8_t> pending;         // bytes that the parser has not consumed
    bool initialized;               // an initialization segment has been read
    Array<Mp4Track> tracks;
    double duration;
    Array<char> text;               // the tracks' codec strings and languages
    Array<uint8_t> configs;         // the tracks' configuration records
    Array<PrMediaTrackInfo> infos;
    Array<PrMediaCodedFrame> frames;
    Array<Mp4Sample> samples;
};

// Appends `text` to the parser's text and returns its offset.
uint32_t addText(MediaSegmentParser* p, PrStringView text) {
    uint32_t at = p->text.count;
    for (size_t i = 0; i < text.size; ++i)
        p->text.push(text.data[i]);
    return at;
}

void addHex(MediaSegmentParser* p, uint8_t value, bool upper) {
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    p->text.push(digits[value >> 4]);
    p->text.push(digits[value & 15]);
}

void addDecimal(MediaSegmentParser* p, uint32_t value) {
    char digits[10];
    uint32_t count = 0;
    do {
        digits[count++] = char('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count != 0)
        p->text.push(digits[--count]);
}

// Reads the length of an MPEG-4 descriptor (ISO/IEC 14496-1, 8.3.3), in up to four bytes of seven bits each.
uint32_t descriptorLength(Reader& r) {
    uint32_t length = 0;
    for (int i = 0; i < 4; ++i) {
        uint8_t byte = r.read8();
        length = length << 7 | (byte & 0x7f);
        if ((byte & 0x80) == 0)
            break;
    }
    return length;
}

// Reads an esds box's payload: the object type of its decoder and its decoder specific info, which for AAC is an
// AudioSpecificConfig. Writes the codecs parameter of the track (mp4a.40.2), and the config, to the track.
void readEsds(MediaSegmentParser* p, const Box& esds, Mp4Track* track) {
    Reader r{esds.payload, esds.size};
    r.skip(4);
    if (r.read8() != 3)
        return;
    descriptorLength(r);
    r.skip(2);
    uint8_t flags = r.read8();
    if ((flags & 0x80) != 0)
        r.skip(2);
    if ((flags & 0x40) != 0)
        r.skip(r.read8());
    if ((flags & 0x20) != 0)
        r.skip(2);
    if (r.read8() != 4)
        return;
    descriptorLength(r);
    uint8_t objectType = r.read8();
    r.skip(12);
    track->codecAt = addText(p, "mp4a.");
    addHex(p, objectType, false);
    if (r.ok && r.read8() == 5) {
        uint32_t length = descriptorLength(r);
        if (r.has(length) && length != 0) {
            track->configAt = p->configs.count;
            track->configSize = length;
            for (uint32_t i = 0; i < length; ++i)
                p->configs.push(r.data[r.at + i]);
            // The audio object type, five bits, or six more past 31 (ISO/IEC 14496-3, 1.6.2.1).
            uint32_t audioType = r.data[r.at] >> 3;
            if (audioType == 31 && length >= 2)
                audioType = 32 + ((uint32_t(r.data[r.at] & 7) << 3) | (r.data[r.at + 1] >> 5));
            if (objectType == 0x40) {
                p->text.push('.');
                addDecimal(p, audioType);
            }
        }
    }
    track->codecSize = p->text.count - track->codecAt;
}

// Reads a sample entry of a stsd box into the track: the codecs parameter that names it, its configuration record, and
// its extent or audio format.
void readSampleEntry(MediaSegmentParser* p, const Box& entry, Mp4Track* track) {
    char name[4] = {char(entry.type >> 24), char(entry.type >> 16), char(entry.type >> 8), char(entry.type)};
    Reader r{entry.payload, entry.size};
    if (track->kind == PrMediaTrackKind_Video) {
        // A visual sample entry's fields run 78 bytes before its boxes (ISO/IEC 14496-12, 12.1.3).
        r.skip(24);
        uint32_t width = r.read16();
        uint32_t height = r.read16();
        track->extent = PrUVec2{width, height};
        Box avcc = childOf(entry, fourCc("avcC"), 78);
        if ((entry.type == fourCc("avc1") || entry.type == fourCc("avc3")) && avcc.size >= 4) {
            track->codecAt = addText(p, PrStringView{name, 4});
            p->text.push('.');
            for (int i = 1; i <= 3; ++i)
                addHex(p, avcc.payload[i], true);
            track->codecSize = p->text.count - track->codecAt;
            track->configAt = p->configs.count;
            track->configSize = uint32_t(avcc.size);
            for (size_t i = 0; i < avcc.size; ++i)
                p->configs.push(avcc.payload[i]);
            return;
        }
    } else if (track->kind == PrMediaTrackKind_Audio) {
        // An audio sample entry's fields run 28 bytes before its boxes (ISO/IEC 14496-12, 12.2.3).
        r.skip(16);
        track->channels = r.read16();
        r.skip(6);
        track->sampleRate = r.read32() >> 16;
        if (entry.type == fourCc("mp4a")) {
            Box esds = childOf(entry, fourCc("esds"), 28);
            if (esds.payload != nullptr) {
                readEsds(p, esds, track);
                return;
            }
        }
    }
    // Any other codec is named by its sample entry's type, in lowercase.
    track->codecAt = p->text.count;
    for (char c : name)
        p->text.push(prAsciiLower(c));
    track->codecSize = 4;
}

// Reads the edit list of an edts box into the track.
void readEdits(const Box& edts, Mp4Track* track) {
    Box elst = childOf(edts, fourCc("elst"));
    Reader r{elst.payload, elst.size};
    uint8_t version = r.read8();
    r.skip(3);
    uint32_t count = r.read32();
    for (uint32_t i = 0; i < count && r.ok; ++i) {
        uint64_t duration = version == 1 ? r.read64() : r.read32();
        int64_t mediaTime = version == 1 ? int64_t(r.read64()) : int64_t(int32_t(r.read32()));
        r.skip(4);
        if (!r.ok)
            return;
        // An empty edit has a media time of -1, and delays what follows.
        if (mediaTime == -1) {
            track->emptyDuration += duration;
            continue;
        }
        track->mediaStart = mediaTime;
        return;
    }
}

// Reads a trak box. Returns false if it is not a track that the segment can describe.
bool readTrack(MediaSegmentParser* p, const Box& trak, Mp4Track* track) {
    Box edts = childOf(trak, fourCc("edts"));
    if (edts.payload != nullptr)
        readEdits(edts, track);
    Box tkhd = childOf(trak, fourCc("tkhd"));
    Box mdia = childOf(trak, fourCc("mdia"));
    if (tkhd.payload == nullptr || mdia.payload == nullptr)
        return false;
    Reader header{tkhd.payload, tkhd.size};
    uint8_t version = header.read8();
    header.skip(3 + (version == 1 ? 16 : 8));
    track->id = header.read32();
    Box mdhd = childOf(mdia, fourCc("mdhd"));
    Box hdlr = childOf(mdia, fourCc("hdlr"));
    if (mdhd.payload == nullptr || hdlr.payload == nullptr || !header.ok)
        return false;
    Reader media{mdhd.payload, mdhd.size};
    version = media.read8();
    media.skip(3 + (version == 1 ? 16 : 8));
    track->timescale = media.read32();
    media.skip(version == 1 ? 8 : 4);
    // The language is three letters, five bits each, offset from 0x60 (ISO/IEC 14496-12, 8.4.2.3).
    uint16_t language = media.read16();
    if (!media.ok || track->timescale == 0)
        return false;
    char letters[3] = {
        char(((language >> 10) & 31) + 0x60), char(((language >> 5) & 31) + 0x60), char((language & 31) + 0x60),
    };
    // und, for undetermined, is no language.
    bool undetermined = letters[0] == 'u' && letters[1] == 'n' && letters[2] == 'd';
    PrStringView named{letters, 3};
    track->languageAt = addText(p, undetermined || language == 0 ? "" : named);
    track->languageSize = p->text.count - track->languageAt;
    Reader handler{hdlr.payload, hdlr.size};
    handler.skip(8);
    uint32_t type = handler.read32();
    track->kind = PrMediaTrackKind_Other;
    if (type == fourCc("vide"))
        track->kind = PrMediaTrackKind_Video;
    else if (type == fourCc("soun"))
        track->kind = PrMediaTrackKind_Audio;
    Box minf = childOf(mdia, fourCc("minf"));
    Box stbl = minf.payload != nullptr ? childOf(minf, fourCc("stbl")) : minf;
    Box stsd = stbl.payload != nullptr ? childOf(stbl, fourCc("stsd")) : stbl;
    // A stsd box is a full box with an entry count before its entries, and the first entry describes the samples.
    Box entry{0, nullptr, 0};
    if (stsd.payload == nullptr || stsd.size < 8)
        return false;
    eachBox(stsd.payload + 8, stsd.size - 8, [&](const Box& child) {
        if (entry.payload == nullptr)
            entry = child;
    });
    if (entry.payload == nullptr)
        return false;
    readSampleEntry(p, entry, track);
    return true;
}

// Reads a moov box: its tracks, and the defaults of their samples that mvex gives.
bool readMovie(MediaSegmentParser* p, const Box& moov) {
    p->tracks.clear();
    p->text.clear();
    p->configs.clear();
    p->duration = kPrF64Nan;
    bool ok = true;
    uint32_t movieTimescale = 0;
    uint64_t movieDuration = 0;
    uint64_t fragmentDuration = 0;
    eachBox(moov.payload, moov.size, [&](const Box& box) {
        if (box.type == fourCc("mvhd")) {
            Reader r{box.payload, box.size};
            uint8_t version = r.read8();
            r.skip(3 + (version == 1 ? 16 : 8));
            movieTimescale = r.read32();
            movieDuration = version == 1 ? r.read64() : r.read32();
            // A duration of all ones is unknown.
            if (movieDuration == (version == 1 ? UINT64_MAX : UINT32_MAX))
                movieDuration = 0;
        } else if (box.type == fourCc("trak")) {
            Mp4Track track{};
            if (readTrack(p, box, &track))
                p->tracks.push(track);
            else
                ok = false;
        } else if (box.type == fourCc("mvex")) {
            Box mehd = childOf(box, fourCc("mehd"));
            Reader r{mehd.payload, mehd.size};
            uint8_t version = r.read8();
            r.skip(3);
            fragmentDuration = version == 1 ? r.read64() : r.read32();
        }
    });
    if (!ok || p->tracks.count == 0)
        return false;
    // mvex can come before or after the traks, so the defaults of each track's samples are read once all are known.
    eachBox(moov.payload, moov.size, [&](const Box& box) {
        if (box.type != fourCc("mvex"))
            return;
        eachBox(box.payload, box.size, [&](const Box& child) {
            if (child.type != fourCc("trex"))
                return;
            Reader r{child.payload, child.size};
            r.skip(4);
            uint32_t id = r.read32();
            r.skip(4);
            uint32_t duration = r.read32();
            uint32_t size = r.read32();
            uint32_t flags = r.read32();
            for (uint32_t i = 0; i < p->tracks.count; ++i) {
                Mp4Track& track = p->tracks.data[i];
                if (track.id == id && r.ok) {
                    track.defaultDuration = duration;
                    track.defaultSize = size;
                    track.defaultFlags = flags;
                }
            }
        });
    });
    uint64_t duration = fragmentDuration != 0 ? fragmentDuration : movieDuration;
    if (movieTimescale != 0 && duration != 0)
        p->duration = double(duration) / movieTimescale;
    for (uint32_t i = 0; i < p->tracks.count; ++i) {
        Mp4Track& track = p->tracks.data[i];
        double empty = movieTimescale != 0 ? double(track.emptyDuration) / movieTimescale : 0;
        track.timeOffset = empty - double(track.mediaStart) / track.timescale;
    }
    return true;
}

// Reports the parser's tracks as an initialization segment.
void reportInit(MediaSegmentParser* p, const PrMediaSegmentHandlers* handlers) {
    p->infos.clear();
    for (uint32_t i = 0; i < p->tracks.count; ++i) {
        const Mp4Track& track = p->tracks.data[i];
        p->infos.push(PrMediaTrackInfo{
            .id = track.id,
            .kind = track.kind,
            .codec = PrStringView{p->text.data + track.codecAt, track.codecSize},
            .language = PrStringView{p->text.data + track.languageAt, track.languageSize},
            .config = PrByteView{p->configs.data + track.configAt, track.configSize},
            .extent = track.extent,
            .sampleRate = track.sampleRate,
            .channels = track.channels,
        });
    }
    PrMediaInitSegment segment{.duration = p->duration, .trackCount = p->infos.count, .tracks = p->infos.data};
    handlers->initSegment(handlers->context, &segment);
}

// Reads a moof box at the start of `fragment`, whose samples are in the rest of `fragment`, and reports them. Returns
// false if the fragment is corrupt.
bool readFragment(MediaSegmentParser* p, const uint8_t* fragment, size_t size, const Box& moof,
        const PrMediaSegmentHandlers* handlers) {
    p->frames.clear();
    uint64_t nextData = 0;          // where the samples of a track fragment without a base start, from the moof
    bool ok = true;
    eachBox(moof.payload, moof.size, [&](const Box& traf) {
        if (traf.type != fourCc("traf") || !ok)
            return;
        Box tfhd = childOf(traf, fourCc("tfhd"));
        Reader header{tfhd.payload, tfhd.size};
        uint32_t flags = header.read32() & 0xffffff;
        uint32_t id = header.read32();
        Mp4Track* track = nullptr;
        for (uint32_t i = 0; i < p->tracks.count; ++i)
            track = p->tracks.data[i].id == id ? &p->tracks.data[i] : track;
        if (tfhd.payload == nullptr || !header.ok || track == nullptr) {
            ok = false;
            return;
        }
        // The first track fragment's data starts at the moof, and each later one's after the data before it, unless the
        // fragment gives a base, or says that the moof is the base (ISO/IEC 14496-12, 8.8.7.1).
        uint64_t base = (flags & 0x20000) != 0 ? 0 : nextData;
        if ((flags & 0x1) != 0) {
            // A base data offset is from the start of the file, which a stream has no place for. The moof's offset in
            // the bytes appended is taken as the file's start.
            header.read64();
            base = 0;
        }
        if ((flags & 0x2) != 0)
            header.read32();
        uint32_t defaultDuration = (flags & 0x8) != 0 ? header.read32() : track->defaultDuration;
        uint32_t defaultSize = (flags & 0x10) != 0 ? header.read32() : track->defaultSize;
        uint32_t defaultFlags = (flags & 0x20) != 0 ? header.read32() : track->defaultFlags;
        uint64_t decodeTime = 0;
        Box tfdt = childOf(traf, fourCc("tfdt"));
        if (tfdt.payload != nullptr) {
            Reader r{tfdt.payload, tfdt.size};
            uint8_t version = r.read8();
            r.skip(3);
            decodeTime = version == 1 ? r.read64() : r.read32();
        }
        uint64_t dataAt = base;
        eachBox(traf.payload, traf.size, [&](const Box& trun) {
            if (trun.type != fourCc("trun") || !ok)
                return;
            Reader r{trun.payload, trun.size};
            uint8_t version = r.read8();
            uint32_t runFlags = uint32_t(r.read8()) << 16 | r.read16();
            uint32_t count = r.read32();
            if ((runFlags & 0x1) != 0)
                dataAt = base + uint64_t(int64_t(int32_t(r.read32())));
            uint32_t firstFlags = (runFlags & 0x4) != 0 ? r.read32() : defaultFlags;
            p->samples.clear();
            for (uint32_t i = 0; i < count && r.ok; ++i) {
                Mp4Sample sample{};
                sample.offset = dataAt;
                sample.duration = (runFlags & 0x100) != 0 ? r.read32() : defaultDuration;
                sample.size = (runFlags & 0x200) != 0 ? r.read32() : defaultSize;
                // The first sample takes the run's first sample flags, when the run gives them.
                uint32_t flags = i == 0 && (runFlags & 0x4) != 0 ? firstFlags : defaultFlags;
                sample.flags = (runFlags & 0x400) != 0 ? r.read32() : flags;
                if ((runFlags & 0x800) != 0) {
                    uint32_t offset = r.read32();
                    sample.compositionOffset = version == 0 ? int64_t(offset) : int64_t(int32_t(offset));
                }
                dataAt += sample.size;
                p->samples.push(sample);
            }
            if (!r.ok) {
                ok = false;
                return;
            }
            for (uint32_t i = 0; i < p->samples.count; ++i) {
                const Mp4Sample& sample = p->samples.data[i];
                if (sample.offset > size || sample.size > size - sample.offset) {
                    ok = false;
                    return;
                }
                double timescale = track->timescale;
                p->frames.push(PrMediaCodedFrame{
                    .track = track->id,
                    .presentation = double(int64_t(decodeTime) + sample.compositionOffset) / timescale +
                        track->timeOffset,
                    .decode = double(decodeTime) / timescale + track->timeOffset,
                    .duration = double(sample.duration) / timescale,
                    // Every audio frame decodes on its own.
                    .key = track->kind == PrMediaTrackKind_Audio || (sample.flags & kSampleNonSync) == 0,
                    .bytes = PrByteView{fragment + sample.offset, sample.size},
                });
                decodeTime += sample.duration;
            }
        });
        nextData = dataAt;
    });
    if (ok && p->frames.count != 0)
        handlers->codedFrames(handlers->context, p->frames.data, p->frames.count);
    return ok;
}

// Parses the whole boxes at the start of the pending bytes, and removes them. Returns false if the stream is corrupt.
bool parseMp4(MediaSegmentParser* p, const PrMediaSegmentHandlers* handlers) {
    size_t at = 0;
    bool ok = true;
    for (;;) {
        const uint8_t* data = p->pending.data + at;
        size_t size = p->pending.count - at;
        uint64_t total = 0;
        uint32_t header = 0;
        int found = boxHeader(data, size, &total, &header);
        if (found < 0) {
            ok = false;
            break;
        }
        if (found == 0 || total > size)
            break;
        Box box{prMemReadBeU32(data + 4), data + header, size_t(total - header)};
        if (box.type == fourCc("moov")) {
            if (!readMovie(p, box)) {
                ok = false;
                break;
            }
            p->initialized = true;
            reportInit(p, handlers);
        } else if (box.type == fourCc("moof")) {
            // A media segment before an initialization segment is an error
            // (media-source-2/#sourcebuffer-segment-parser-loop ). The moof's samples are in the mdat that follows,
            // which must be whole too.
            if (!p->initialized) {
                ok = false;
                break;
            }
            size_t end = size_t(total);
            bool whole = false;
            for (;;) {
                uint64_t next = 0;
                uint32_t nextHeader = 0;
                int nextFound = boxHeader(data + end, size - end, &next, &nextHeader);
                if (nextFound < 0) {
                    ok = false;
                    break;
                }
                if (nextFound == 0 || next > size - end)
                    break;
                bool mdat = prMemReadBeU32(data + end + 4) == fourCc("mdat");
                end += size_t(next);
                if (mdat) {
                    whole = true;
                    break;
                }
            }
            if (!ok || !whole)
                break;
            if (!readFragment(p, data, end, box, handlers)) {
                ok = false;
                break;
            }
            total = end;
        }
        // Any other box, such as ftyp, styp, sidx, emsg, free or an mdat with no moof before it, carries nothing that
        // the parser reports.
        at += size_t(total);
    }
    p->pending.erase(0, uint32_t(at));
    return ok;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrMediaSegmentParser : MediaSegmentParser {};

PrMediaByteStream prMediaByteStreamFromType(PrStringView type) {
    PrStringView essence = prMimeTypeEssence(type);
    if (prAsciiEqualsFold(essence, "video/mp4") || prAsciiEqualsFold(essence, "audio/mp4"))
        return PrMediaByteStream_Mp4;
    return PrMediaByteStream_None;
}

PrMediaSegmentParser* prMediaSegmentParserCreate(PrMediaByteStream format) {
    PR_ASSERT(format == PrMediaByteStream_Mp4);
    PrMediaSegmentParser* parser = allocZeroed<PrMediaSegmentParser>();
    parser->format = format;
    parser->duration = kPrF64Nan;
    return parser;
}

void prMediaSegmentParserDestroy(PrMediaSegmentParser* PR_NULLABLE parser) {
    if (parser == nullptr)
        return;
    parser->pending.reset();
    parser->tracks.reset();
    parser->text.reset();
    parser->configs.reset();
    parser->infos.reset();
    parser->frames.reset();
    parser->samples.reset();
    prMemoryFree(parser);
}

PrStatus prMediaSegmentParserAppend(PrMediaSegmentParser* parser, PrByteView bytes,
        const PrMediaSegmentHandlers* handlers) {
    if (bytes.size != 0)
        parser->pending.append(bytes.data, uint32_t(bytes.size));
    return parseMp4(parser, handlers) ? PrStatus_Ok : PrStatus_InvalidData;
}

bool prMediaSegmentParserIsParsingMedia(const PrMediaSegmentParser* parser) {
    // The pending bytes hold a moof whose mdat has not arrived, or part of a box. An initialization segment's bytes in
    // flight are not part of a media segment, but a parser of a stream that has begun one cannot tell yet.
    return parser->initialized && parser->pending.count != 0;
}

void prMediaSegmentParserReset(PrMediaSegmentParser* parser) {
    parser->pending.clear();
}
