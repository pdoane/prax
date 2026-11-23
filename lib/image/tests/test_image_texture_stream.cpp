// Image texture sources whose bytes are still arriving: when they open, the revisions that they show as the bytes
// arrive, the picture that they end with, and how they fail.

#include "pr/array.h"
#include "pr/blob.h"
#include "pr/checksum.h"
#include "pr/compress_select.h"
#include "pr/gpu.h"
#include "pr/gpu_memory.h"
#include "pr/image_select.h"
#include "pr/image_texture_source.h"
#include "pr/mem.h"
#include "pr/min_max.h"
#include "pr/png.h"
#include "pr/resource.h"
#include "pr/texture.h"
#include "pr/texture_cache.h"
#include "pr/texture_catalog.h"

#include "tools/harness/animated_gif.h"
#include "tools/harness/jpeg_vectors.h"
#include "tools/harness/psd_document.h"
#include "tools/harness/texture_await.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

using namespace harness;

// The codecs and the texture format of the tests.
struct Formats {
    PrCompressCodecSet* compress = prCompressCodecSetCreateDefault();
    PrImageCodecSet* images = prImageCodecSetCreateDefault(compress);
    PrTextureFormatSet* set = nullptr;

    Formats() {
        PrTextureFormat* format = prImageTextureFormatCreate(images, PrImageTextureColor_Stored);
        set = prTextureFormatSetCreate(&format, 1);
    }
    Formats(const Formats&) = delete;
    Formats& operator=(const Formats&) = delete;
    ~Formats() {
        prTextureFormatSetDestroy(set);
        prImageCodecSetDestroy(images);
        prCompressCodecSetDestroy(compress);
    }

    // Returns a PNG of `extent` opaque pixels of noise, interlaced if `interlaced` is set.
    Array<uint8_t> noisePng(PrUVec2 extent, bool interlaced) const {
        Array<uint8_t> pixels(extent.x * extent.y * 4);
        uint32_t seed = 7;
        for (uint32_t i = 0; i < pixels.count(); ++i) {
            seed = seed * 1103515245u + 12345u;
            pixels[i] = i % 4 == 3 ? 255 : uint8_t(seed >> 16);
        }
        PrPixelView view{pixels.data(), size_t(extent.x) * 4, extent};
        PrPngEncodeFlags flags = interlaced ? PrPngEncodeFlag_Interlaced : 0;
        Array<uint8_t> file(uint32_t(prPngEncode(view, flags, compress, nullptr, 0)));
        prPngEncode(view, flags, compress, file.data(), file.count());
        return file;
    }
};

// Appends a chunk of `type` and `data` to `out`, with its length and its CRC (png 5.3).
void putChunk(Array<uint8_t>* out, const char* type, PrByteView data) {
    const uint8_t length[4] = {
        uint8_t(data.size >> 24), uint8_t(data.size >> 16), uint8_t(data.size >> 8),
        uint8_t(data.size),
    };
    out->append(length, 4);
    Array<uint8_t> body;
    body.append(reinterpret_cast<const uint8_t*>(type), 4);
    if (data.size != 0)
        body.append(data.data, uint32_t(data.size));
    out->append(body.data(), body.count());
    const uint32_t crc = prCrc32Update(0, PrByteView{body.data(), body.count()});
    const uint8_t check[4] = {uint8_t(crc >> 24), uint8_t(crc >> 16), uint8_t(crc >> 8), uint8_t(crc)};
    out->append(check, 4);
}

// Writes the 4-byte big-endian `value` at `at`.
void putBeU32(uint8_t* at, uint32_t value) {
    at[0] = uint8_t(value >> 24);
    at[1] = uint8_t(value >> 16);
    at[2] = uint8_t(value >> 8);
    at[3] = uint8_t(value);
}

// Returns the data of the IDAT chunks of a PNG, joined.
Array<uint8_t> imageData(const Array<uint8_t>& png) {
    Array<uint8_t> data;
    for (uint32_t at = 8; at + 12 <= png.count();) {
        const uint32_t length = uint32_t(png[at]) << 24 | uint32_t(png[at + 1]) << 16 | uint32_t(png[at + 2]) << 8 |
            png[at + 3];
        if (prMemEquals(png.data() + at + 4, "IDAT", 4))
            data.append(png.data() + at + 8, length);
        at += length + 12;
    }
    return data;
}

// Returns an animated PNG of 16 by 16 of `frames` frames, each half a second, played forever. Frame `i` is opaque, of
// one color whose red is 40 * (i + 1).
Array<uint8_t> animatedPng(const Formats& formats, uint32_t frames) {
    Array<uint8_t> out;
    const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    out.append(signature, 8);
    uint32_t sequence = 0;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        Array<uint8_t> pixels(16 * 16 * 4);
        for (uint32_t i = 0; i < pixels.count(); i += 4) {
            pixels[i] = uint8_t(40 * (frame + 1));
            pixels[i + 3] = 255;
        }
        PrPixelView view{pixels.data(), 16 * 4, {16, 16}};
        Array<uint8_t> png(uint32_t(prPngEncode(view, 0, formats.compress, nullptr, 0)));
        prPngEncode(view, 0, formats.compress, png.data(), png.count());
        if (frame == 0) {
            out.append(png.data() + 8, 25);
            uint8_t control[8] = {};
            putBeU32(control, frames);
            putChunk(&out, "acTL", PrByteView{control, 8});
        }
        uint8_t fctl[26] = {};
        putBeU32(fctl, sequence++);
        putBeU32(fctl + 4, 16);
        putBeU32(fctl + 8, 16);
        fctl[21] = 1;
        fctl[23] = 2;
        putChunk(&out, "fcTL", PrByteView{fctl, 26});
        Array<uint8_t> data = imageData(png);
        if (frame == 0) {
            putChunk(&out, "IDAT", PrByteView{data.data(), data.count()});
        } else {
            Array<uint8_t> fdat(4);
            putBeU32(fdat.data(), sequence++);
            fdat.append(data.data(), data.count());
            putChunk(&out, "fdAT", PrByteView{fdat.data(), fdat.count()});
        }
    }
    putChunk(&out, "IEND", PrByteView{});
    return out;
}

// The result of an open, which may complete on the thread that feeds the bytes.
struct Opened {
    bool done = false;
    PrStatus status = PrStatus_Ok;
    const PrTextureSource* source = nullptr;

    static void fn(void* PR_NULLABLE context, PrStatus status, const PrTextureSource* PR_NULLABLE source) {
        Opened* opened = static_cast<Opened*>(context);
        opened->done = true;
        opened->status = status;
        opened->source = source;
    }

    ~Opened() { prTextureSourceRelease(source); }
};

// Reads every texel of `source` into `out`. Returns the status of the read.
PrStatus readAll(const PrTextureSource* source, Array<uint8_t>* out) {
    PrTextureReadDesc desc = prTextureReadDescAll(prTextureSourceDesc(source));
    PrTextureRead* read = prTextureReadCreate(source, &desc);
    PrStatus status = textureReadAwait(read);
    out->clear();
    if (status == PrStatus_Ok)
        out->append(PrByteView{prTextureReadStorage(read), prTextureReadSize(prTextureSourceDesc(source), &desc)});
    prTextureReadDestroy(read);
    return status;
}

// Returns the texels of the image of every byte of `file`, as a source opened from the whole file reads them.
Array<uint8_t> wholeTexels(const Formats& formats, const Array<uint8_t>& file) {
    const PrTextureSource* source = nullptr;
    REQUIRE(prImageTextureSourceOpenBytes(PrByteView{file.data(), file.count()}, formats.images, nullptr,
        PrImageTextureColor_Stored, &source) == PrStatus_Ok);
    Array<uint8_t> texels;
    REQUIRE(readAll(source, &texels) == PrStatus_Ok);
    prTextureSourceRelease(source);
    return texels;
}

// Returns the rows of Rgba8 `texels` of `width` whose alpha is not all zero: the rows that a revision shows.
uint32_t rowsShown(const Array<uint8_t>& texels, uint32_t width) {
    uint32_t rows = 0;
    for (uint32_t y = 0; y < texels.count() / (width * 4); ++y) {
        bool shown = false;
        for (uint32_t x = 0; x < width && !shown; ++x)
            shown = texels[(y * width + x) * 4 + 3] != 0;
        rows += shown ? 1 : 0;
    }
    return rows;
}

// Returns the share of the bytes of `a` that are within 32 of those of `b`.
double nearShare(const Array<uint8_t>& a, const Array<uint8_t>& b) {
    uint32_t near = 0;
    for (uint32_t i = 0; i < a.count(); ++i)
        near += (a[i] > b[i] ? a[i] - b[i] : b[i] - a[i]) <= 32 ? 1 : 0;
    return double(near) / double(a.count());
}

// Appends the bytes of `file` from `*at` to `end` in pieces of `piece` bytes.
void feedTo(PrBlobFeed* feed, const Array<uint8_t>& file, uint32_t* at, uint32_t end, uint32_t piece) {
    while (*at < end) {
        uint32_t size = end - *at < piece ? end - *at : piece;
        prBlobFeedAppend(feed, PrByteView{file.data() + *at, size});
        *at += size;
    }
}

// What a source showed at one revision.
struct Revision {
    uint64_t serial;
    bool complete;
    Array<uint8_t> texels;
};

// Feeds `file` in pieces of `piece` bytes, opening it as it arrives, and records each picture that the source shows at
// time zero while the bytes arrive. Ends the bytes, and returns the source with a reference for the caller.
const PrTextureSource* streamed(const Formats& formats, const Array<uint8_t>& file, uint32_t piece,
        Array<Revision>* revisions) {
    PrBlobFeed* feed = prBlobFeedCreate(kPrBlobSizeUnknown);
    Opened opened;
    prTextureFormatSetOpen(formats.set, prBlobFeedSource(feed), "streamed", nullptr, Opened::fn, &opened);
    CHECK(!opened.done);
    uint32_t at = 0;
    uint64_t last = ~uint64_t(0);
    while (at < file.count()) {
        feedTo(feed, file, &at, at + piece < file.count() ? at + piece : file.count(), piece);
        if (!opened.done)
            continue;
        REQUIRE(opened.status == PrStatus_Ok);
        PrTextureShow show{};
        prTextureSourceShow(opened.source, 0, &show);
        CHECK(show.status == PrStatus_Ok);
        if (show.serial != last) {
            last = show.serial;
            Revision revision{.serial = show.serial, .complete = show.complete};
            REQUIRE(readAll(show.picture, &revision.texels) == PrStatus_Ok);
            revisions->push(static_cast<Revision&&>(revision));
        }
        prTextureSourceRelease(show.picture);
    }
    prBlobFeedEnd(feed);
    prBlobFeedDestroy(feed);
    REQUIRE(opened.done);
    const PrTextureSource* source = opened.source;
    opened.source = nullptr;
    return source;
}

// Returns the first byte of the region's texels on the memory device.
uint8_t firstByte(PrGpuDevice* device, const PrTextureRegion& region) {
    PrGpuNativePixels native{};
    REQUIRE(prGpuTextureNative(device, region.texture.texture, PrGpuNativeKind_Pixels, &native));
    return native.pixels.data[size_t(region.rect.top) * native.pixels.stride + size_t(region.rect.left) * 4];
}

// Asks the cache to play `source`, and lets go of the picture.
PrTextureCacheState playOf(PrTextureCache* cache, const PrTextureSource* source, PrTextureRegion* region) {
    const PrTextureSource* picture = nullptr;
    PrTextureCacheAsk ask = prTextureCacheAskEncoded(0);
    ask.alpha = PrTextureAlpha_Straight;
    PrTextureCacheState state = prTextureCachePlay(cache, source, ask, nullptr, region, &picture);
    prTextureSourceRelease(picture);
    return state;
}

// Returns a JPEG of `image` whose Exif data embeds `thumbnail`, in an APP1 segment after the image's SOI marker.
Array<uint8_t> withThumbnail(PrByteView image, PrByteView thumbnail) {
    // The TIFF structure is a header, an empty first directory, the three fields of the second directory, and the
    // thumbnail at offset 56. The first directory links to the second at offset 14.
    Array<uint8_t> tiff;
    auto push = [](Array<uint8_t>& out, uint32_t value, uint32_t bytes) {
        for (uint32_t i = bytes; i-- > 0;)
            out.push(uint8_t(value >> (8 * i)));
    };
    const uint8_t header[] = {'M', 'M', 0, 42, 0, 0, 0, 8};
    tiff.append(header, 8);
    push(tiff, 0, 2);
    push(tiff, 14, 4);
    push(tiff, 3, 2);
    const uint32_t fields[3][3] = {{0x0103, 3, 6u << 16}, {0x0201, 4, 56}, {0x0202, 4, uint32_t(thumbnail.size)}};
    for (const uint32_t* field : fields) {
        push(tiff, field[0], 2);
        push(tiff, field[1], 2);
        push(tiff, 1, 4);
        push(tiff, field[2], 4);
    }
    push(tiff, 0, 4);
    tiff.append(thumbnail.data, uint32_t(thumbnail.size));
    Array<uint8_t> file;
    file.append(image.data, 2);
    const uint8_t marker[] = {0xFF, 0xE1};
    file.append(marker, 2);
    push(file, 2 + 6 + tiff.count(), 2);
    const uint8_t exif[] = {'E', 'x', 'i', 'f', 0, 0};
    file.append(exif, 6);
    file.append(tiff.data(), tiff.count());
    file.append(image.data + 2, uint32_t(image.size - 2));
    return file;
}

PrTextureCache* cacheOn(PrGpuDevice* device) {
    PrTextureCacheDesc desc = prTextureCacheDescDefault(device);
    desc.pack = PrTextureCachePack_Never;
    return prTextureCacheCreate(&desc);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_SUITE_BEGIN("image texture streams");

TEST_CASE("a still image fed in pieces shows revisions of a growing part, and ends as the image of every byte") {
    Formats formats;
    Array<uint8_t> file = formats.noisePng({64, 64}, false);
    Array<Revision> revisions;
    const PrTextureSource* source = streamed(formats, file, 256, &revisions);
    REQUIRE(source != nullptr);
    CHECK(prTextureSourceChanges(source));
    CHECK(prTextureSourceNotifier(source) != nullptr);
    // The revisions cover more rows each time, and none is complete before the decoder has the whole image.
    REQUIRE(revisions.count() >= 3);
    uint32_t rows = 0;
    bool part = false;
    for (const Revision& revision : revisions) {
        uint32_t shown = rowsShown(revision.texels, 64);
        CHECK(shown >= rows);
        CHECK(revision.complete == (shown == 64 && &revision == &revisions[revisions.count() - 1]));
        part = part || (shown != 0 && shown < 64);
        rows = shown;
    }
    CHECK(part);
    // The source then shows the image of every byte, as a whole file opens it, and never changes again.
    PrTextureShow show{};
    prTextureSourceShow(source, 0, &show);
    REQUIRE(show.picture != nullptr);
    CHECK(!prTextureSourceChanges(show.picture));
    CHECK(show.complete);
    CHECK(!show.notifies);
    CHECK(show.next == kPrF64Infinity);
    CHECK(show.status == PrStatus_Ok);
    Array<uint8_t> final;
    REQUIRE(readAll(show.picture, &final) == PrStatus_Ok);
    CHECK(final.span() == wholeTexels(formats, file).span());
    prTextureSourceRelease(show.picture);
    prTextureSourceRelease(source);
}

TEST_CASE("a source of arriving bytes plays in the cache as Partial revisions, then costs frames nothing") {
    Formats formats;
    Array<uint8_t> file = formats.noisePng({32, 32}, false);
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    PrTextureCache* cache = cacheOn(device);
    PrBlobFeed* feed = prBlobFeedCreate(kPrBlobSizeUnknown);
    Opened opened;
    prTextureFormatSetOpen(formats.set, prBlobFeedSource(feed), "x.png", nullptr, Opened::fn, &opened);
    uint32_t at = 0;
    feedTo(feed, file, &at, file.count() / 2, 64);
    REQUIRE(opened.done);
    REQUIRE(opened.status == PrStatus_Ok);
    PrTextureRegion region{};
    CHECK(playOf(cache, opened.source, &region) == PrTextureCacheState_Partial);
    CHECK(prTextureCacheNextFrame(cache) == 0);
    // More bytes notify, which is an arrival.
    uint64_t arrivals = prTextureCacheArrivals(cache);
    feedTo(feed, file, &at, file.count() - 1, 64);
    CHECK(prTextureCacheArrivals(cache) != arrivals);
    CHECK(playOf(cache, opened.source, &region) == PrTextureCacheState_Partial);
    feedTo(feed, file, &at, file.count(), 64);
    prBlobFeedEnd(feed);
    CHECK(playOf(cache, opened.source, &region) == PrTextureCacheState_Ready);
    CHECK(firstByte(device, region) == wholeTexels(formats, file)[0]);
    // After the bytes are complete, frames do not wake the host or add a time to draw at.
    arrivals = prTextureCacheArrivals(cache);
    for (uint32_t frame = 0; frame < 50; ++frame) {
        prTextureCacheSetTime(cache, frame * 0.1);
        CHECK(playOf(cache, opened.source, &region) == PrTextureCacheState_Ready);
        prTextureCacheTrim(cache);
    }
    CHECK(prTextureCacheArrivals(cache) == arrivals);
    CHECK(prTextureCacheNextFrame(cache) == 0);
    prBlobFeedDestroy(feed);
    prTextureCacheDestroy(cache);
    prGpuDeviceDestroy(device);
}

TEST_CASE("an interlaced PNG fed in pieces shows the whole picture coarsely before it shows it whole") {
    Formats formats;
    Array<uint8_t> file = formats.noisePng({64, 64}, true);
    Array<Revision> revisions;
    const PrTextureSource* source = streamed(formats, file, 256, &revisions);
    REQUIRE(source != nullptr);
    Array<uint8_t> whole = wholeTexels(formats, file);
    // Some revision covers every row, as blocks of the passes so far, while it is not yet the picture.
    bool coarse = false;
    for (const Revision& revision : revisions)
        coarse = coarse || (rowsShown(revision.texels, 64) == 64 && revision.texels.span() != whole.span());
    CHECK(coarse);
    Array<uint8_t> final;
    REQUIRE(readAll(source, &final) == PrStatus_Ok);
    CHECK(final.span() == whole.span());
    prTextureSourceRelease(source);
}

TEST_CASE("a progressive JPEG fed in pieces shows the whole picture at the precision of its scans so far") {
    Formats formats;
    PrBlob* blob = prResourceOpen("stream/progressive.jpg");
    REQUIRE(blob != nullptr);
    Array<uint8_t> file;
    file.append(prBlobBytes(blob));
    prBlobRelease(blob);
    Array<Revision> revisions;
    const PrTextureSource* source = streamed(formats, file, 128, &revisions);
    REQUIRE(source != nullptr);
    Array<uint8_t> whole = wholeTexels(formats, file);
    // Each revision covers every row, and the revisions grow nearer the picture: the last one before the bytes end is
    // near it almost everywhere.
    REQUIRE(revisions.count() >= 3);
    double first = nearShare(revisions[1].texels, whole);
    double later = nearShare(revisions[revisions.count() - 1].texels, whole);
    for (const Revision& revision : revisions)
        CHECK(rowsShown(revision.texels, prTextureSourceDesc(source)->extent.x) ==
            prTextureSourceDesc(source)->extent.y);
    CHECK(later >= first);
    CHECK(later > 0.9);
    Array<uint8_t> final;
    REQUIRE(readAll(source, &final) == PrStatus_Ok);
    CHECK(final.span() == whole.span());
    prTextureSourceRelease(source);
}

TEST_CASE("a stream that fails midway keeps its last picture and reports the failure") {
    Formats formats;
    Array<uint8_t> file = formats.noisePng({64, 64}, false);
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    PrTextureCache* cache = cacheOn(device);
    PrBlobFeed* feed = prBlobFeedCreate(kPrBlobSizeUnknown);
    Opened opened;
    prTextureFormatSetOpen(formats.set, prBlobFeedSource(feed), "x.png", nullptr, Opened::fn, &opened);
    uint32_t at = 0;
    feedTo(feed, file, &at, file.count() / 2, 128);
    REQUIRE(opened.done);
    REQUIRE(opened.status == PrStatus_Ok);
    PrTextureRegion region{};
    CHECK(playOf(cache, opened.source, &region) == PrTextureCacheState_Partial);
    prBlobFeedFail(feed, PrStatus_Io);
    PrTextureShow show{};
    prTextureSourceShow(opened.source, 0, &show);
    CHECK(show.status == PrStatus_Io);
    CHECK(show.complete);
    CHECK(!show.notifies);
    CHECK(show.picture != opened.source);
    Array<uint8_t> texels;
    REQUIRE(readAll(show.picture, &texels) == PrStatus_Ok);
    uint32_t rows = rowsShown(texels, 64);
    CHECK(rows > 0);
    CHECK(rows < 64);
    prTextureSourceRelease(show.picture);
    // The cache shows the last picture as the one that stays, and says why it stopped.
    CHECK(playOf(cache, opened.source, &region) == PrTextureCacheState_Ready);
    CHECK(prTextureCacheStatus(cache, opened.source, prTextureCacheAskEncoded(0)) == PrStatus_Io);
    prBlobFeedDestroy(feed);
    prTextureCacheDestroy(cache);
    prGpuDeviceDestroy(device);
}

TEST_CASE("a stream whose header never completes fails to open as its bytes end or fail") {
    Formats formats;
    Array<uint8_t> file = formats.noisePng({16, 16}, false);
    struct Ending {
        PrStatus fail;
        PrStatus opened;
    };
    const Ending endings[] = {{PrStatus_Ok, PrStatus_InvalidData}, {PrStatus_Io, PrStatus_Io}};
    for (const Ending& ending : endings) {
        PrBlobFeed* feed = prBlobFeedCreate(kPrBlobSizeUnknown);
        Opened opened;
        prTextureFormatSetOpen(formats.set, prBlobFeedSource(feed), "x.png", nullptr, Opened::fn, &opened);
        prBlobFeedAppend(feed, PrByteView{file.data(), 20});
        CHECK(!opened.done);
        if (ending.fail == PrStatus_Ok)
            prBlobFeedEnd(feed);
        else
            prBlobFeedFail(feed, ending.fail);
        REQUIRE(opened.done);
        CHECK(opened.status == ending.opened);
        CHECK(opened.source == nullptr);
        prBlobFeedDestroy(feed);
    }
    // A feed that is let go before its header arrives fails the open with Lost.
    PrBlobFeed* feed = prBlobFeedCreate(kPrBlobSizeUnknown);
    Opened opened;
    prTextureFormatSetOpen(formats.set, prBlobFeedSource(feed), "x.png", nullptr, Opened::fn, &opened);
    prBlobFeedDestroy(feed);
    REQUIRE(opened.done);
    CHECK(opened.status == PrStatus_Lost);
}

TEST_CASE("a stream that arrives across a device loss ends as its picture on the new device") {
    Formats formats;
    Array<uint8_t> file = formats.noisePng({32, 32}, false);
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    PrTextureCache* cache = cacheOn(device);
    PrBlobFeed* feed = prBlobFeedCreate(kPrBlobSizeUnknown);
    Opened opened;
    prTextureFormatSetOpen(formats.set, prBlobFeedSource(feed), "x.png", nullptr, Opened::fn, &opened);
    uint32_t at = 0;
    feedTo(feed, file, &at, file.count() / 2, 64);
    REQUIRE(opened.done);
    PrTextureRegion region{};
    CHECK(playOf(cache, opened.source, &region) == PrTextureCacheState_Partial);
    // The device is lost while the bytes arrive, and the cache has no device for a while.
    prTextureCacheSetDevice(cache, nullptr);
    prGpuDeviceDestroy(device);
    feedTo(feed, file, &at, file.count() * 3 / 4, 64);
    CHECK(playOf(cache, opened.source, &region) == PrTextureCacheState_Pending);
    PrGpuDevice* next = prGpuMemoryCreate(nullptr);
    prTextureCacheSetDevice(cache, next);
    CHECK(playOf(cache, opened.source, &region) == PrTextureCacheState_Partial);
    feedTo(feed, file, &at, file.count(), 64);
    prBlobFeedEnd(feed);
    CHECK(playOf(cache, opened.source, &region) == PrTextureCacheState_Ready);
    CHECK(firstByte(next, region) == wholeTexels(formats, file)[0]);
    prBlobFeedDestroy(feed);
    prTextureCacheDestroy(cache);
    prGpuDeviceDestroy(next);
}

TEST_CASE("an animation that is still arriving plays the frames that exist, and later frames appear") {
    Formats formats;
    Array<uint8_t> gif = animatedGif(3, 50, true);
    PrBlobFeed* feed = prBlobFeedCreate(kPrBlobSizeUnknown);
    Opened opened;
    prTextureFormatSetOpen(formats.set, prBlobFeedSource(feed), "x.gif", nullptr, Opened::fn, &opened);
    // The bytes of the first frame, and the start of the second: its graphic control extension and image descriptor,
    // and a byte of its data. They arrive in one piece after the header, which grows the bytes enough for a revision.
    uint32_t controls = 0;
    uint32_t second = 0;
    for (uint32_t i = 0; i + 1 < gif.count() && second == 0; ++i) {
        if (gif[i] == 0x21 && gif[i + 1] == 0xF9 && ++controls == 2)
            second = i + 21;
    }
    REQUIRE(second != 0);
    uint32_t at = 0;
    feedTo(feed, gif, &at, 30, 30);
    REQUIRE(opened.done);
    REQUIRE(opened.status == PrStatus_Ok);
    feedTo(feed, gif, &at, second, second);
    const PrTextureSource* source = opened.source;
    // The first frame is complete, and shows for its duration.
    PrTextureShow show{};
    prTextureSourceShow(source, 0.1, &show);
    CHECK(show.complete);
    CHECK(show.notifies);
    CHECK(show.next == doctest::Approx(0.5));
    Array<uint8_t> texels;
    REQUIRE(readAll(show.picture, &texels) == PrStatus_Ok);
    CHECK(texels[0] == 255);
    prTextureSourceRelease(show.picture);
    // Past it, the frame that is arriving shows, and waits for its bytes.
    prTextureSourceShow(source, 3, &show);
    CHECK(!show.complete);
    CHECK(show.notifies);
    CHECK(show.next == kPrF64Infinity);
    uint64_t arriving = show.serial;
    prTextureSourceRelease(show.picture);
    // Every frame arrives. The animation then loops: at 3.6 seconds, 600 ms into the second play, the second frame
    // shows, which is green.
    feedTo(feed, gif, &at, gif.count(), 4);
    prBlobFeedEnd(feed);
    prTextureSourceShow(source, 3.6, &show);
    CHECK(show.complete);
    CHECK(!show.notifies);
    CHECK(show.serial != arriving);
    CHECK(show.next == doctest::Approx(4.0));
    REQUIRE(readAll(show.picture, &texels) == PrStatus_Ok);
    CHECK(texels[0] == 0);
    CHECK(texels[1] == 255);
    prTextureSourceRelease(show.picture);
    prBlobFeedDestroy(feed);
}

TEST_CASE("a catalog opens a resource whose bytes are arriving once its header arrives") {
    Formats formats;
    Array<uint8_t> file = formats.noisePng({16, 16}, false);
    PrTextureCatalogDesc desc = prTextureCatalogDescDefault(formats.set);
    PrTextureCatalog* catalog = prTextureCatalogCreate(&desc);
    PrBlobFeed* feed = prBlobFeedCreate(kPrBlobSizeUnknown);
    const PrTextureSource* source = nullptr;
    CHECK(prTextureCatalogSourceFromBytes(catalog, "arriving.png", prBlobFeedSource(feed), &source) ==
        PrTextureCatalogState_Pending);
    uint32_t at = 0;
    feedTo(feed, file, &at, file.count() / 2, 32);
    REQUIRE(prTextureCatalogSourceFromBytes(catalog, "arriving.png", prBlobFeedSource(feed), &source) ==
        PrTextureCatalogState_Ready);
    CHECK(prTextureSourceChanges(source));
    feedTo(feed, file, &at, file.count(), 32);
    prBlobFeedEnd(feed);
    PrTextureShow show{};
    prTextureSourceShow(source, 0, &show);
    REQUIRE(show.picture != nullptr);
    CHECK(!prTextureSourceChanges(show.picture));
    prTextureSourceRelease(show.picture);
    prTextureSourceRelease(source);
    prBlobFeedDestroy(feed);
    prTextureCatalogDestroy(catalog);
}

TEST_CASE("an ask that does not play shows an image whose bytes are arriving as Partial, and waits for no bytes") {
    Formats formats;
    Array<uint8_t> file = formats.noisePng({32, 32}, false);
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    PrTextureCache* cache = cacheOn(device);
    PrBlobFeed* feed = prBlobFeedCreate(kPrBlobSizeUnknown);
    Opened opened;
    prTextureFormatSetOpen(formats.set, prBlobFeedSource(feed), "x.png", nullptr, Opened::fn, &opened);
    uint32_t at = 0;
    feedTo(feed, file, &at, file.count() / 2, 64);
    REQUIRE(opened.done);
    PrTextureCacheAsk ask = prTextureCacheAskEncoded(0);
    ask.alpha = PrTextureAlpha_Straight;
    PrTextureRegion region{};
    CHECK(prTextureCacheRealize(cache, opened.source, ask, &region) == PrTextureCacheState_Partial);
    CHECK(!region.playing);
    CHECK(prTextureCacheInfo(cache).inFlight == 0);
    feedTo(feed, file, &at, file.count(), 64);
    prBlobFeedEnd(feed);
    CHECK(prTextureCacheRealize(cache, opened.source, ask, &region) == PrTextureCacheState_Ready);
    CHECK(firstByte(device, region) == wholeTexels(formats, file)[0]);
    prBlobFeedDestroy(feed);
    prTextureCacheDestroy(cache);
    prGpuDeviceDestroy(device);
}

TEST_CASE("an image decodes as its bytes arrive in pieces of any size, with work proportional to the file") {
    Formats formats;
    PrBlob* jpeg = prResourceOpen("stream/progressive.jpg");
    PrBlob* baseline = prResourceOpen("stream/baseline.jpg");
    REQUIRE(jpeg != nullptr);
    REQUIRE(baseline != nullptr);
    Array<uint8_t> files[5];
    files[0] = formats.noisePng({48, 32}, false);
    files[1] = formats.noisePng({48, 32}, true);
    files[2].append(prBlobBytes(jpeg));
    files[3].append(prBlobBytes(baseline));
    files[4] = animatedGif(6, 10, true);
    prBlobRelease(jpeg);
    prBlobRelease(baseline);
    for (const Array<uint8_t>& file : files) {
        PrImageDecoder* whole = prImageDecoderCreate(formats.images);
        PrImageSource source{};
        source.memory = PrByteView{file.data(), file.count()};
        REQUIRE(prImageDecoderOpen(whole, source, nullptr) == PrStatus_Ok);
        const PrUVec2 extent = prImageDecoderInfo(whole)->extent;
        const uint32_t frames = prImageDecoderInfo(whole)->frameCount;
        Array<uint8_t> expected(extent.x * extent.y * 4);
        Array<uint8_t> got(expected.count());
        const uint32_t kPieces[3] = {1, 6, 4096};
        for (uint32_t piece : kPieces) {
            CAPTURE(piece);
            PrImageDecoder* decoder = prImageDecoderCreate(formats.images);
            REQUIRE(prImageDecoderOpenStream(decoder, nullptr) == PrStatus_Ok);
            PrStatus status = PrStatus_Truncated;
            for (uint32_t at = 0; at < file.count(); at += piece) {
                uint32_t size = file.count() - at < piece ? file.count() - at : piece;
                status = prImageDecoderAppend(decoder, PrByteView{file.data() + at, size}, at + size == file.count());
                REQUIRE((status == PrStatus_Ok || status == PrStatus_Truncated));
            }
            CHECK(status == PrStatus_Ok);
            REQUIRE(prImageDecoderInfo(decoder)->frameCount == frames);
            for (uint32_t frame = 0; frame < frames; ++frame) {
                REQUIRE(prImageDecoderReadRgba8(whole, frame, PrImageAlpha_Straight, PrImageOrientation_Identity,
                    PrPixels{expected.data(), extent.x * 4, extent}) == PrStatus_Ok);
                REQUIRE(prImageDecoderReadRgba8(decoder, frame, PrImageAlpha_Straight, PrImageOrientation_Identity,
                    PrPixels{got.data(), extent.x * 4, extent}) == PrStatus_Ok);
                CHECK(got.span() == expected.span());
            }
            // The work is a few times the bytes and the samples, whatever the pieces: no piece reads again what the
            // pieces before it read.
            CHECK(prImageDecoderWork(decoder) <= uint64_t(file.count()) * 16 + uint64_t(expected.count()) * 4);
            prImageDecoderDestroy(decoder);
        }
        prImageDecoderDestroy(whole);
    }
}

TEST_CASE("an animated PNG that is still arriving plays the frames that are whole, and later frames appear") {
    Formats formats;
    Array<uint8_t> file = animatedPng(formats, 3);
    PrBlobFeed* feed = prBlobFeedCreate(kPrBlobSizeUnknown);
    Opened opened;
    prTextureFormatSetOpen(formats.set, prBlobFeedSource(feed), "x.png", nullptr, Opened::fn, &opened);
    // The bytes through the third frame's control chunk and a few after it: two frames are whole.
    uint32_t controls = 0;
    uint32_t third = 0;
    for (uint32_t i = 0; i + 4 <= file.count() && third == 0; ++i) {
        if (prMemEquals(file.data() + i, "fcTL", 4) && ++controls == 3)
            third = i + 40;
    }
    REQUIRE(third != 0);
    uint32_t at = 0;
    feedTo(feed, file, &at, third, 7);
    REQUIRE(opened.done);
    REQUIRE(opened.status == PrStatus_Ok);
    PrTextureShow show{};
    Array<uint8_t> texels;
    // The second frame shows half a second in, complete, and the animation waits after it.
    prTextureSourceShow(opened.source, 0.6, &show);
    CHECK(show.complete);
    CHECK(show.notifies);
    REQUIRE(readAll(show.picture, &texels) == PrStatus_Ok);
    CHECK(texels[0] == 80);
    prTextureSourceRelease(show.picture);
    prTextureSourceShow(opened.source, 5, &show);
    CHECK(show.next == kPrF64Infinity);
    prTextureSourceRelease(show.picture);
    // The rest arrives: the third frame plays, and the animation loops.
    feedTo(feed, file, &at, file.count(), 7);
    prBlobFeedEnd(feed);
    prTextureSourceShow(opened.source, 1.2, &show);
    CHECK(!show.notifies);
    REQUIRE(readAll(show.picture, &texels) == PrStatus_Ok);
    CHECK(texels[0] == 120);
    prTextureSourceRelease(show.picture);
    prTextureSourceShow(opened.source, 1.6, &show);
    REQUIRE(readAll(show.picture, &texels) == PrStatus_Ok);
    CHECK(texels[0] == 40);
    prTextureSourceRelease(show.picture);
    prBlobFeedDestroy(feed);
}

TEST_CASE("a baseline JPEG fed in pieces shows the rows of blocks that have arrived") {
    Formats formats;
    PrBlob* blob = prResourceOpen("stream/baseline.jpg");
    REQUIRE(blob != nullptr);
    Array<uint8_t> file;
    file.append(prBlobBytes(blob));
    prBlobRelease(blob);
    Array<Revision> revisions;
    const PrTextureSource* source = streamed(formats, file, 16, &revisions);
    REQUIRE(source != nullptr);
    Array<uint8_t> whole = wholeTexels(formats, file);
    const uint32_t width = prTextureSourceDesc(source)->extent.x;
    // Each revision has the rows of the picture that the one before had, and more of them.
    REQUIRE(revisions.count() >= 3);
    uint32_t matching = 0;
    for (const Revision& revision : revisions) {
        uint32_t rows = 0;
        while (rows * width * 4 < whole.count() &&
                prMemEquals(revision.texels.data() + rows * width * 4, whole.data() + rows * width * 4, width * 4))
            ++rows;
        CHECK(rows >= matching);
        matching = rows;
    }
    CHECK(matching > 0);
    Array<uint8_t> final;
    REQUIRE(readAll(source, &final) == PrStatus_Ok);
    CHECK(final.span() == whole.span());
    prTextureSourceRelease(source);
}

TEST_SUITE_END();

TEST_CASE("two timelines play the whole frames of an arriving gif, each from frames read with the stream's decoders") {
    Formats formats;
    const uint32_t kFrames = 12;
    Array<uint8_t> file = animatedGif(kFrames, 10, true);
    // The bytes through the ninth frame's start: eight frames are whole.
    uint32_t starts = 0;
    uint32_t ninth = 0;
    for (uint32_t i = 0; i + 1 < file.count() && ninth == 0; ++i) {
        if (file[i] == 0x21 && file[i + 1] == 0xF9 && ++starts == 9)
            ninth = i + 24;
    }
    REQUIRE(ninth != 0);
    PrBlobFeed* feed = prBlobFeedCreate(kPrBlobSizeUnknown);
    Opened opened;
    prTextureFormatSetOpen(formats.set, prBlobFeedSource(feed), "x.gif", nullptr, Opened::fn, &opened);
    uint32_t at = 0;
    feedTo(feed, file, &at, ninth, 5);
    REQUIRE(opened.done);
    REQUIRE(opened.status == PrStatus_Ok);
    // The colors of the frames, in order: red, green, blue and yellow, then again.
    const uint8_t kColors[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}};
    Array<uint8_t> texels;
    // One timeline is three frames behind the other, and their reads take turns. Each frame is whole and still.
    for (uint32_t step = 0; step < 8; ++step) {
        const uint32_t frames[2] = {step, step >= 3 ? step - 3 : 0};
        for (uint32_t frame : frames) {
            CAPTURE(frame);
            PrTextureShow show{};
            prTextureSourceShow(opened.source, (frame + 0.5) * 0.1, &show);
            CHECK(show.complete);
            REQUIRE(show.picture != nullptr);
            CHECK(!prTextureSourceChanges(show.picture));
            REQUIRE(readAll(show.picture, &texels) == PrStatus_Ok);
            CHECK(prMemEquals(texels.data(), kColors[frame % 4], 3));
            prTextureSourceRelease(show.picture);
        }
    }
    feedTo(feed, file, &at, file.count(), 5);
    prBlobFeedEnd(feed);
    prBlobFeedDestroy(feed);
}

TEST_CASE("an arriving JPEG shows its embedded thumbnail once the thumbnail's bytes are in, before its image data") {
    Formats formats;
    // An image of 17 by 16 whose Exif data embeds a thumbnail of 8 by 8, which stands for it.
    Array<uint8_t> file = withThumbnail(PrByteView{kRgb420RestartEveryMcuBytes, sizeof(kRgb420RestartEveryMcuBytes)},
        PrByteView{kRgb444BaselineBytes, sizeof(kRgb444BaselineBytes)});
    // A comment before the scan stands for the rest of a large file's header, so that the bytes before the image data
    // take several of the tries that an open of arriving bytes makes as they double. The image's scan follows the APP1
    // segment, which holds the thumbnail's own scan.
    uint32_t scan = 4 + (uint32_t(file[4]) << 8 | file[5]);
    while (!(file[scan] == 0xFF && file[scan + 1] == 0xDA))
        ++scan;
    Array<uint8_t> padded;
    padded.append(file.data(), scan);
    const uint32_t kComment = 6000;
    const uint8_t marker[4] = {0xFF, 0xFE, uint8_t((kComment + 2) >> 8), uint8_t(kComment + 2)};
    padded.append(marker, 4);
    padded.resize(padded.count() + kComment);
    padded.append(file.data() + scan, file.count() - scan);
    file = static_cast<Array<uint8_t>&&>(padded);
    scan += 4 + kComment;
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    PrTextureCache* cache = cacheOn(device);
    PrBlobFeed* feed = prBlobFeedCreate(kPrBlobSizeUnknown);
    Opened opened;
    prTextureFormatSetOpen(formats.set, prBlobFeedSource(feed), "x.jpg", nullptr, Opened::fn, &opened);
    // The bytes arrive one at a time. The image opens once its header has arrived, which is after the thumbnail.
    uint32_t at = 0;
    while (!opened.done && at < scan)
        feedTo(feed, file, &at, at + 1, 1);
    REQUIRE(opened.done);
    REQUIRE(opened.status == PrStatus_Ok);
    CHECK(at <= scan);
    // The thumbnail is the source's one preview: the decodes at a quarter and an eighth need the image data.
    REQUIRE(prTextureSourcePreviewCount(opened.source) == 1);
    CHECK(prTextureSourceDesc(prTextureSourcePreview(opened.source, 0))->extent == PrUVec2{8, 8});
    // An ask for 8 texels reads the thumbnail at once, whole. No realization waits for the image data.
    PrTextureCacheAsk small = prTextureCacheAskEncoded(0);
    small.detail = 8;
    PrTextureRegion region{};
    CHECK(prTextureCacheRealize(cache, opened.source, small, &region) == PrTextureCacheState_Ready);
    CHECK(region.extent == PrUVec2{8, 8});
    CHECK(prTextureCacheInfo(cache).inFlight == 0);
    Array<uint8_t> texels;
    REQUIRE(readAll(prTextureSourcePreview(opened.source, 0), &texels) == PrStatus_Ok);
    // An ask for the whole image shows what has arrived of it meanwhile.
    CHECK(prTextureCacheRealize(cache, opened.source, prTextureCacheAskEncoded(0), &region) ==
        PrTextureCacheState_Partial);
    CHECK(region.extent == PrUVec2{17, 16});
    feedTo(feed, file, &at, file.count(), 7);
    prBlobFeedEnd(feed);
    CHECK(prTextureCacheRealize(cache, opened.source, prTextureCacheAskEncoded(0), &region) ==
        PrTextureCacheState_Ready);
    prBlobFeedDestroy(feed);
    prTextureCacheDestroy(cache);
    prGpuDeviceDestroy(device);
}

TEST_CASE("a JPEG that arrived in pieces offers the previews of the whole file once its bytes are complete") {
    Formats formats;
    PrBlob* blob = prResourceOpen("stream/progressive.jpg");
    REQUIRE(blob != nullptr);
    Array<uint8_t> file;
    file.append(prBlobBytes(blob));
    prBlobRelease(blob);
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    PrTextureCache* cache = cacheOn(device);
    PrBlobFeed* feed = prBlobFeedCreate(kPrBlobSizeUnknown);
    Opened opened;
    prTextureFormatSetOpen(formats.set, prBlobFeedSource(feed), "x.jpg", nullptr, Opened::fn, &opened);
    uint32_t at = 0;
    feedTo(feed, file, &at, file.count() / 2, 256);
    REQUIRE(opened.done);
    REQUIRE(opened.status == PrStatus_Ok);
    const PrUVec2 extent = prTextureSourceDesc(opened.source)->extent;
    REQUIRE(extent == PrUVec2{200, 150});
    // While the bytes arrive, a small ask shows what has arrived of the whole image.
    PrTextureCacheAsk small = prTextureCacheAskEncoded(0);
    small.detail = 25;
    PrTextureRegion region{};
    CHECK(prTextureCacheRealize(cache, opened.source, small, &region) == PrTextureCacheState_Partial);
    CHECK(region.extent == extent);
    feedTo(feed, file, &at, file.count(), 256);
    prBlobFeedEnd(feed);
    // Once they are complete, the small ask reads the decode at an eighth, as it does of the file opened whole.
    CHECK(prTextureCacheRealize(cache, opened.source, small, &region) == PrTextureCacheState_Ready);
    CHECK(region.extent == PrUVec2{25, 19});
    const PrTextureSource* whole = nullptr;
    REQUIRE(textureOpenAwait(formats.set, prBlobFeedSource(feed), "x.jpg", nullptr, &whole) == PrStatus_Ok);
    PrTextureShow show{};
    prTextureSourceShow(opened.source, 0, &show);
    CHECK(prTextureSourcePreviewCount(show.picture) == prTextureSourcePreviewCount(whole));
    CHECK(prTextureSourcePreviewCount(whole) == 2);
    prTextureSourceRelease(show.picture);
    prTextureSourceRelease(whole);
    prBlobFeedDestroy(feed);
    prTextureCacheDestroy(cache);
    prGpuDeviceDestroy(device);
}

TEST_CASE("an arriving PSD shows its thumbnail once the image resources have arrived, before the merged image") {
    Formats formats;
    // A 128 by 128 RGB document of noise, stored raw, whose image resources hold an 8 by 8 JPEG thumbnail.
    const uint32_t side = 128;
    Array<uint8_t> planes;
    uint32_t seed = 5;
    for (uint32_t i = 0; i < side * side * 3; ++i) {
        seed = seed * 1103515245u + 12345u;
        planes.push(uint8_t(seed >> 16));
    }
    Array<uint8_t> thumbnail;
    psdPut32(&thumbnail, 1);
    for (uint32_t i = 0; i < 6; ++i)
        psdPut32(&thumbnail, 0);
    thumbnail.append(PrByteView{kRgb444BaselineBytes, sizeof(kRgb444BaselineBytes)});
    Array<uint8_t> document = psdFile(PsdLayout{.width = side, .height = side}, {}, psdResource(0x040c, thumbnail), planes);
    // The layer and mask information, which a document of layers holds between its resources and its merged image, is
    // 2000 bytes here: its layer information is empty, and the rest is the masks' and extra data, which the reader
    // skips. The section's length is the four bytes before the compression of the merged image.
    const uint32_t layers = document.count() - planes.count() - 2 - 4;
    Array<uint8_t> file;
    file.append(document.data(), layers);
    psdPut32(&file, 2000);
    file.resize(file.count() + 2000);
    file.append(document.data() + layers + 4, document.count() - layers - 4);
    // The merged image's data is the last of the file.
    const uint32_t data = file.count() - planes.count();
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    PrTextureCache* cache = cacheOn(device);
    PrBlobFeed* feed = prBlobFeedCreate(kPrBlobSizeUnknown);
    Opened opened;
    prTextureFormatSetOpen(formats.set, prBlobFeedSource(feed), "x.psd", nullptr, Opened::fn, &opened);
    uint32_t at = 0;
    while (!opened.done && at < file.count())
        feedTo(feed, file, &at, at + 64 < file.count() ? at + 64 : file.count(), 64);
    REQUIRE(opened.done);
    REQUIRE(opened.status == PrStatus_Ok);
    CHECK(at <= data);
    REQUIRE(prTextureSourcePreviewCount(opened.source) == 1);
    PrTextureCacheAsk small = prTextureCacheAskEncoded(0);
    small.detail = 8;
    PrTextureRegion region{};
    CHECK(prTextureCacheRealize(cache, opened.source, small, &region) == PrTextureCacheState_Ready);
    CHECK(region.extent == PrUVec2{8, 8});
    feedTo(feed, file, &at, file.count(), 512);
    prBlobFeedEnd(feed);
    CHECK(prTextureCacheRealize(cache, opened.source, prTextureCacheAskEncoded(0), &region) ==
        PrTextureCacheState_Ready);
    CHECK(region.extent == PrUVec2{side, side});
    prBlobFeedDestroy(feed);
    prTextureCacheDestroy(cache);
    prGpuDeviceDestroy(device);
}

TEST_CASE("an arriving baseline JPEG shows exactly the rows of blocks that have arrived, and mid gray below them") {
    Formats formats;
    PrBlob* blob = prResourceOpen("stream/baseline.jpg");
    REQUIRE(blob != nullptr);
    Array<uint8_t> file;
    file.append(prBlobBytes(blob));
    prBlobRelease(blob);
    // The height of a row of MCUs: eight rows for each vertical sample of the most sampled component (T.81 A.2).
    uint32_t mcuHeight = 0;
    for (uint32_t i = 0; i + 1 < file.count() && mcuHeight == 0; ++i) {
        if (file[i] != 0xFF || file[i + 1] != 0xC0)
            continue;
        const uint32_t components = file[i + 9];
        for (uint32_t c = 0; c < components; ++c)
            mcuHeight = max(mcuHeight, uint32_t(file[i + 11 + c * 3] & 15) * 8);
    }
    REQUIRE(mcuHeight != 0);
    PrImageDecoder* whole = prImageDecoderCreate(formats.images);
    PrImageSource source{};
    source.memory = PrByteView{file.data(), file.count()};
    REQUIRE(prImageDecoderOpen(whole, source, nullptr) == PrStatus_Ok);
    const PrUVec2 extent = prImageDecoderInfo(whole)->extent;
    Array<uint8_t> final(extent.x * extent.y * 4);
    REQUIRE(prImageDecoderReadRgba8(whole, 0, PrImageAlpha_Straight, PrImageOrientation_Identity,
        PrPixels{final.data(), extent.x * 4, extent}) == PrStatus_Ok);
    prImageDecoderDestroy(whole);
    const uint32_t mcuRows = (extent.y + mcuHeight - 1) / mcuHeight;

    PrImageDecoder* decoder = prImageDecoderCreate(formats.images);
    REQUIRE(prImageDecoderOpenStream(decoder, nullptr) == PrStatus_Ok);
    Array<uint8_t> got(final.count());
    const size_t row = size_t(extent.x) * 4;
    // At each read, some number of rows of MCUs `whole` are the final decode's, and the rows of MCUs after the one that
    // is arriving are the mid gray of blocks without coefficients. The row of pixels on each side of the boundary of a
    // row of MCUs is upsampled from the chroma of the row beside it, so it is left out of either check. The number only
    // grows.
    uint32_t done = 0;
    uint32_t reads = 0;
    for (uint32_t at = 0; at < file.count(); at += 5) {
        const uint32_t size = file.count() - at < 5 ? file.count() - at : 5;
        const bool end = at + size == file.count();
        prImageDecoderAppend(decoder, PrByteView{file.data() + at, size}, end);
        if (prImageDecoderReadRgba8(decoder, 0, PrImageAlpha_Straight, PrImageOrientation_Identity,
                PrPixels{got.data(), row, extent}) != PrStatus_Truncated)
            continue;
        CAPTURE(at);
        ++reads;
        bool matched = false;
        for (; done <= mcuRows; ++done) {
            const uint32_t exactTo = done * mcuHeight > 1 ? min(done * mcuHeight - 1, extent.y) : 0;
            const uint32_t grayFrom = min((done + 1) * mcuHeight + 1, extent.y);
            bool same = prMemEquals(got.data(), final.data(), exactTo * row);
            for (uint32_t y = grayFrom; y < extent.y && same; ++y) {
                for (uint32_t x = 0; x < extent.x && same; ++x) {
                    const uint8_t gray[4] = {128, 128, 128, 255};
                    same = prMemEquals(got.data() + y * row + x * 4, gray, 4);
                }
            }
            matched = same;
            if (matched)
                break;
        }
        REQUIRE(matched);
    }
    CHECK(reads > 20);
    CHECK(done + 1 >= mcuRows);
    REQUIRE(prImageDecoderReadRgba8(decoder, 0, PrImageAlpha_Straight, PrImageOrientation_Identity,
        PrPixels{got.data(), row, extent}) == PrStatus_Ok);
    CHECK(prMemEquals(got.data(), final.data(), final.count()));
    prImageDecoderDestroy(decoder);
}

TEST_CASE("the arriving frame of a gif shows exactly its pixels that have arrived, over the frame before it") {
    Formats formats;
    Array<uint8_t> file = animatedGif(4, 10, true);
    PrImageDecoder* whole = prImageDecoderCreate(formats.images);
    PrImageSource source{};
    source.memory = PrByteView{file.data(), file.count()};
    REQUIRE(prImageDecoderOpen(whole, source, nullptr) == PrStatus_Ok);
    const PrUVec2 extent = prImageDecoderInfo(whole)->extent;
    const uint32_t count = extent.x * extent.y;
    Array<uint8_t> frames[4];
    for (uint32_t f = 0; f < 4; ++f) {
        frames[f].resize(count * 4);
        REQUIRE(prImageDecoderReadRgba8(whole, f, PrImageAlpha_Straight, PrImageOrientation_Identity,
            PrPixels{frames[f].data(), extent.x * 4, extent}) == PrStatus_Ok);
    }
    prImageDecoderDestroy(whole);
    PrImageDecoder* decoder = prImageDecoderCreate(formats.images);
    REQUIRE(prImageDecoderOpenStream(decoder, nullptr) == PrStatus_Ok);
    Array<uint8_t> got(count * 4);
    const Array<uint8_t> none(count * 4);
    uint32_t arrived[4] = {};
    for (uint32_t at = 0; at < file.count(); ++at) {
        prImageDecoderAppend(decoder, PrByteView{file.data() + at, 1}, at + 1 == file.count());
        const PrImageInfo& info = *prImageDecoderInfo(decoder);
        if (info.extent.x == 0)
            continue;
        const uint32_t frame = info.frameCount - 1;
        if (prImageDecoderReadRgba8(decoder, frame, PrImageAlpha_Straight, PrImageOrientation_Identity,
                PrPixels{got.data(), extent.x * 4, extent}) != PrStatus_Truncated)
            continue;
        CAPTURE(at);
        CAPTURE(frame);
        // The first pixels of the frame, in its order, are the frame's, and the rest are the frame before it.
        const Array<uint8_t>& below = frame == 0 ? none : frames[frame - 1];
        bool matched = false;
        for (; arrived[frame] <= count; ++arrived[frame]) {
            const uint32_t n = arrived[frame];
            matched = prMemEquals(got.data(), frames[frame].data(), n * 4) &&
                prMemEquals(got.data() + n * 4, below.data() + n * 4, (count - n) * 4);
            if (matched)
                break;
        }
        REQUIRE(matched);
    }
    prImageDecoderDestroy(decoder);
}
