// OpenEXR images.

#include "pr/exr.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/color_space.h"
#include "pr/float16.h"
#include "pr/mem.h"
#include "pr/raw_array.h"
#include "pr/sort.h"
#include "pr/string_view.h"

#include "exr/src/exr_compress.h"

namespace pr_impl {

// The flags of the version field: a single part that is tiled, names longer than 31 bytes, parts of deep data, and
// several parts. The file format version is the low byte.
constexpr uint32_t kFlagTiled = 0x200;
constexpr uint32_t kFlagLongNames = 0x400;
constexpr uint32_t kFlagDeep = 0x800;
constexpr uint32_t kFlagMultipart = 0x1000;
constexpr uint32_t kFileVersion = 2;

// The longest name of an attribute, its type or a channel.
constexpr uint32_t kMaxName = 255;

// The most channels that a part has.
constexpr uint32_t kMaxChannels = 32767;

struct ExrPartChannel {
    PrStringView name;
    ExrPixel pixel;
    bool linear;
    uint32_t xSampling;
    uint32_t ySampling;
};

// What the header of a part says, once validated. Its channels and attributes are ranges of the image's.
struct ExrPart {
    uint32_t firstChannel;
    uint32_t channelCount;
    uint32_t firstAttribute;
    uint32_t attributeCount;
    PrExrBox dataWindow;
    PrExrBox displayWindow;
    ExrScheme compression;
    bool tiled;
    PrExrLevels levels;
    bool roundUp;                   // the levels' extents round up rather than down
    uint32_t tileWidth;
    uint32_t tileHeight;
    uint32_t levelsX;               // the levels across, and down, of a rip map; equal for a mip chain
    uint32_t levelsY;
    uint64_t chunkCount;
    uint64_t tableOffset;           // where the part's offset table starts in the file
    bool hasChromaticities;
    PrColorChromaticities chromaticities;
    PrStatus support;               // NotSupported for a part that the decoder does not read
};

// How the selected part's channels become the pixels of the image's format.
enum {
    ExrColor_Rgb,                   // R, G and B, any missing ones zero
    ExrColor_Luminance,             // Y alone
    ExrColor_Chroma,                // Y, RY and BY, converted to RGB
    ExrColor_First,                 // the part's first channel, as gray
};
typedef uint8_t ExrColor;

// How the samples of a channel are written to a pixel: where in the pixel, and in what form.
enum {
    ExrWrite_Copy16,            // a half as a half
    ExrWrite_Copy32,            // a float as a float, or a uint as a uint
    ExrWrite_HalfToFloat,
    ExrWrite_UintToFloat,
    ExrWrite_FloatToHalf,
    ExrWrite_UintToHalf,
};
typedef uint8_t ExrWrite;

struct ExrTarget {
    int32_t offset;             // the bytes into a pixel, or -1 for a channel that is not written
    ExrWrite write;
};

struct ExrImage {
    PrByteView bytes;
    PrExrLimits limits;
    uint64_t held;
    bool multipart;
    RawArray<ExrPart> parts;
    RawArray<ExrPartChannel> channels;
    RawArray<PrExrAttribute> attributes;

    // The selected part: the offsets of its chunks, zero for a chunk that the file does not hold, and how its pixels
    // map to the image's format.
    uint32_t part;
    RawArray<uint64_t> offsets;
    PrStatus status;
    PrExrPartInfo info;
    ExrColor color;
    int32_t slots[4];               // the part's channel for each sample of a pixel of the format, or -1
    float lumaWeights[3];           // the luminance of red, green and blue, which chroma converts back by

    ExrCodec codec;
    RawArray<uint8_t> unpacked;
    RawArray<ExrBlockChannel> blockChannels;
    RawArray<ExrTarget> targets;    // by channel of the part
};

namespace {

uint32_t readU32(const uint8_t* bytes) {
    return prMemReadLeU32(bytes);
}

int32_t readI32(const uint8_t* bytes) {
    return int32_t(prMemReadLeU32(bytes));
}

float readF32(const uint8_t* bytes) {
    uint32_t bits = prMemReadLeU32(bytes);
    float value = 0;
    prMemCopy(&value, &bits, sizeof(value));
    return value;
}

uint64_t boxWidth(const PrExrBox& box) {
    return uint64_t(int64_t(box.xMax) - int64_t(box.xMin) + 1);
}

uint64_t boxHeight(const PrExrBox& box) {
    return uint64_t(int64_t(box.yMax) - int64_t(box.yMin) + 1);
}

// Reads a name that ends with a zero byte, of 1 to kMaxName bytes, at `*at` of `bytes`, and moves past it. Returns
// false if no zero ends it within the bytes or the limit.
bool readName(PrByteView bytes, size_t* at, PrStringView* out) {
    size_t start = *at;
    for (size_t i = start; i < bytes.size && i - start <= kMaxName; ++i) {
        if (bytes.data[i] != 0)
            continue;
        if (i == start)
            return false;
        *out = PrStringView{reinterpret_cast<const char*>(bytes.data) + start, i - start};
        *at = i + 1;
        return true;
    }
    return false;
}

// Returns floor(log2(x)) if `up` is false, and ceil(log2(x)) otherwise, for x of at least one.
uint32_t roundedLog2(uint64_t x, bool up) {
    uint32_t log = 0;
    uint64_t y = x;
    while (y > 1) {
        y >>= 1;
        ++log;
    }
    if (up && (uint64_t(1) << log) < x)
        ++log;
    return log;
}

// Returns the extent of a level `level` of a side of `full` pixels.
uint32_t levelSide(uint64_t full, uint32_t level, bool up) {
    uint64_t side = full >> level;
    if (up && (side << level) < full)
        ++side;
    return uint32_t(side != 0 ? side : 1);
}

uint64_t ceilDivide(uint64_t a, uint64_t b) {
    return (a + b - 1) / b;
}

// The extents of level (`lx`, `ly`) of a tiled part.
uint32_t levelWidth(const ExrPart& part, uint32_t lx) {
    return levelSide(boxWidth(part.dataWindow), lx, part.roundUp);
}

uint32_t levelHeight(const ExrPart& part, uint32_t ly) {
    return levelSide(boxHeight(part.dataWindow), ly, part.roundUp);
}

// Returns the number of the part's chunks, as its windows, tiles and compression make them.
uint64_t chunksOf(const ExrPart& part) {
    if (!part.tiled)
        return ceilDivide(boxHeight(part.dataWindow), exrSchemeLines(part.compression));
    uint64_t chunks = 0;
    for (uint32_t ly = 0; ly < part.levelsY; ++ly) {
        for (uint32_t lx = 0; lx < part.levelsX; ++lx) {
            if (part.levels == PrExrLevels_Mip && lx != ly)
                continue;
            chunks += ceilDivide(levelWidth(part, lx), part.tileWidth) *
                ceilDivide(levelHeight(part, ly), part.tileHeight);
        }
    }
    return chunks;
}

// Returns the number of lines from `y` through `y + height - 1` that a channel of `sampling` has samples on.
uint32_t sampledLines(int32_t y, uint32_t height, uint32_t sampling) {
    if (sampling == 1)
        return height;
    int64_t s = sampling;
    int64_t first = int64_t(y);
    int64_t last = first + int64_t(height) - 1;
    // The multiples of the sampling from first through last, by floor division.
    int64_t high = last >= 0 ? last / s : -((-last + s - 1) / s);
    int64_t low = first >= 0 ? (first + s - 1) / s : -((-first) / s);
    return high >= low ? uint32_t(high - low + 1) : 0;
}

// Reads the attribute `name` of `type` and the value that the parser knows, into `part`. Returns InvalidData for a
// known attribute of another type or size, and Ok for any other attribute, which the part keeps as it is.
PrStatus readKnown(const PrExrAttribute& attribute, ExrImage& image, ExrPart& part, uint32_t* seen) {
    PrStringView name = attribute.name;
    PrStringView type = attribute.type;
    PrByteView v = attribute.value;
    // A bit for each attribute that the parser reads, in the order below.
    enum {
        Seen_Channels = 1u << 0,
        Seen_Compression = 1u << 1,
        Seen_DataWindow = 1u << 2,
        Seen_DisplayWindow = 1u << 3,
        Seen_Tiles = 1u << 4,
        Seen_ChunkCount = 1u << 5,
        Seen_Type = 1u << 6,
    };
    if (name == "channels") {
        if (type != "chlist")
            return PrStatus_InvalidData;
        part.firstChannel = image.channels.count;
        size_t at = 0;
        while (at < v.size && v.data[at] != 0) {
            ExrPartChannel channel{};
            if (!readName(v, &at, &channel.name) || v.size - at < 16)
                return PrStatus_InvalidData;
            uint32_t pixel = readU32(v.data + at);
            uint8_t linear = v.data[at + 4];
            int32_t xSampling = readI32(v.data + at + 8);
            int32_t ySampling = readI32(v.data + at + 12);
            at += 16;
            if (pixel >= ExrPixel_Count || linear > 1 || xSampling < 1 || ySampling < 1)
                return PrStatus_InvalidData;
            channel.pixel = ExrPixel(pixel);
            channel.linear = linear != 0;
            channel.xSampling = uint32_t(xSampling);
            channel.ySampling = uint32_t(ySampling);
            if (image.channels.count - part.firstChannel >= kMaxChannels)
                return PrStatus_InvalidData;
            image.channels.push(channel);
        }
        if (at + 1 != v.size)
            return PrStatus_InvalidData;
        part.channelCount = image.channels.count - part.firstChannel;
        *seen |= Seen_Channels;
    } else if (name == "compression") {
        if (type != "compression" || v.size != 1)
            return PrStatus_InvalidData;
        part.compression = v.data[0];
        *seen |= Seen_Compression;
    } else if (name == "dataWindow" || name == "displayWindow") {
        if (type != "box2i" || v.size != 16)
            return PrStatus_InvalidData;
        PrExrBox box{readI32(v.data), readI32(v.data + 4), readI32(v.data + 8), readI32(v.data + 12)};
        if (box.xMax < box.xMin || box.yMax < box.yMin)
            return PrStatus_InvalidData;
        if (name == "dataWindow") {
            part.dataWindow = box;
            *seen |= Seen_DataWindow;
        } else {
            part.displayWindow = box;
            *seen |= Seen_DisplayWindow;
        }
    } else if (name == "lineOrder") {
        if (type != "lineOrder" || v.size != 1 || v.data[0] > 2)
            return PrStatus_InvalidData;
    } else if (name == "pixelAspectRatio" || name == "screenWindowWidth") {
        if (type != "float" || v.size != 4)
            return PrStatus_InvalidData;
    } else if (name == "screenWindowCenter") {
        if (type != "v2f" || v.size != 8)
            return PrStatus_InvalidData;
    } else if (name == "tiles") {
        if (type != "tiledesc" || v.size != 9)
            return PrStatus_InvalidData;
        part.tileWidth = readU32(v.data);
        part.tileHeight = readU32(v.data + 4);
        uint8_t mode = v.data[8];
        if (part.tileWidth == 0 || part.tileHeight == 0 || part.tileWidth > 0x1fffffff ||
                part.tileHeight > 0x1fffffff || (mode & 15) > PrExrLevels_Rip || (mode >> 4) > 1)
            return PrStatus_InvalidData;
        part.levels = PrExrLevels(mode & 15);
        part.roundUp = (mode >> 4) == 1;
        *seen |= Seen_Tiles;
    } else if (name == "chromaticities") {
        if (type != "chromaticities" || v.size != 32)
            return PrStatus_InvalidData;
        part.chromaticities = PrColorChromaticities{
            .red = {readF32(v.data), readF32(v.data + 4)},
            .green = {readF32(v.data + 8), readF32(v.data + 12)},
            .blue = {readF32(v.data + 16), readF32(v.data + 20)},
            .white = {readF32(v.data + 24), readF32(v.data + 28)},
        };
        PrDMatrix3 toXyz{};
        part.hasChromaticities = prColorChromaticitiesToXyz(&part.chromaticities, &toXyz);
    } else if (name == "chunkCount") {
        if (type != "int" || v.size != 4 || readI32(v.data) < 0)
            return PrStatus_InvalidData;
        part.chunkCount = uint32_t(readI32(v.data));
        *seen |= Seen_ChunkCount;
    } else if (name == "type") {
        if (type != "string")
            return PrStatus_InvalidData;
        PrStringView kind{reinterpret_cast<const char*>(v.data), v.size};
        if (kind == "deepscanline" || kind == "deeptile")
            part.support = PrStatus_NotSupported;
        else if (kind == "tiledimage")
            part.tiled = true;
        else if (kind != "scanlineimage")
            part.support = PrStatus_NotSupported;
        *seen |= Seen_Type;
    }
    return PrStatus_Ok;
}

// Reads the header of a part at `*at`, up to and past the zero byte that ends it, and validates it. Returns InvalidData
// for a header that breaks the file format, and Limit for one past the limits.
PrStatus readHeader(ExrImage& image, uint32_t flags, size_t* at, uint64_t* metadata, ExrPart* out) {
    PrByteView bytes = image.bytes;
    ExrPart part{.levelsX = 1, .levelsY = 1, .support = PrStatus_Ok};
    part.tiled = (flags & kFlagTiled) != 0;
    part.firstAttribute = image.attributes.count;
    uint32_t seen = 0;
    for (;;) {
        if (*at >= bytes.size)
            return PrStatus_Truncated;
        if (bytes.data[*at] == 0) {
            ++*at;
            break;
        }
        PrExrAttribute attribute{};
        if (!readName(bytes, at, &attribute.name) || !readName(bytes, at, &attribute.type) || bytes.size - *at < 4)
            return *at >= bytes.size - 4 ? PrStatus_Truncated : PrStatus_InvalidData;
        int32_t size = readI32(bytes.data + *at);
        *at += 4;
        if (size < 0)
            return PrStatus_InvalidData;
        if (uint64_t(size) > bytes.size - *at)
            return PrStatus_Truncated;
        attribute.value = PrByteView{bytes.data + *at, size_t(size)};
        *at += size_t(size);
        *metadata += uint64_t(size) + attribute.name.size + attribute.type.size;
        if (*metadata > image.limits.metadataBytes)
            return PrStatus_Limit;
        for (uint32_t i = part.firstAttribute; i < image.attributes.count; ++i) {
            if (image.attributes.data[i].name == attribute.name)
                return PrStatus_InvalidData;
        }
        image.attributes.push(attribute);
        PrStatus status = readKnown(attribute, image, part, &seen);
        if (status != PrStatus_Ok)
            return status;
    }
    part.attributeCount = image.attributes.count - part.firstAttribute;
    // The channels, compression and windows are required. A missing line order, pixel aspect ratio or screen window has
    // its default.
    constexpr uint32_t kRequired = 0xf;
    if ((seen & kRequired) != kRequired || part.channelCount == 0)
        return PrStatus_InvalidData;
    if (part.compression >= ExrScheme_Count)
        part.support = PrStatus_NotSupported;
    if (image.multipart && (seen & (1u << 6)) == 0)
        return PrStatus_InvalidData;
    if (part.tiled && (seen & (1u << 4)) == 0)
        return PrStatus_InvalidData;
    if (!part.tiled)
        part.levels = PrExrLevels_One;

    // The channel list is in the order of the channels' names, and names no channel twice.
    for (uint32_t i = 1; i < part.channelCount; ++i) {
        PrStringView a = image.channels.data[part.firstChannel + i - 1].name;
        PrStringView b = image.channels.data[part.firstChannel + i].name;
        size_t common = a.size < b.size ? a.size : b.size;
        int order = prMemCompare(a.data, b.data, common);
        if (order > 0 || (order == 0 && a.size >= b.size))
            return PrStatus_InvalidData;
    }
    uint64_t width = boxWidth(part.dataWindow);
    uint64_t height = boxHeight(part.dataWindow);
    for (uint32_t i = 0; i < part.channelCount; ++i) {
        const ExrPartChannel& channel = image.channels.data[part.firstChannel + i];
        if (part.tiled && (channel.xSampling != 1 || channel.ySampling != 1))
            return PrStatus_InvalidData;
        if (part.dataWindow.xMin % int32_t(channel.xSampling) != 0 ||
                part.dataWindow.yMin % int32_t(channel.ySampling) != 0 || width % channel.xSampling != 0 ||
                height % channel.ySampling != 0)
            return PrStatus_InvalidData;
    }
    if (width > 0x7fffffff || height > 0x7fffffff || boxWidth(part.displayWindow) > 0x7fffffff ||
            boxHeight(part.displayWindow) > 0x7fffffff)
        return PrStatus_Limit;
    if (part.tiled && part.levels != PrExrLevels_One) {
        uint32_t levelsX = roundedLog2(width, part.roundUp) + 1;
        uint32_t levelsY = roundedLog2(height, part.roundUp) + 1;
        if (part.levels == PrExrLevels_Mip) {
            part.levelsX = levelsX > levelsY ? levelsX : levelsY;
            part.levelsY = part.levelsX;
        } else {
            part.levelsX = levelsX;
            part.levelsY = levelsY;
        }
    }
    uint64_t chunks = part.support == PrStatus_Ok ? chunksOf(part) : part.chunkCount;
    if ((seen & (1u << 5)) != 0 && part.chunkCount != chunks)
        return PrStatus_InvalidData;
    if (image.multipart && (seen & (1u << 5)) == 0)
        return PrStatus_InvalidData;
    part.chunkCount = chunks;
    *out = part;
    return PrStatus_Ok;
}

// Returns the layer and name of a channel: the name after its last dot, and what comes before.
PrStringView channelSuffix(PrStringView name) {
    for (size_t i = name.size; i > 0; --i) {
        if (name.data[i - 1] == '.')
            return PrStringView{name.data + i, name.size - i};
    }
    return name;
}

PrStringView channelLayer(PrStringView name) {
    PrStringView suffix = channelSuffix(name);
    return PrStringView{name.data, name.size - suffix.size};
}

// Returns the index within the part of the channel `name` of `layer`, or -1.
int32_t findChannel(const ExrImage& image, const ExrPart& part, PrStringView layer, PrStringView name) {
    for (uint32_t i = 0; i < part.channelCount; ++i) {
        PrStringView channel = image.channels.data[part.firstChannel + i].name;
        if (channel.size == layer.size + name.size && prMemEquals(channel.data, layer.data, layer.size) &&
                prMemEquals(channel.data + layer.size, name.data, name.size))
            return int32_t(i);
    }
    return -1;
}

bool isColorName(PrStringView name) {
    return name == "R" || name == "G" || name == "B" || name == "Y" || name == "A";
}

// Chooses how the part's channels become the image's pixels, and describes them in the image's info. The color is the
// channels without a layer's prefix, or else those of the first layer that has color, or else the first channel.
void mapChannels(ExrImage& image, const ExrPart& part) {
    PrStringView layer = "";
    bool found = false;
    for (uint32_t i = 0; i < part.channelCount && !found; ++i) {
        PrStringView name = image.channels.data[part.firstChannel + i].name;
        found = isColorName(name);
    }
    for (uint32_t i = 0; i < part.channelCount && !found; ++i) {
        PrStringView name = image.channels.data[part.firstChannel + i].name;
        if (isColorName(channelSuffix(name)) && channelLayer(name).size != 0) {
            layer = channelLayer(name);
            found = true;
        }
    }
    int32_t r = findChannel(image, part, layer, "R");
    int32_t g = findChannel(image, part, layer, "G");
    int32_t b = findChannel(image, part, layer, "B");
    int32_t y = findChannel(image, part, layer, "Y");
    int32_t ry = findChannel(image, part, layer, "RY");
    int32_t by = findChannel(image, part, layer, "BY");
    int32_t a = findChannel(image, part, layer, "A");
    for (int32_t& slot : image.slots)
        slot = -1;
    PrExrPartInfo& info = image.info;
    if (r >= 0 || g >= 0 || b >= 0) {
        image.color = ExrColor_Rgb;
        info.colors = 3;
        image.slots[0] = r;
        image.slots[1] = g;
        image.slots[2] = b;
    } else if (y >= 0 && (ry >= 0 || by >= 0)) {
        image.color = ExrColor_Chroma;
        info.colors = 3;
        image.slots[0] = y;
        image.slots[1] = ry;
        image.slots[2] = by;
    } else if (y >= 0) {
        image.color = ExrColor_Luminance;
        info.colors = 1;
        image.slots[0] = y;
    } else {
        image.color = ExrColor_First;
        info.colors = 1;
        image.slots[0] = 0;
        a = -1;
    }
    info.alpha = a >= 0;
    image.slots[info.colors] = a;
    // Halves stay halves, and any other mix of channels is floats.
    bool halves = true;
    for (uint32_t i = 0; i < 4; ++i) {
        if (image.slots[i] >= 0)
            halves = halves && image.channels.data[part.firstChannel + uint32_t(image.slots[i])].pixel == ExrPixel_Half;
    }
    info.sample = halves ? PrExrSample_Half : PrExrSample_Float;
    // A file without chromaticities has the primaries and white of Rec. ITU-R BT.709.
    PrColorChromaticities chromaticities = prColorPrimariesChromaticities(PrColorPrimaries_Rec709);
    if (part.hasChromaticities)
        chromaticities = part.chromaticities;
    info.chromaticities = chromaticities;
    PrDMatrix3 toXyz{};
    bool made = prColorChromaticitiesToXyz(&chromaticities, &toXyz);
    PR_ASSERT(made);
    for (uint32_t i = 0; i < 3; ++i)
        image.lumaWeights[i] = float(toXyz.m[1][i]);
}

// Returns the bytes of the uncompressed pixels of a chunk of `block`, and fills in each channel's samples.
uint64_t blockBytes(ExrImage& image, const ExrPart& part, int32_t y, uint32_t width, uint32_t height) {
    uint64_t bytes = 0;
    image.blockChannels.resize(part.channelCount);
    for (uint32_t c = 0; c < part.channelCount; ++c) {
        const ExrPartChannel& channel = image.channels.data[part.firstChannel + c];
        ExrBlockChannel& block = image.blockChannels.data[c];
        block.name = channel.name;
        block.pixel = channel.pixel;
        block.linear = channel.linear;
        block.ySampling = channel.ySampling;
        block.width = width / channel.xSampling;
        block.height = sampledLines(y, height, channel.ySampling);
        bytes += uint64_t(block.width) * block.height * exrPixelBytes(channel.pixel);
    }
    return bytes;
}

// The largest chunk of the part, uncompressed.
uint64_t largestChunk(const ExrPart& part, uint64_t pixelBytes) {
    uint64_t width = boxWidth(part.dataWindow);
    uint64_t lines = exrSchemeLines(part.compression);
    if (part.tiled) {
        width = part.tileWidth < width ? part.tileWidth : width;
        lines = part.tileHeight;
    }
    uint64_t height = boxHeight(part.dataWindow);
    return width * (lines < height ? lines : height) * pixelBytes;
}

// The tile of the chunk at `index` of a tiled part: its position and level.
struct ExrTile {
    uint32_t x;
    uint32_t y;
    uint32_t levelX;
    uint32_t levelY;
};

// Returns the tile whose chunk is at `index` of the part's offset table, which orders the levels, then each level's
// tiles by line and then by column.
bool tileAt(const ExrPart& part, uint64_t index, ExrTile* out) {
    for (uint32_t ly = 0; ly < part.levelsY; ++ly) {
        for (uint32_t lx = 0; lx < part.levelsX; ++lx) {
            if (part.levels == PrExrLevels_Mip && lx != ly)
                continue;
            uint64_t across = ceilDivide(levelWidth(part, lx), part.tileWidth);
            uint64_t tiles = across * ceilDivide(levelHeight(part, ly), part.tileHeight);
            if (index < tiles) {
                *out = ExrTile{uint32_t(index % across), uint32_t(index / across), lx, ly};
                return true;
            }
            index -= tiles;
        }
    }
    return false;
}

// Returns the index in the offset table of the tile, or the chunk count for a tile that the part does not have.
uint64_t tileIndex(const ExrPart& part, const ExrTile& tile) {
    uint64_t index = 0;
    for (uint32_t ly = 0; ly < part.levelsY; ++ly) {
        for (uint32_t lx = 0; lx < part.levelsX; ++lx) {
            if (part.levels == PrExrLevels_Mip && lx != ly)
                continue;
            uint64_t across = ceilDivide(levelWidth(part, lx), part.tileWidth);
            uint64_t down = ceilDivide(levelHeight(part, ly), part.tileHeight);
            if (lx == tile.levelX && ly == tile.levelY) {
                if (tile.x >= across || tile.y >= down)
                    return part.chunkCount;
                return index + uint64_t(tile.y) * across + tile.x;
            }
            index += across * down;
        }
    }
    return part.chunkCount;
}

// The bytes of a chunk's header after its part number: a scan line block's y, or a tile's coordinates and levels, and
// then the size of its data.
uint32_t chunkHeaderBytes(const ExrPart& part) {
    return part.tiled ? 20 : 8;
}

// Returns the index in the offset table of the chunk whose header is at `offset`, or the chunk count if the header is
// not that of a chunk of the part.
uint64_t chunkIndexAt(const ExrImage& image, const ExrPart& part, uint64_t offset) {
    PrByteView bytes = image.bytes;
    uint64_t at = offset;
    if (image.multipart) {
        if (bytes.size - at < 4 || readU32(bytes.data + at) != image.part)
            return part.chunkCount;
        at += 4;
    }
    if (bytes.size - at < chunkHeaderBytes(part))
        return part.chunkCount;
    if (!part.tiled) {
        int64_t y = readI32(bytes.data + at);
        int64_t lines = exrSchemeLines(part.compression);
        int64_t relative = y - part.dataWindow.yMin;
        if (relative < 0 || relative % lines != 0 || uint64_t(relative / lines) >= part.chunkCount)
            return part.chunkCount;
        return uint64_t(relative / lines);
    }
    int32_t values[4] = {
        readI32(bytes.data + at), readI32(bytes.data + at + 4), readI32(bytes.data + at + 8),
        readI32(bytes.data + at + 12),
    };
    for (int32_t value : values) {
        if (value < 0)
            return part.chunkCount;
    }
    return tileIndex(part, ExrTile{uint32_t(values[0]), uint32_t(values[1]), uint32_t(values[2]), uint32_t(values[3])});
}

// Reads the selected part's offset table, which starts at `tableOffset`, into the image's offsets. An offset that is
// zero or lies outside the chunks is missing, and becomes zero. A single part's missing offsets are found by reading
// the chunks in order from the end of the tables, as a file whose writing stopped before its table was written holds
// them. Returns Truncated if any chunk is missing. A chunk whose header is not the one that its place in the table
// gives is corrupt, which reading it reports.
PrStatus readOffsets(ExrImage& image, uint64_t tablesEnd) {
    const ExrPart& part = image.parts.data[image.part];
    PrByteView bytes = image.bytes;
    image.offsets.resize(uint32_t(part.chunkCount));
    bool missing = false;
    for (uint64_t i = 0; i < part.chunkCount; ++i) {
        uint64_t at = part.tableOffset + i * 8;
        uint64_t offset = at + 8 <= bytes.size ? prMemReadLeU64(bytes.data + at) : 0;
        if (offset < tablesEnd || offset >= bytes.size)
            offset = 0;
        image.offsets.data[i] = offset;
        missing = missing || offset == 0;
    }
    if (missing && !image.multipart) {
        uint64_t at = tablesEnd;
        while (at < bytes.size) {
            uint64_t index = chunkIndexAt(image, part, at);
            if (index >= part.chunkCount)
                break;
            uint64_t sizeAt = at + chunkHeaderBytes(part) - 4;
            int32_t size = readI32(bytes.data + sizeAt);
            if (size < 0 || uint64_t(size) > bytes.size - sizeAt - 4)
                break;
            if (image.offsets.data[index] == 0)
                image.offsets.data[index] = at;
            at = sizeAt + 4 + uint64_t(size);
        }
        missing = false;
        for (uint64_t i = 0; i < part.chunkCount; ++i)
            missing = missing || image.offsets.data[i] == 0;
    }
    return missing ? PrStatus_Truncated : PrStatus_Ok;
}

// Describes the selected part in the image's info, after checking it against the limits.
PrStatus describePart(ExrImage& image, uint64_t tablesEnd) {
    const ExrPart& part = image.parts.data[image.part];
    image.info = PrExrPartInfo{};
    if (part.support != PrStatus_Ok)
        return part.support;
    mapChannels(image, part);
    PrExrPartInfo* info = &image.info;
    uint64_t displayWidth = boxWidth(part.displayWindow);
    uint64_t displayHeight = boxHeight(part.displayWindow);
    info->extent = PrUVec2{uint32_t(displayWidth), uint32_t(displayHeight)};
    info->channelCount = part.channelCount;
    info->attributeCount = part.attributeCount;
    info->levels = part.levels;
    info->levelCount = part.levels == PrExrLevels_Mip ? part.levelsX : part.levelsX * part.levelsY;
    info->levelColumns = part.levels == PrExrLevels_Rip ? part.levelsX : 1;

    // Decoding takes the input, the largest chunk uncompressed and its scratch four times over, and the tables of
    // Huffman coding and of B44 and DWA.
    uint64_t pixelBytes = 0;
    uint64_t samples = 0;
    for (uint32_t c = 0; c < part.channelCount; ++c) {
        const ExrPartChannel& channel = image.channels.data[part.firstChannel + c];
        pixelBytes += exrPixelBytes(channel.pixel);
        samples += 1;
    }
    uint64_t dataPixels = boxWidth(part.dataWindow) * boxHeight(part.dataWindow);
    uint64_t levelFactor = part.levels == PrExrLevels_Rip ? 4 : part.levels == PrExrLevels_Mip ? 2 : 1;
    const PrExrLimits& limits = image.limits;
    if (displayWidth * displayHeight > limits.pixels || dataPixels > limits.pixels || part.channelCount > limits.work ||
            dataPixels * levelFactor > limits.work / samples || part.chunkCount > limits.work ||
            part.chunkCount > 0xffffffffu)
        return PrStatus_Limit;
    uint64_t chunk = largestChunk(part, pixelBytes);
    uint64_t fixed = sizeof(ExrImage) + part.chunkCount * 8 + 3 * 65536 * 2 + 1024 * 1024 +
        uint64_t(image.parts.count) * sizeof(ExrPart) + uint64_t(image.channels.count) * sizeof(ExrPartChannel) +
        uint64_t(image.attributes.count) * sizeof(PrExrAttribute);
    if (chunk > 0xffffffffu / 4)
        return PrStatus_Limit;
    uint64_t working = image.held + fixed + chunk * 4;
    if (working > limits.workingBytes)
        return PrStatus_Limit;
    info->workingBytes = working;
    return readOffsets(image, tablesEnd);
}

// The end of the offset tables, which the first chunk follows.
uint64_t tablesEnd(const ExrImage& image) {
    const ExrPart& last = image.parts.data[image.parts.count - 1];
    return last.tableOffset + last.chunkCount * 8;
}

// Returns the bits of the float that the half of bits `bits` is: every half, NaNs with their payload, is exactly a
// float.
uint32_t halfToFloatBits(uint16_t bits) {
    uint32_t sign = uint32_t(bits & 0x8000) << 16;
    uint32_t exponent = bits >> 10 & 31;
    uint32_t mantissa = bits & 1023;
    if (exponent == 31)
        return sign | 0x7f800000 | mantissa << 13;
    if (exponent != 0)
        return sign | (exponent + 112) << 23 | mantissa << 13;
    if (mantissa == 0)
        return sign;
    uint32_t shift = 0;
    while ((mantissa & 1024) == 0) {
        mantissa <<= 1;
        ++shift;
    }
    return sign | (113 - shift) << 23 | (mantissa & 1023) << 13;
}

float halfToFloat(uint16_t bits) {
    uint32_t value = halfToFloatBits(bits);
    float out = 0;
    prMemCopy(&out, &value, sizeof(out));
    return out;
}

uint16_t floatToHalf(float value) {
    PrFloat16 half = prFloat16FromFloat(value);
    uint16_t bits = 0;
    prMemCopy(&bits, &half, sizeof(bits));
    return bits;
}

// The pixels that a level decodes into: the output, the pixel of the output that the part's pixel (x, y) maps to, and
// the size of a pixel. The output's pixel (0, 0) is the part's (originX, originY).
struct ExrDestination {
    PrPixels output;
    int64_t originX;
    int64_t originY;
    uint32_t pixelBytes;
    const ExrTarget* targets;
};

// Writes the samples of one line of a channel, of `count` samples from the part's pixel (x, y), to the destination,
// each over the `xSampling` by `ySampling` pixels that it covers there.
void writeSamples(const ExrDestination& d, const ExrTarget& target, const uint8_t* samples, uint32_t count, int64_t x,
        int64_t y, uint32_t xSampling, uint32_t ySampling) {
    int64_t width = d.output.extent.x;
    int64_t height = d.output.extent.y;
    for (uint32_t row = 0; row < ySampling; ++row) {
        int64_t oy = y + row - d.originY;
        if (oy < 0 || oy >= height)
            continue;
        uint8_t* line = d.output.data + size_t(oy) * d.output.stride + target.offset;
        // The samples that land in the output, from the first to the last.
        int64_t first = x - d.originX;
        int64_t start = first < 0 ? (-first + xSampling - 1) / xSampling : 0;
        int64_t end = count;
        if (first + int64_t(count) * xSampling > width)
            end = (width - first + xSampling - 1) / xSampling;
        if (end > int64_t(count))
            end = count;
        if (xSampling == 1) {
            uint8_t* out = line + (first + start) * d.pixelBytes;
            uint32_t stride = d.pixelBytes;
            uint32_t n = uint32_t(end > start ? end - start : 0);
            switch (target.write) {
                case ExrWrite_Copy16: {
                    const uint8_t* in = samples + start * 2;
                    for (uint32_t i = 0; i < n; ++i)
                        prMemCopy(out + size_t(i) * stride, in + size_t(i) * 2, 2);
                    break;
                }
                case ExrWrite_Copy32: {
                    const uint8_t* in = samples + start * 4;
                    for (uint32_t i = 0; i < n; ++i)
                        prMemCopy(out + size_t(i) * stride, in + size_t(i) * 4, 4);
                    break;
                }
                case ExrWrite_HalfToFloat: {
                    const uint8_t* in = samples + start * 2;
                    for (uint32_t i = 0; i < n; ++i) {
                        uint32_t bits = halfToFloatBits(prMemReadLeU16(in + size_t(i) * 2));
                        prMemCopy(out + size_t(i) * stride, &bits, 4);
                    }
                    break;
                }
                case ExrWrite_UintToFloat: {
                    const uint8_t* in = samples + start * 4;
                    for (uint32_t i = 0; i < n; ++i) {
                        float value = float(prMemReadLeU32(in + size_t(i) * 4));
                        prMemCopy(out + size_t(i) * stride, &value, 4);
                    }
                    break;
                }
                case ExrWrite_FloatToHalf: {
                    const uint8_t* in = samples + start * 4;
                    for (uint32_t i = 0; i < n; ++i) {
                        uint16_t bits = floatToHalf(readF32(in + size_t(i) * 4));
                        prMemCopy(out + size_t(i) * stride, &bits, 2);
                    }
                    break;
                }
                case ExrWrite_UintToHalf: {
                    const uint8_t* in = samples + start * 4;
                    for (uint32_t i = 0; i < n; ++i) {
                        uint16_t bits = floatToHalf(float(prMemReadLeU32(in + size_t(i) * 4)));
                        prMemCopy(out + size_t(i) * stride, &bits, 2);
                    }
                    break;
                }
                default:
                    break;
            }
            continue;
        }
        for (int64_t i = start; i < end; ++i) {
            uint8_t value[4];
            uint32_t bytes = 4;
            switch (target.write) {
                case ExrWrite_Copy16:
                    prMemCopy(value, samples + i * 2, 2);
                    bytes = 2;
                    break;
                case ExrWrite_Copy32:
                    prMemCopy(value, samples + i * 4, 4);
                    break;
                case ExrWrite_HalfToFloat: {
                    uint32_t bits = halfToFloatBits(prMemReadLeU16(samples + i * 2));
                    prMemCopy(value, &bits, 4);
                    break;
                }
                case ExrWrite_UintToFloat: {
                    float f = float(prMemReadLeU32(samples + i * 4));
                    prMemCopy(value, &f, 4);
                    break;
                }
                case ExrWrite_FloatToHalf: {
                    uint16_t bits = floatToHalf(readF32(samples + i * 4));
                    prMemCopy(value, &bits, 2);
                    bytes = 2;
                    break;
                }
                default: {
                    uint16_t bits = floatToHalf(float(prMemReadLeU32(samples + i * 4)));
                    prMemCopy(value, &bits, 2);
                    bytes = 2;
                    break;
                }
            }
            for (uint32_t k = 0; k < xSampling; ++k) {
                int64_t ox = first + i * xSampling + k;
                if (ox >= 0 && ox < width)
                    prMemCopy(line + ox * d.pixelBytes, value, bytes);
            }
        }
    }
}

// Decodes the chunk at `index` and writes its samples to the destination. Returns Truncated for a chunk that the file
// does not hold, and InvalidData for one that is corrupt.
PrStatus decodeChunk(ExrImage& image, uint64_t index, const ExrDestination& d) {
    const ExrPart& part = image.parts.data[image.part];
    uint64_t offset = image.offsets.data[index];
    if (offset == 0)
        return PrStatus_Truncated;
    if (chunkIndexAt(image, part, offset) != index)
        return PrStatus_InvalidData;
    PrByteView bytes = image.bytes;
    uint64_t at = offset + (image.multipart ? 4 : 0);
    int32_t x = part.dataWindow.xMin;
    int32_t y = part.dataWindow.yMin;
    uint32_t width = 0;
    uint32_t height = 0;
    if (part.tiled) {
        ExrTile tile{};
        bool found = tileAt(part, index, &tile);
        PR_ASSERT(found);
        uint32_t levelW = levelWidth(part, tile.levelX);
        uint32_t levelH = levelHeight(part, tile.levelY);
        uint64_t tileX = uint64_t(tile.x) * part.tileWidth;
        uint64_t tileY = uint64_t(tile.y) * part.tileHeight;
        width = uint32_t(levelW - tileX < part.tileWidth ? levelW - tileX : part.tileWidth);
        height = uint32_t(levelH - tileY < part.tileHeight ? levelH - tileY : part.tileHeight);
        x = int32_t(int64_t(x) + int64_t(tileX));
        y = int32_t(int64_t(y) + int64_t(tileY));
        at += 16;
    } else {
        uint32_t lines = exrSchemeLines(part.compression);
        uint64_t first = index * lines;
        uint64_t total = boxHeight(part.dataWindow);
        width = uint32_t(boxWidth(part.dataWindow));
        height = uint32_t(total - first < lines ? total - first : lines);
        y = int32_t(int64_t(y) + int64_t(first));
        at += 4;
    }
    int32_t size = readI32(bytes.data + at);
    at += 4;
    if (size < 0 || uint64_t(size) > bytes.size - at)
        return PrStatus_Truncated;
    PrByteView packed{bytes.data + at, size_t(size)};
    uint64_t unpackedSize = blockBytes(image, part, y, width, height);
    const uint8_t* pixels = packed.data;
    if (uint64_t(size) != unpackedSize) {
        if (part.compression == ExrScheme_None || unpackedSize > 0xffffffffu)
            return PrStatus_InvalidData;
        image.unpacked.resize(uint32_t(unpackedSize));
        ExrBlock block{
            .channels = image.blockChannels.data,
            .channelCount = part.channelCount,
            .y = y,
            .width = width,
            .height = height,
        };
        PrStatus status = exrDecompress(image.codec, part.compression, block, packed, image.unpacked.data,
            size_t(unpackedSize));
        if (status != PrStatus_Ok)
            return status;
        pixels = image.unpacked.data;
    }
    // The chunk's lines, each with the samples of each channel that samples it.
    for (uint32_t row = 0; row < height; ++row) {
        int32_t line = y + int32_t(row);
        for (uint32_t c = 0; c < part.channelCount; ++c) {
            const ExrPartChannel& channel = image.channels.data[part.firstChannel + c];
            if (channel.ySampling != 1 && line % int32_t(channel.ySampling) != 0)
                continue;
            uint32_t count = width / channel.xSampling;
            if (d.targets[c].offset >= 0)
                writeSamples(d, d.targets[c], pixels, count, x, line, channel.xSampling, channel.ySampling);
            pixels += size_t(count) * exrPixelBytes(channel.pixel);
        }
    }
    return PrStatus_Ok;
}

// Decodes every chunk of `level` into the destination. Returns Truncated if any is missing, after the rest.
PrStatus decodeLevel(ExrImage& image, uint32_t level, const ExrDestination& d) {
    const ExrPart& part = image.parts.data[image.part];
    uint64_t first = 0;
    uint64_t count = part.chunkCount;
    if (part.tiled) {
        uint32_t lx = part.levels == PrExrLevels_Rip ? level % part.levelsX : level;
        uint32_t ly = part.levels == PrExrLevels_Rip ? level / part.levelsX : level;
        first = tileIndex(part, ExrTile{0, 0, lx, ly});
        count = ceilDivide(levelWidth(part, lx), part.tileWidth) * ceilDivide(levelHeight(part, ly), part.tileHeight);
    }
    bool truncated = false;
    for (uint64_t i = first; i < first + count; ++i) {
        PrStatus status = decodeChunk(image, i, d);
        if (status == PrStatus_Truncated)
            truncated = true;
        else if (status != PrStatus_Ok)
            return status;
    }
    return truncated ? PrStatus_Truncated : PrStatus_Ok;
}

// The part's pixel that the output's first pixel is for: the display window's first at the first level, which holds the
// data window in the display window's frame, and the data window's first at any further level.
void destinationOrigin(const ExrPart& part, uint32_t level, ExrDestination* d) {
    bool full = level == 0;
    d->originX = full ? part.displayWindow.xMin : part.dataWindow.xMin;
    d->originY = full ? part.displayWindow.yMin : part.dataWindow.yMin;
}

// Returns how a sample of `pixel` is written as `sample`, which is Uint only for a uint.
ExrWrite writeOf(ExrPixel pixel, PrExrSample sample) {
    if (sample == PrExrSample_Uint)
        return ExrWrite_Copy32;
    if (sample == PrExrSample_Half) {
        if (pixel == ExrPixel_Half)
            return ExrWrite_Copy16;
        return pixel == ExrPixel_Uint ? ExrWrite_UintToHalf : ExrWrite_FloatToHalf;
    }
    if (pixel == ExrPixel_Half)
        return ExrWrite_HalfToFloat;
    return pixel == ExrPixel_Uint ? ExrWrite_UintToFloat : ExrWrite_Copy32;
}

// Zeroes the pixels of the destination that its level's chunks do not write: all of them if a sample of each pixel is
// `missing` or a chunk is, and otherwise those outside the data window.
void zeroUncovered(const ExrImage& image, const ExrDestination& d, uint32_t level, bool missing) {
    const ExrPart& part = image.parts.data[image.part];
    PrPixels output = d.output;
    size_t rowBytes = size_t(output.extent.x) * d.pixelBytes;
    int64_t x0 = int64_t(part.dataWindow.xMin) - d.originX;
    int64_t y0 = int64_t(part.dataWindow.yMin) - d.originY;
    // A level past the first is the data window's alone.
    int64_t x1 = level != 0 ? int64_t(output.extent.x) : int64_t(part.dataWindow.xMax) + 1 - d.originX;
    int64_t y1 = level != 0 ? int64_t(output.extent.y) : int64_t(part.dataWindow.yMax) + 1 - d.originY;
    x0 = x0 < 0 ? 0 : x0 > int64_t(output.extent.x) ? int64_t(output.extent.x) : x0;
    x1 = x1 < x0 ? x0 : x1 > int64_t(output.extent.x) ? int64_t(output.extent.x) : x1;
    y0 = y0 < 0 ? 0 : y0 > int64_t(output.extent.y) ? int64_t(output.extent.y) : y0;
    y1 = y1 < y0 ? y0 : y1 > int64_t(output.extent.y) ? int64_t(output.extent.y) : y1;
    bool all = missing || image.status == PrStatus_Truncated;
    for (uint32_t y = 0; y < output.extent.y; ++y) {
        uint8_t* row = output.data + size_t(y) * output.stride;
        if (all || int64_t(y) < y0 || int64_t(y) >= y1) {
            prMemZero(row, rowBytes);
            continue;
        }
        prMemZero(row, size_t(x0) * d.pixelBytes);
        prMemZero(row + size_t(x1) * d.pixelBytes, rowBytes - size_t(x1) * d.pixelBytes);
    }
}

// Converts luminance and chroma, decoded into red, green and blue, back to RGB. The pixels are `sample` floats or
// halves, `pixelBytes` apart.
void chromaToRgb(const ExrImage& image, PrPixels output, uint32_t pixelBytes) {
    bool halves = image.info.sample == PrExrSample_Half;
    float wr = image.lumaWeights[0];
    float wg = image.lumaWeights[1];
    float wb = image.lumaWeights[2];
    for (uint32_t y = 0; y < output.extent.y; ++y) {
        uint8_t* pixel = output.data + size_t(y) * output.stride;
        for (uint32_t x = 0; x < output.extent.x; ++x, pixel += pixelBytes) {
            float c[3];
            for (uint32_t i = 0; i < 3; ++i) {
                if (halves)
                    c[i] = halfToFloat(prMemReadLeU16(pixel + i * 2));
                else
                    c[i] = readF32(pixel + i * 4);
            }
            float luminance = c[0];
            float r = (c[1] + 1) * luminance;
            float b = (c[2] + 1) * luminance;
            float g = wg != 0 ? (luminance - r * wr - b * wb) / wg : 0;
            float rgb[3] = {r, g, b};
            for (uint32_t i = 0; i < 3; ++i) {
                if (halves) {
                    uint16_t bits = floatToHalf(rgb[i]);
                    prMemCopy(pixel + i * 2, &bits, 2);
                } else {
                    prMemCopy(pixel + i * 4, &rgb[i], 4);
                }
            }
        }
    }
}

PrStatus selectPart(ExrImage& image, uint32_t part) {
    image.part = part;
    image.status = describePart(image, tablesEnd(image));
    return image.status;
}

// Decodes `level` of the selected part into `output`, in the format of the part's info.
PrStatus exrReadLevel(ExrImage& image, uint32_t level, PrPixels output) {
    const ExrPart& part = image.parts.data[image.part];
    uint32_t sampleBytes = image.info.sample == PrExrSample_Half ? 2 : 4;
    uint32_t samples = image.info.colors + (image.info.alpha ? 1 : 0);
    image.targets.resize(part.channelCount);
    ExrTarget* targets = image.targets.data;
    for (uint32_t c = 0; c < part.channelCount; ++c)
        targets[c] = ExrTarget{.offset = -1};
    for (uint32_t s = 0; s < samples; ++s) {
        int32_t channel = image.slots[s];
        if (channel < 0)
            continue;
        ExrPixel pixel = image.channels.data[part.firstChannel + uint32_t(channel)].pixel;
        targets[channel] = ExrTarget{.offset = int32_t(s * sampleBytes), .write = writeOf(pixel, image.info.sample)};
    }
    ExrDestination d{.output = output, .pixelBytes = samples * sampleBytes, .targets = targets};
    destinationOrigin(part, level, &d);
    bool missing = false;
    for (uint32_t s = 0; s < samples; ++s)
        missing = missing || image.slots[s] < 0;
    zeroUncovered(image, d, level, missing);
    PrStatus status = decodeLevel(image, level, d);
    if (status != PrStatus_Ok && status != PrStatus_Truncated)
        return status;
    if (image.color == ExrColor_Chroma)
        chromaToRgb(image, output, samples * sampleBytes);
    return status == PrStatus_Ok ? image.status : status;
}

// Decodes the channels of `channels` of `level` of the selected part into `output`, as prExrImageReadChannels does.
PrStatus exrReadChannels(ExrImage& image, uint32_t level, const uint32_t* channels, uint32_t count, PrExrSample sample,
        PrPixels output) {
    const ExrPart& part = image.parts.data[image.part];
    uint32_t sampleBytes = sample == PrExrSample_Half ? 2 : 4;
    image.targets.resize(part.channelCount);
    ExrTarget* targets = image.targets.data;
    for (uint32_t c = 0; c < part.channelCount; ++c)
        targets[c] = ExrTarget{.offset = -1};
    for (uint32_t i = 0; i < count; ++i) {
        PR_ASSERT(targets[channels[i]].offset < 0);
        ExrPixel pixel = image.channels.data[part.firstChannel + channels[i]].pixel;
        if (sample == PrExrSample_Uint && pixel != ExrPixel_Uint)
            return PrStatus_NotSupported;
        targets[channels[i]] = ExrTarget{.offset = int32_t(i * sampleBytes), .write = writeOf(pixel, sample)};
    }
    ExrDestination d{.output = output, .pixelBytes = count * sampleBytes, .targets = targets};
    destinationOrigin(part, level, &d);
    zeroUncovered(image, d, level, false);
    PrStatus status = decodeLevel(image, level, d);
    if (status != PrStatus_Ok && status != PrStatus_Truncated)
        return status;
    return status == PrStatus_Ok ? image.status : status;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrExrImage : ExrImage {};

bool prExrHasSignature(PrByteView bytes) {
    return bytes.size >= kPrExrSignatureBytes && bytes.data[0] == 0x76 && bytes.data[1] == 0x2f && bytes.data[2] == 0x31 &&
        bytes.data[3] == 0x01;
}

PrExrLimits prExrLimitsDefault(void) {
    return PrExrLimits{
        .pixels = 64u * 1024u * 1024u,
        .workingBytes = 512u * 1024u * 1024u,
        .work = 1000000000u,
        .parts = 4096,
        .metadataBytes = 16u * 1024u * 1024u,
    };
}

PrStatus prExrImageCreate(PrByteView bytes, const PrCompressCodecSet* PR_NULLABLE compress,
        const PrExrLimits* PR_NULLABLE limits, uint64_t held, PrExrImage* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    const PrExrLimits bounds = limits != nullptr ? *limits : prExrLimitsDefault();
    if (bounds.work == 0)
        return PrStatus_Limit;
    if (!prExrHasSignature(bytes))
        return PrStatus_NotSupported;
    if (bytes.size < 8)
        return PrStatus_Truncated;
    uint32_t version = readU32(bytes.data + 4);
    uint32_t flags = version & ~0xffu;
    if ((version & 0xff) != kFileVersion || (flags & ~(kFlagTiled | kFlagLongNames | kFlagDeep | kFlagMultipart)) != 0)
        return PrStatus_NotSupported;
    if ((flags & kFlagTiled) != 0 && (flags & (kFlagDeep | kFlagMultipart)) != 0)
        return PrStatus_InvalidData;
    PrExrImage* image = allocZeroed<PrExrImage>();
    image->bytes = bytes;
    image->limits = bounds;
    image->held = held;
    image->multipart = (flags & kFlagMultipart) != 0;
    image->codec.compress = compress;
    size_t at = 8;
    uint64_t metadata = 0;
    PrStatus status = PrStatus_Ok;
    for (;;) {
        ExrPart part{};
        status = readHeader(*image, flags, &at, &metadata, &part);
        if (status != PrStatus_Ok)
            break;
        // A single part of deep data says so in the version field alone.
        if (!image->multipart && (flags & kFlagDeep) != 0)
            part.support = PrStatus_NotSupported;
        if (image->parts.count >= bounds.parts) {
            status = PrStatus_Limit;
            break;
        }
        image->parts.push(part);
        if (!image->multipart)
            break;
        if (at >= bytes.size) {
            status = PrStatus_Truncated;
            break;
        }
        if (bytes.data[at] == 0) {
            ++at;
            break;
        }
    }
    // A file that ends within its headers has no part to read.
    if (status == PrStatus_Truncated)
        status = PrStatus_InvalidData;
    if (status == PrStatus_Ok) {
        uint64_t tableAt = at;
        for (uint32_t i = 0; i < image->parts.count; ++i) {
            image->parts.data[i].tableOffset = tableAt;
            tableAt += image->parts.data[i].chunkCount * 8;
        }
        // The first part that the decoder reads is selected.
        for (uint32_t part = 0; part < image->parts.count; ++part) {
            status = selectPart(*image, part);
            if (status != PrStatus_NotSupported)
                break;
        }
    }
    if (status != PrStatus_Ok && status != PrStatus_Truncated) {
        prExrImageDestroy(image);
        return status;
    }
    *out = image;
    return status;
}

void prExrImageDestroy(PrExrImage* PR_NULLABLE image) {
    if (image == nullptr)
        return;
    exrCodecReset(image->codec);
    image->parts.reset();
    image->channels.reset();
    image->attributes.reset();
    image->offsets.reset();
    image->unpacked.reset();
    image->blockChannels.reset();
    image->targets.reset();
    prMemoryFree(image);
}

uint32_t prExrImagePartCount(const PrExrImage* image) {
    return image->parts.count;
}

PrStatus prExrImageSelectPart(PrExrImage* image, uint32_t part) {
    PR_ASSERT(part < image->parts.count);
    uint32_t previous = image->part;
    PrStatus status = selectPart(*image, part);
    if (status != PrStatus_Ok && status != PrStatus_Truncated)
        selectPart(*image, previous);
    return status;
}

uint32_t prExrImageSelectedPart(const PrExrImage* image) {
    return image->part;
}

const PrExrPartInfo* prExrImagePartInfo(const PrExrImage* image) {
    return &image->info;
}

PrUVec2 prExrImageLevelExtent(const PrExrImage* image, uint32_t level) {
    PR_ASSERT(level < image->info.levelCount);
    const ExrPart& part = image->parts.data[image->part];
    if (level == 0)
        return PrUVec2{uint32_t(boxWidth(part.displayWindow)), uint32_t(boxHeight(part.displayWindow))};
    uint32_t lx = part.levels == PrExrLevels_Rip ? level % part.levelsX : level;
    uint32_t ly = part.levels == PrExrLevels_Rip ? level / part.levelsX : level;
    return PrUVec2{levelWidth(part, lx), levelHeight(part, ly)};
}

PrExrChannel prExrImageChannel(const PrExrImage* image, uint32_t index) {
    const ExrPart& part = image->parts.data[image->part];
    PR_ASSERT(index < part.channelCount);
    const ExrPartChannel& channel = image->channels.data[part.firstChannel + index];
    return PrExrChannel{
        .name = channel.name,
        .sample = PrExrSample(channel.pixel),
        .sampling = PrUVec2{channel.xSampling, channel.ySampling},
        .perceptual = channel.linear,
    };
}

PrExrAttribute prExrImageAttribute(const PrExrImage* image, uint32_t index) {
    const ExrPart& part = image->parts.data[image->part];
    PR_ASSERT(index < part.attributeCount);
    const PrExrAttribute& attribute = image->attributes.data[part.firstAttribute + index];
    return PrExrAttribute{.name = attribute.name, .type = attribute.type, .value = attribute.value};
}

PrStatus prExrImageHeaderRead(const PrExrImage* image, PrExrHeader* out) {
    *out = PrExrHeader{.pixelAspectRatio = 1, .screenWindowWidth = 1};
    uint32_t windows = 0;
    auto readBox = [](const uint8_t* bytes) {
        return PrExrBox{
            .xMin = readI32(bytes),
            .yMin = readI32(bytes + 4),
            .xMax = readI32(bytes + 8),
            .yMax = readI32(bytes + 12),
        };
    };
    for (uint32_t i = 0; i < image->info.attributeCount; ++i) {
        PrExrAttribute attribute = prExrImageAttribute(image, i);
        const uint8_t* v = attribute.value.data;
        size_t size = attribute.value.size;
        PrStringView text{reinterpret_cast<const char*>(v), size};
        if (attribute.name == "dataWindow" && attribute.type == "box2i" && size == 16) {
            out->dataWindow = readBox(v);
            windows |= 1;
        } else if (attribute.name == "displayWindow" && attribute.type == "box2i" && size == 16) {
            out->displayWindow = readBox(v);
            windows |= 2;
        } else if (attribute.name == "pixelAspectRatio" && attribute.type == "float" && size == 4) {
            out->pixelAspectRatio = readF32(v);
        } else if (attribute.name == "screenWindowCenter" && attribute.type == "v2f" && size == 8) {
            out->screenWindowCenter = PrVec2{readF32(v), readF32(v + 4)};
        } else if (attribute.name == "screenWindowWidth" && attribute.type == "float" && size == 4) {
            out->screenWindowWidth = readF32(v);
        } else if (attribute.name == "lineOrder" && attribute.type == "lineOrder" && size == 1) {
            out->lineOrder = v[0];
        } else if (attribute.name == "compression" && attribute.type == "compression" && size == 1) {
            out->compression = v[0];
        } else if (attribute.name == "tiles" && attribute.type == "tiledesc" && size == 9) {
            out->tileExtent = PrUVec2{readU32(v), readU32(v + 4)};
        } else if (attribute.name == "name" && attribute.type == "string") {
            out->name = text;
        } else if (attribute.name == "type" && attribute.type == "string") {
            out->type = text;
        } else if (attribute.name == "view" && (attribute.type == "string" || attribute.type == "text")) {
            out->view = text;
        }
    }
    return windows == 3 ? PrStatus_Ok : PrStatus_InvalidData;
}

PrStatus prExrImageRead(PrExrImage* image, uint32_t level, PrPixels output) {
    PR_ASSERT(level < image->info.levelCount);
    PR_ASSERT(output.extent == prExrImageLevelExtent(image, level));
    return exrReadLevel(*image, level, output);
}

PrStatus prExrImageReadChannels(PrExrImage* image, uint32_t level, const uint32_t* channels, uint32_t count,
        PrExrSample sample, PrPixels output) {
    PR_ASSERT(level < image->info.levelCount && count != 0);
    for (uint32_t i = 0; i < count; ++i)
        PR_ASSERT(channels[i] < image->info.channelCount);
    return exrReadChannels(*image, level, channels, count, sample, output);
}
