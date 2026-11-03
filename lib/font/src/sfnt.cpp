// OpenType tables
//
// opentype - https://learn.microsoft.com/en-us/typography/opentype/spec/

#include "font/src/sfnt.h"

#include "pr/raw_array.h"
#include "pr/search.h"
#include "pr/utf8.h"

namespace pr_impl {
namespace {

constexpr uint32_t kTtcf = 0x74746366;           // 'ttcf'
constexpr uint32_t kMvar = 0x4d564152;           // 'MVAR'
constexpr uint32_t kName = 0x6e616d65;           // 'name'
constexpr uint32_t kBase = 0x42415345;           // 'BASE'
constexpr uint32_t kDflt = 0x44464c54;           // 'DFLT'
constexpr uint32_t kGsub = 0x47535542;           // 'GSUB'
constexpr uint32_t kGpos = 0x47504f53;           // 'GPOS'
constexpr uint32_t kOs2 = 0x4f532f32;            // 'OS/2'
constexpr uint32_t kPost = 0x706f7374;           // 'post'
constexpr uint32_t kFvar = 0x66766172;           // 'fvar'
constexpr uint32_t kAvar = 0x61766172;           // 'avar'
constexpr uint32_t kSbix = 0x73626978;           // 'sbix'
constexpr uint32_t kCblc = 0x43424c43;           // 'CBLC'
constexpr uint32_t kCbdt = 0x43424454;           // 'CBDT'
constexpr uint32_t kMaxp = 0x6d617870;           // 'maxp'
constexpr uint32_t kCmap = 0x636d6170;           // 'cmap'
constexpr uint32_t kSvg = 0x53564720;            // 'SVG '
constexpr uint32_t kPng = 0x706e6720;            // 'png ' in sbix
constexpr uint32_t kJpg = 0x6a706720;            // 'jpg ' in sbix
constexpr uint32_t kTiff = 0x74696666;           // 'tiff' in sbix
constexpr uint32_t kDupe = 0x64757065;           // 'dupe' in sbix

// Returns the scalar of a region at the coordinates: the product of the tents of its axes.
float regionScalar(SfntTable regions, uint16_t index, SfntCoords coords) {
    uint16_t axisCount = regions.uint16At(0);
    uint16_t regionCount = regions.uint16At(2);
    if (index >= regionCount)
        return 0;
    uint32_t at = 4 + uint32_t(index) * axisCount * 6;
    float scalar = 1;
    for (uint16_t axis = 0; axis < axisCount && scalar != 0; ++axis, at += 6) {
        float coord = axis < coords.count && coords.values != nullptr ? coords.values[axis] : 0;
        scalar *= sfntRegionAxisScalar(regions.f2dot14(at), regions.f2dot14(at + 2), regions.f2dot14(at + 4), coord);
    }
    return scalar;
}

// Returns `value` rounded to a multiple of 1/16384, as F2DOT14 holds it.
float roundF2dot14(float value) {
    return float(int32_t(value * 16384 + (value < 0 ? -0.5f : 0.5f))) / 16384;
}

// Returns the coordinate of a BaseCoord table, in font units, at the coordinates.
float baseCoord(SfntTable coord, SfntTable store, SfntCoords coords) {
    float value = float(coord.int16At(2));
    if (coord.uint16At(0) == 3 && coord.uint16At(4) != 0 && store.size != 0) {
        SfntTable device = coord.from(coord.uint16At(4));
        if (device.uint16At(4) == 0x8000)
            value += sfntVariationDelta(store, device.uint16At(0), device.uint16At(2), coords);
    }
    return value;
}

// Returns the base values of a script record, or an empty table if it has none.
SfntTable baseValues(SfntTable scripts, uint16_t record) {
    SfntTable script = scripts.from(scripts.uint16At(2 + uint32_t(record) * 6 + 4));
    uint16_t offset = script.uint16At(0);
    return offset != 0 ? script.from(offset) : SfntTable{nullptr, 0};
}

void appendUtf8(PrRawString* out, uint32_t codePoint) {
    char bytes[kPrUtf8MaxBytes];
    uint32_t length = prUtf8Encode(codePoint, bytes);
    prRawStringAppend(out, PrStringView{bytes, length});
}

// Reads the size of a PNG from its header. Returns false for bytes that are not a PNG.
bool pngSize(SfntTable png, uint32_t* width, uint32_t* height) {
    if (png.uint32At(0) != 0x89504e47u || png.uint32At(4) != 0x0d0a1a0au || png.uint32At(12) != 0x49484452u)
        return false;
    *width = png.uint32At(16);
    *height = png.uint32At(20);
    return *width != 0 && *height != 0;
}

// Reads the size of a JPEG from its frame header. Returns false for bytes that are not a JPEG.
bool jpegSize(SfntTable jpeg, uint32_t* width, uint32_t* height) {
    if (jpeg.uint16At(0) != 0xffd8)
        return false;
    for (uint32_t at = 2; at + 4 <= jpeg.size;) {
        if (jpeg.uint8At(at) != 0xff)
            return false;
        uint8_t marker = jpeg.uint8At(at + 1);
        if (marker == 0xff) {
            ++at;
            continue;
        }
        uint32_t length = jpeg.uint16At(at + 2);
        bool frame = marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc;
        if (frame) {
            *height = jpeg.uint16At(at + 5);
            *width = jpeg.uint16At(at + 7);
            return *width != 0 && *height != 0;
        }
        if (marker == 0xd9 || marker == 0xda || length < 2)
            return false;
        at += 2 + length;
    }
    return false;
}

// Reads the size of the first image of a TIFF from its directory. Returns false for bytes that are not a TIFF.
bool tiffSize(SfntTable tiff, uint32_t* width, uint32_t* height) {
    bool little = tiff.uint16At(0) == 0x4949 && tiff.uint16At(2) == 0x2a00;
    bool big = tiff.uint16At(0) == 0x4d4d && tiff.uint16At(2) == 0x002a;
    if (!little && !big)
        return false;
    auto read16 = [&](uint32_t at) {
        uint16_t v = tiff.uint16At(at);
        return little ? uint16_t(v >> 8 | v << 8) : v;
    };
    auto read32 = [&](uint32_t at) {
        uint32_t v = tiff.uint32At(at);
        return little ? (v >> 24 | (v >> 8 & 0xff00) | (v << 8 & 0xff0000) | v << 24) : v;
    };
    uint32_t directory = read32(4);
    uint32_t entries = read16(directory);
    *width = 0;
    *height = 0;
    for (uint32_t i = 0; i < entries; ++i) {
        uint32_t entry = directory + 2 + i * 12;
        uint16_t tag = read16(entry);
        uint32_t value = read16(entry + 2) == 3 ? read16(entry + 8) : read32(entry + 8);
        if (tag == 256)
            *width = value;
        else if (tag == 257)
            *height = value;
    }
    return *width != 0 && *height != 0;
}

// Finds the image of a glyph in an sbix strike, following a duplicate once. Returns false if the strike has no image
// for the glyph.
bool sbixImage(SfntTable strike, uint32_t glyph, uint32_t glyphCount, SfntImage* out) {
    for (uint32_t follow = 0; follow < 2 && glyph < glyphCount; ++follow) {
        uint32_t start = strike.uint32At(4 + glyph * 4);
        uint32_t end = strike.uint32At(8 + glyph * 4);
        if (end > strike.size || start >= end || end - start <= 8)
            return false;
        SfntTable data = strike.from(start);
        uint32_t type = data.uint32At(4);
        if (type == kDupe) {
            glyph = data.uint16At(8);
            continue;
        }
        SfntTable image{data.data + 8, end - start - 8};
        uint32_t width = 0;
        uint32_t height = 0;
        bool sized = false;
        if (type == kPng)
            sized = pngSize(image, &width, &height);
        else if (type == kJpg)
            sized = jpegSize(image, &width, &height);
        else if (type == kTiff)
            sized = tiffSize(image, &width, &height);
        if (!sized)
            return false;
        // The origin offset places the bottom left corner of the image, with y up.
        out->data = image.data;
        out->size = image.size;
        out->type = type == kPng ? SfntImageType_Png : type == kJpg ? SfntImageType_Jpeg : SfntImageType_Tiff;
        out->width = width;
        out->height = height;
        out->left = float(data.int16At(0));
        out->top = -float(data.int16At(2)) - float(height);
        return true;
    }
    return false;
}

// Returns the number of entries in a table that the file gives, limited to the entries that fit, so that a loop over
// them ends with the table.
uint32_t entriesIn(SfntTable table, uint32_t start, uint32_t entrySize, uint32_t count) {
    uint32_t fit = table.size > start ? (table.size - start) / entrySize : 0;
    return count < fit ? count : fit;
}

// Finds the image of a glyph in CBDT for a CBLC strike. Returns false if the strike has no image for the glyph.
bool cbdtImage(SfntTable cblc, SfntTable cbdt, uint32_t record, uint32_t glyph, SfntImage* out) {
    SfntTable arrays = cblc.from(cblc.uint32At(record));
    uint32_t count = entriesIn(arrays, 0, 8, cblc.uint32At(record + 8));
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t first = arrays.uint16At(i * 8);
        uint32_t last = arrays.uint16At(i * 8 + 2);
        if (glyph < first || glyph > last)
            continue;
        SfntTable sub = arrays.from(arrays.uint32At(i * 8 + 4));
        uint16_t indexFormat = sub.uint16At(0);
        uint16_t imageFormat = sub.uint16At(2);
        uint32_t imageData = sub.uint32At(4);
        uint32_t offset = 0;
        uint32_t length = 0;
        SfntTable bigMetrics{nullptr, 0};
        uint32_t k = glyph - first;
        switch (indexFormat) {
            case 1:
                offset = sub.uint32At(8 + k * 4);
                length = sub.uint32At(12 + k * 4) - offset;
                break;
            case 2:
                length = sub.uint32At(8);
                offset = k * length;
                bigMetrics = sub.from(12);
                break;
            case 3:
                offset = sub.uint16At(8 + k * 2);
                length = uint32_t(sub.uint16At(10 + k * 2)) - offset;
                break;
            case 4: {
                uint32_t glyphs = sub.uint32At(8);
                glyphs = entriesIn(sub, 12, 4, glyphs);
                bool found = false;
                for (uint32_t g = 0; g < glyphs && !found; ++g) {
                    if (sub.uint16At(12 + g * 4) == glyph) {
                        offset = sub.uint16At(14 + g * 4);
                        length = uint32_t(sub.uint16At(18 + g * 4)) - offset;
                        found = true;
                    }
                }
                if (!found)
                    return false;
                break;
            }
            case 5: {
                length = sub.uint32At(8);
                bigMetrics = sub.from(12);
                uint32_t glyphs = sub.uint32At(20);
                glyphs = entriesIn(sub, 24, 2, glyphs);
                bool found = false;
                for (uint32_t g = 0; g < glyphs && !found; ++g) {
                    if (sub.uint16At(24 + g * 2) == glyph) {
                        offset = g * length;
                        found = true;
                    }
                }
                if (!found)
                    return false;
                break;
            }
            default:
                return false;
        }
        if (length == 0 || offset > 0xffffffffu - imageData)
            return false;
        SfntTable at = cbdt.from(imageData + offset);
        if (at.data == nullptr || at.size < length)
            return false;
        SfntTable data{at.data, length};
        // The metrics are those of the image itself, or those of the index. A bearing places the top left corner, with
        // y up.
        float left = 0;
        float top = 0;
        uint32_t skip = 0;
        if (imageFormat == 17) {
            left = float(int8_t(data.uint8At(2)));
            top = -float(int8_t(data.uint8At(3)));
            skip = 5;
        } else if (imageFormat == 18) {
            left = float(int8_t(data.uint8At(2)));
            top = -float(int8_t(data.uint8At(3)));
            skip = 8;
        } else if (imageFormat == 19 && bigMetrics.size != 0) {
            left = float(int8_t(bigMetrics.uint8At(2)));
            top = -float(int8_t(bigMetrics.uint8At(3)));
        } else {
            return false;
        }
        uint32_t dataLength = data.uint32At(skip);
        if (data.size < skip + 4 || dataLength > data.size - skip - 4)
            return false;
        SfntTable png{data.data + skip + 4, dataLength};
        uint32_t width = 0;
        uint32_t height = 0;
        if (!pngSize(png, &width, &height))
            return false;
        *out = SfntImage{png.data, png.size, width, height, 0, left, top, SfntImageType_Png};
        return true;
    }
    return false;
}

} // namespace

uint8_t SfntTable::uint8At(uint32_t at) const {
    return data != nullptr && at < size ? data[at] : 0;
}

uint16_t SfntTable::uint16At(uint32_t at) const {
    return uint16_t(uint8At(at) << 8 | uint8At(at + 1));
}

int16_t SfntTable::int16At(uint32_t at) const {
    return int16_t(uint16At(at));
}

uint32_t SfntTable::uint32At(uint32_t at) const {
    return uint32_t(uint16At(at)) << 16 | uint16At(at + 2);
}

float SfntTable::fixed(uint32_t at) const {
    return float(double(int32_t(uint32At(at))) / 65536);
}

float SfntTable::f2dot14(uint32_t at) const {
    return float(int16At(at)) / 16384;
}

SfntTable SfntTable::from(uint32_t at) const {
    if (data == nullptr || at >= size)
        return SfntTable{nullptr, 0};
    return SfntTable{data + at, size - at};
}

uint32_t sfntFaceCount(const uint8_t* file, size_t size) {
    SfntTable whole{file, uint32_t(size < 0xffffffffu ? size : 0xffffffffu)};
    uint32_t count = whole.uint32At(0) == kTtcf ? entriesIn(whole, 12, 4, whole.uint32At(8)) : 1;
    Sfnt sfnt;
    return sfntOpen(file, size, 0, &sfnt) ? count : 0;
}

bool sfntOpen(const uint8_t* file, size_t size, uint32_t index, Sfnt* out) {
    SfntTable whole{file, uint32_t(size < 0xffffffffu ? size : 0xffffffffu)};
    uint32_t directory = 0;
    if (whole.uint32At(0) == kTtcf) {
        if (index >= whole.uint32At(8))
            return false;
        directory = whole.uint32At(12 + index * 4);
    } else if (index != 0) {
        return false;
    }
    uint32_t version = whole.uint32At(directory);
    if (version != 0x00010000 && version != 0x4f54544f && version != 0x74727565)
        return false;
    // The table directory, with its records, lies within the file.
    uint64_t records = uint64_t(directory) + 12 + uint64_t(whole.uint16At(directory + 4)) * 16;
    if (records > whole.size)
        return false;
    *out = Sfnt{file, size, directory};
    return true;
}

SfntTable sfntTable(const Sfnt& sfnt, uint32_t tag) {
    SfntTable whole{sfnt.file, uint32_t(sfnt.size < 0xffffffffu ? sfnt.size : 0xffffffffu)};
    uint16_t count = whole.uint16At(sfnt.directory + 4);
    for (uint16_t i = 0; i < count; ++i) {
        uint32_t record = sfnt.directory + 12 + uint32_t(i) * 16;
        if (whole.uint32At(record) != tag)
            continue;
        uint32_t offset = whole.uint32At(record + 8);
        uint32_t length = whole.uint32At(record + 12);
        if (offset > whole.size || length > whole.size - offset)
            return SfntTable{nullptr, 0};
        return SfntTable{sfnt.file + offset, length};
    }
    return SfntTable{nullptr, 0};
}

float sfntVariationDelta(SfntTable store, uint16_t outer, uint16_t inner, SfntCoords coords) {
    if (coords.count == 0 || store.uint16At(0) != 1 || outer >= store.uint16At(6))
        return 0;
    SfntTable regions = store.from(store.uint32At(2));
    SfntTable data = store.from(store.uint32At(8 + uint32_t(outer) * 4));
    uint16_t itemCount = data.uint16At(0);
    uint16_t words = data.uint16At(2);
    bool longWords = (words & 0x8000) != 0;
    uint16_t wordCount = words & 0x7fff;
    uint16_t regionCount = data.uint16At(4);
    if (inner >= itemCount || wordCount > regionCount)
        return 0;
    uint32_t wide = longWords ? 4 : 2;
    uint32_t narrow = longWords ? 2 : 1;
    uint32_t row = wordCount * wide + (regionCount - wordCount) * narrow;
    uint32_t at = 6 + uint32_t(regionCount) * 2 + inner * row;
    float delta = 0;
    for (uint16_t r = 0; r < regionCount; ++r) {
        int32_t value = 0;
        if (r < wordCount) {
            value = longWords ? int32_t(data.uint32At(at)) : data.int16At(at);
            at += wide;
        } else {
            value = longWords ? data.int16At(at) : int8_t(data.uint8At(at));
            at += narrow;
        }
        uint16_t region = data.uint16At(6 + uint32_t(r) * 2);
        if (value != 0)
            delta += float(value) * regionScalar(regions, region, coords);
    }
    return delta;
}

void sfntDeltaSetIndex(SfntTable map, uint32_t index, uint16_t* outer, uint16_t* inner) {
    if (map.size == 0) {
        *outer = 0;
        *inner = uint16_t(index);
        return;
    }
    uint8_t format = map.uint8At(0);
    uint8_t entryFormat = map.uint8At(1);
    uint32_t count = format == 0 ? map.uint16At(2) : map.uint32At(2);
    uint32_t start = format == 0 ? 4 : 6;
    uint32_t entrySize = uint32_t((entryFormat >> 4) & 3) + 1;
    uint32_t innerBits = uint32_t(entryFormat & 0xf) + 1;
    if (count == 0) {
        *outer = 0;
        *inner = 0;
        return;
    }
    if (index >= count)
        index = count - 1;
    uint32_t at = start + index * entrySize;
    uint32_t entry = 0;
    for (uint32_t i = 0; i < entrySize; ++i)
        entry = entry << 8 | map.uint8At(at + i);
    *outer = uint16_t(entry >> innerBits);
    *inner = uint16_t(entry & ((1u << innerBits) - 1));
}

uint32_t sfntVariationScalars(SfntTable store, uint16_t outer, SfntCoords coords, float* PR_NULLABLE out,
        uint32_t capacity) {
    if (store.uint16At(0) != 1 || outer >= store.uint16At(6))
        return 0;
    SfntTable regions = store.from(store.uint32At(2));
    SfntTable data = store.from(store.uint32At(8 + uint32_t(outer) * 4));
    uint16_t regionCount = data.uint16At(4);
    for (uint16_t r = 0; r < regionCount && r < capacity; ++r)
        out[r] = regionScalar(regions, data.uint16At(6 + uint32_t(r) * 2), coords);
    return regionCount;
}

float sfntRegionAxisScalar(float start, float peak, float end, float coord) {
    if (peak == 0 || start > peak || peak > end || (start < 0 && end > 0))
        return 1;
    if (coord == peak)
        return 1;
    if (coord <= start || coord >= end)
        return 0;
    return coord < peak ? (coord - start) / (peak - start) : (end - coord) / (end - peak);
}

uint32_t sfntAxisCount(const Sfnt& sfnt) {
    SfntTable fvar = sfntTable(sfnt, kFvar);
    uint32_t count = fvar.uint16At(8);
    uint32_t size = fvar.uint16At(10);
    return size >= 20 && fvar.uint16At(0) == 1 ? entriesIn(fvar, fvar.uint16At(4), size, count) : 0;
}

PrFontAxis sfntAxis(const Sfnt& sfnt, uint32_t index) {
    SfntTable fvar = sfntTable(sfnt, kFvar);
    uint32_t at = fvar.uint16At(4) + index * fvar.uint16At(10);
    float minimum = fvar.fixed(at + 4);
    float defaultValue = fvar.fixed(at + 8);
    float maximum = fvar.fixed(at + 12);
    // An axis whose range does not hold its default is pinned to the default.
    if (minimum > defaultValue)
        minimum = defaultValue;
    if (maximum < defaultValue)
        maximum = defaultValue;
    return PrFontAxis{fvar.uint32At(at), minimum, defaultValue, maximum};
}

void sfntNormalize(const Sfnt& sfnt, const float* design, uint32_t count, float* out) {
    for (uint32_t i = 0; i < count; ++i) {
        PrFontAxis axis = sfntAxis(sfnt, i);
        float value = design[i] < axis.min ? axis.min : design[i] > axis.max ? axis.max : design[i];
        float normalized = 0;
        if (value < axis.defaultValue && axis.defaultValue > axis.min)
            normalized = (value - axis.defaultValue) / (axis.defaultValue - axis.min);
        else if (value > axis.defaultValue && axis.max > axis.defaultValue)
            normalized = (value - axis.defaultValue) / (axis.max - axis.defaultValue);
        out[i] = roundF2dot14(normalized);
    }
    // avar maps each axis through segments of (from, to) pairs, linear between them.
    SfntTable avar = sfntTable(sfnt, kAvar);
    if (avar.uint16At(0) != 1 && avar.uint16At(0) != 2)
        return;
    uint32_t axes = avar.uint16At(6);
    uint32_t at = 8;
    for (uint32_t i = 0; i < axes && at + 2 <= avar.size; ++i) {
        uint32_t pairs = avar.uint16At(at);
        uint32_t map = at + 2;
        at = map + pairs * 4;
        if (i >= count || pairs < 2)
            continue;
        float value = out[i];
        float lowestFrom = avar.f2dot14(map);
        float highestFrom = avar.f2dot14(map + (pairs - 1) * 4);
        if (value <= lowestFrom) {
            value = avar.f2dot14(map + 2);
        } else if (value >= highestFrom) {
            value = avar.f2dot14(map + (pairs - 1) * 4 + 2);
        } else {
            for (uint32_t k = 1; k < pairs; ++k) {
                float from = avar.f2dot14(map + k * 4);
                if (value >= from)
                    continue;
                float previousFrom = avar.f2dot14(map + (k - 1) * 4);
                float previousTo = avar.f2dot14(map + (k - 1) * 4 + 2);
                float to = avar.f2dot14(map + k * 4 + 2);
                value = previousTo + (value - previousFrom) * (to - previousTo) / (from - previousFrom);
                break;
            }
        }
        out[i] = roundF2dot14(value);
    }
}

float sfntMetricDelta(const Sfnt& sfnt, uint32_t tag, SfntCoords coords) {
    SfntTable mvar = sfntTable(sfnt, kMvar);
    if (coords.count == 0 || mvar.size == 0)
        return 0;
    uint16_t recordSize = mvar.uint16At(6);
    uint16_t count = mvar.uint16At(8);
    SfntTable store = mvar.from(mvar.uint16At(10));
    for (uint16_t i = 0; i < count; ++i) {
        uint32_t record = 12 + uint32_t(i) * recordSize;
        if (mvar.uint32At(record) == tag) {
            return sfntVariationDelta(store, mvar.uint16At(record + 4), mvar.uint16At(record + 6), coords);
        }
    }
    return 0;
}

PrRawString sfntName(const Sfnt& sfnt, uint16_t id) {
    SfntTable name = sfntTable(sfnt, kName);
    uint16_t count = name.uint16At(2);
    SfntTable strings = name.from(name.uint16At(4));
    // The best record: Windows Unicode in US English, any Windows Unicode, Unicode, Mac Roman.
    int32_t best = -1;
    uint32_t bestRank = 0;
    for (uint16_t i = 0; i < count; ++i) {
        uint32_t record = 6 + uint32_t(i) * 12;
        if (name.uint16At(record + 6) != id)
            continue;
        uint16_t platform = name.uint16At(record);
        uint16_t encoding = name.uint16At(record + 2);
        uint16_t language = name.uint16At(record + 4);
        uint32_t rank = 0;
        if (platform == 3 && (encoding == 1 || encoding == 10))
            rank = language == 0x409 ? 4 : 3;
        else if (platform == 0)
            rank = 2;
        else if (platform == 1 && encoding == 0)
            rank = 1;
        if (rank > bestRank) {
            bestRank = rank;
            best = int32_t(record);
        }
    }
    if (best < 0)
        return prRawStringNull();
    uint32_t record = uint32_t(best);
    SfntTable text = strings.from(name.uint16At(record + 10));
    uint32_t length = name.uint16At(record + 8);
    PrRawString out = prRawStringEmpty();
    if (bestRank == 1) {
        for (uint32_t i = 0; i < length; ++i) {
            uint8_t byte = text.uint8At(i);
            appendUtf8(&out, byte < 0x80 ? byte : 0xfffd);
        }
        return out;
    }
    for (uint32_t i = 0; i + 1 < length; i += 2) {
        uint32_t unit = text.uint16At(i);
        if (unit >= 0xd800 && unit < 0xdc00 && i + 3 < length) {
            uint32_t low = text.uint16At(i + 2);
            if (low >= 0xdc00 && low < 0xe000) {
                unit = 0x10000 + ((unit - 0xd800) << 10) + (low - 0xdc00);
                i += 2;
            }
        }
        appendUtf8(&out, unit);
    }
    return out;
}

bool sfntBaseline(const Sfnt& sfnt, uint32_t baselineTag, uint32_t scriptTag, SfntCoords coords, float* out) {
    SfntTable base = sfntTable(sfnt, kBase);
    if (base.size == 0 || base.uint16At(4) == 0)
        return false;
    SfntTable store{nullptr, 0};
    if (base.uint16At(2) >= 1 && base.uint32At(8) != 0)
        store = base.from(base.uint32At(8));
    SfntTable axis = base.from(base.uint16At(4));
    SfntTable tags = axis.from(axis.uint16At(0));
    SfntTable scripts = axis.from(axis.uint16At(2));
    uint16_t tagCount = tags.uint16At(0);
    uint32_t tagIndex = tagCount;
    for (uint16_t i = 0; i < tagCount; ++i) {
        if (tags.uint32At(2 + uint32_t(i) * 4) == baselineTag)
            tagIndex = i;
    }
    if (tagIndex == tagCount || axis.uint16At(0) == 0)
        return false;
    // The requested script, then the default script, then the first script that has the tag.
    uint16_t scriptCount = scripts.uint16At(0);
    uint32_t order[3] = {scriptTag, kDflt, 0};
    for (uint32_t wanted : order) {
        for (uint16_t i = 0; i < scriptCount; ++i) {
            if (wanted != 0 && scripts.uint32At(2 + uint32_t(i) * 6) != wanted)
                continue;
            SfntTable values = baseValues(scripts, i);
            if (values.size == 0 || tagIndex >= values.uint16At(2))
                continue;
            uint16_t offset = values.uint16At(4 + tagIndex * 2);
            if (offset == 0)
                continue;
            *out = baseCoord(values.from(offset), store, coords);
            return true;
        }
    }
    return false;
}

uint32_t sfntScriptTags(const Sfnt& sfnt, uint32_t* PR_NULLABLE out, uint32_t capacity) {
    uint32_t count = 0;
    auto add = [&](uint32_t tag) {
        for (uint32_t i = 0; i < count && i < capacity; ++i) {
            if (out[i] == tag)
                return;
        }
        if (count < capacity)
            out[count] = tag;
        ++count;
    };
    const uint32_t tables[] = {kGsub, kGpos};
    for (uint32_t tableTag : tables) {
        SfntTable table = sfntTable(sfnt, tableTag);
        if (table.size == 0)
            continue;
        SfntTable scripts = table.from(table.uint16At(4));
        uint16_t scriptCount = scripts.uint16At(0);
        for (uint16_t i = 0; i < scriptCount; ++i) {
            uint32_t record = 2 + uint32_t(i) * 6;
            uint32_t scriptTag = scripts.uint32At(record);
            if (scriptTag != kDflt)
                add(scriptTag);
        }
    }
    return count;
}

void sfntDescribe(const Sfnt& sfnt, PrFontInfo* out) {
    SfntTable os2 = sfntTable(sfnt, kOs2);
    SfntTable post = sfntTable(sfnt, kPost);
    constexpr float kWidths[] = {100, 50, 62.5f, 75, 87.5f, 100, 112.5f, 125, 150, 200};
    float weight = os2.size != 0 && os2.uint16At(4) != 0 ? float(os2.uint16At(4)) : kPrFontWeightNormal;
    uint16_t widthClass = os2.uint16At(6);
    float width = widthClass >= 1 && widthClass <= 9 ? kWidths[widthClass] : 100;
    PrFontInfo& d = *out;
    d = PrFontInfo{weight, weight, width, width, 0, 0, 0, 0, PrFontSlope_Normal, false};
    uint16_t selection = os2.uint16At(62);
    float italicAngle = post.fixed(4);
    if ((selection & 1u) != 0)
        d.slope = PrFontSlope_Italic;
    else if ((selection & (1u << 9)) != 0 || italicAngle != 0)
        d.slope = PrFontSlope_Oblique;
    if (d.slope != PrFontSlope_Normal)
        d.obliqueMin = d.obliqueMax = -italicAngle;
    SfntTable fvar = sfntTable(sfnt, kFvar);
    uint16_t axes = fvar.uint16At(4);
    uint16_t count = fvar.uint16At(8);
    uint16_t size = fvar.uint16At(10);
    for (uint16_t i = 0; i < count; ++i) {
        uint32_t at = axes + uint32_t(i) * size;
        uint32_t tag = fvar.uint32At(at);
        float minimum = fvar.fixed(at + 4);
        float maximum = fvar.fixed(at + 12);
        if (tag == 0x77676874) {                // wght
            d.weightMin = minimum;
            d.weightMax = maximum;
        } else if (tag == 0x77647468) {         // wdth
            d.widthMin = minimum;
            d.widthMax = maximum;
        } else if (tag == 0x736c6e74) {         // slnt, counterclockwise if positive
            d.obliqueMin = -maximum;
            d.obliqueMax = -minimum;
        } else if (tag == 0x6f70737a) {         // opsz
            d.opticalSizeMin = minimum;
            d.opticalSizeMax = maximum;
        } else if (tag == 0x6974616c) {         // ital
            d.italicAxis = true;
        }
    }
}

PrRawString sfntFamily(const Sfnt& sfnt) {
    PrRawString name = sfntName(sfnt, 16);
    if (prRawStringIsNull(&name))
        name = sfntName(sfnt, 1);
    return name;
}

bool sfntCmapUnicode(const Sfnt& sfnt, SfntCmap* out) {
    *out = SfntCmap{};
    SfntTable cmap = sfntTable(sfnt, kCmap);
    uint32_t count = entriesIn(cmap, 4, 8, cmap.uint16At(2));
    // The formats that FreeType makes charmaps of, and the encodings that it takes as Unicode.
    auto mapsCharacters = [](uint16_t format) {
        return format == 0 || format == 2 || format == 4 || format == 6 || format == 8 || format == 10 || format ==
            12 || format == 13 || format == 14;
    };
    auto unicode = [](uint16_t platform, uint16_t encoding) {
        return platform == 0 || platform == 2 || (platform == 3 && (encoding == 1 || encoding == 10));
    };
    int32_t chosen = -1;
    for (int32_t pass = 0; pass < 2 && chosen < 0; ++pass) {
        for (uint32_t i = count; i-- > 0 && chosen < 0;) {
            uint32_t record = 4 + i * 8;
            uint16_t platform = cmap.uint16At(record);
            uint16_t encoding = cmap.uint16At(record + 2);
            uint16_t format = cmap.from(cmap.uint32At(record + 4)).uint16At(0);
            if (!mapsCharacters(format) || !unicode(platform, encoding))
                continue;
            bool wide = (platform == 3 && encoding == 10) || (platform == 0 && encoding == 4) ||
                (platform == 0 && encoding == 6 && format == 13);
            if (pass == 1 || wide)
                chosen = int32_t(i);
        }
    }
    if (chosen < 0)
        return false;
    SfntTable subtable = cmap.from(cmap.uint32At(4 + uint32_t(chosen) * 8 + 4));
    uint16_t format = subtable.uint16At(0);
    if (format != 0 && format != 4 && format != 6 && format != 12 && format != 13)
        return false;
    out->subtable = subtable;
    out->format = format;
    return true;
}

uint32_t sfntCmapGlyph(const SfntCmap& cmap, uint32_t codePoint) {
    const SfntTable& t = cmap.subtable;
    switch (cmap.format) {
        case 0:
            return codePoint < 256 ? t.uint8At(6 + codePoint) : 0;
        case 4: {
            // Segments are sorted by their last code point. The first segment that ends at or after the code point
            // holds it, if that segment starts at or before it.
            if (codePoint > 0xffff)
                return 0;
            uint32_t segments = t.uint16At(6) / 2;
            uint32_t ends = 14;
            uint32_t starts = ends + segments * 2 + 2;
            uint32_t deltas = starts + segments * 2;
            uint32_t offsets = deltas + segments * 2;
            uint32_t low = partitionPoint(0, segments, [&](uint32_t i) { return t.uint16At(ends + i * 2) < codePoint; });
            if (low == segments || t.uint16At(starts + low * 2) > codePoint)
                return 0;
            uint16_t delta = t.uint16At(deltas + low * 2);
            uint16_t offset = t.uint16At(offsets + low * 2);
            if (offset == 0)
                return uint16_t(codePoint + delta);
            uint32_t at = offsets + low * 2 + offset + (codePoint - t.uint16At(starts + low * 2)) * 2;
            uint16_t glyph = t.uint16At(at);
            return glyph != 0 ? uint16_t(glyph + delta) : 0;
        }
        case 6: {
            uint32_t first = t.uint16At(6);
            uint32_t count = t.uint16At(8);
            return codePoint >= first && codePoint - first < count ? t.uint16At(10 + (codePoint - first) * 2) : 0;
        }
        case 12:
        case 13: {
            // Groups of consecutive code points, in order. The glyphs of a group are consecutive in format 12, and a
            // single glyph in format 13.
            uint32_t groups = entriesIn(t, 16, 12, t.uint32At(12));
            uint32_t low = partitionPoint(0, groups, [&](uint32_t i) { return t.uint32At(16 + i * 12 + 4) < codePoint; });
            uint32_t group = 16 + low * 12;
            if (low == groups || t.uint32At(group) > codePoint)
                return 0;
            uint32_t glyph = t.uint32At(group + 8);
            return cmap.format == 12 ? glyph + (codePoint - t.uint32At(group)) : glyph;
        }
        default: return 0;
    }
}

bool sfntGlyphImage(const Sfnt& sfnt, uint32_t glyph, float pixelsPerEm, SfntImage* out) {
    // Checks the image of the glyph in each strike, keeping the best.
    bool found = false;
    float best = 0;
    auto consider = [&](float ppem, const SfntImage& image) {
        bool better = !found || (best < pixelsPerEm ? ppem > best : ppem >= pixelsPerEm && ppem < best);
        if (better) {
            *out = image;
            out->pixelsPerEm = ppem;
            best = ppem;
            found = true;
        }
    };
    SfntTable sbix = sfntTable(sfnt, kSbix);
    uint32_t glyphCount = sfntTable(sfnt, kMaxp).uint16At(4);
    uint32_t strikes = entriesIn(sbix, 8, 4, sbix.uint32At(4));
    for (uint32_t i = 0; i < strikes; ++i) {
        SfntTable strike = sbix.from(sbix.uint32At(8 + i * 4));
        SfntImage image{};
        if (strike.uint16At(0) != 0 && sbixImage(strike, glyph, glyphCount, &image))
            consider(float(strike.uint16At(0)), image);
    }
    SfntTable cblc = sfntTable(sfnt, kCblc);
    SfntTable cbdt = sfntTable(sfnt, kCbdt);
    uint32_t sizes = cbdt.size != 0 ? entriesIn(cblc, 8, 48, cblc.uint32At(4)) : 0;
    for (uint32_t i = 0; i < sizes; ++i) {
        uint32_t record = 8 + i * 48;
        uint32_t start = cblc.uint16At(record + 40);
        uint32_t end = cblc.uint16At(record + 42);
        SfntImage image{};
        if (glyph >= start && glyph <= end && cblc.uint8At(record + 45) != 0 && cbdtImage(cblc, cbdt, record, glyph, &image))
            consider(float(cblc.uint8At(record + 45)), image);
    }
    return found;
}

bool sfntGlyphSvg(const Sfnt& sfnt, uint32_t glyph, SfntTable* document, uint32_t* index) {
    SfntTable svg = sfntTable(sfnt, kSvg);
    SfntTable list = svg.from(svg.uint32At(2));
    uint32_t count = entriesIn(list, 2, 12, list.uint16At(0));
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t record = 2 + i * 12;
        if (glyph < list.uint16At(record) || glyph > list.uint16At(record + 2))
            continue;
        SfntTable at = list.from(list.uint32At(record + 4));
        uint32_t length = list.uint32At(record + 8);
        if (at.data == nullptr || length == 0 || at.size < length)
            return false;
        *document = SfntTable{at.data, length};
        *index = i;
        return true;
    }
    return false;
}

} // namespace pr_impl
