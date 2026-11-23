// PSD and PSB decoding of the merged image.
//
// All integers are big-endian. A PSB file widens the lengths of the layer and mask information and the byte counts of
// PackBits rows.

#include "pr/psd.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/mem.h"

namespace pr_impl {

namespace {

// The image resources that the decoder reads.
constexpr uint32_t kResourceResolution = 0x03ed;
constexpr uint32_t kResourceThumbnail = 0x040c;
constexpr uint32_t kResourceProfile = 0x040f;
constexpr uint32_t kResourceVersion = 0x0421;
constexpr uint32_t kResourceExif = 0x0422;

// The compressions of the merged image.
constexpr uint32_t kCompressionRaw = 0;
constexpr uint32_t kCompressionPackBits = 1;

// The meters in an inch.
constexpr float kMetersPerInch = 0.0254f;

// A document: what the info says, and where the merged image is.
struct Document {
    PrByteView bytes;
    PrByteView table;               // the color table of Indexed, 768 bytes
    size_t dataAt;                  // the first byte after the compression
    uint32_t compression;
    uint32_t storedChannels;        // the channels that the file stores
    uint32_t readChannels;          // the channels that a read decodes: the colors and alpha
    uint32_t storedColors;          // the color channels that the file stores: one for Indexed
    uint64_t rowBytes;
    bool psb;
};

// Reads the image resources in `block` into `info`. Writes to `*merged` whether the version information says that the
// file has a real merged image, which is true without the resource.
void readResources(PrByteView block, PrPsdInfo* info, bool* merged) {
    *merged = true;
    size_t at = 0;
    while (block.size - at >= 12) {
        const uint8_t* entry = block.data + at;
        // Photoshop writes 8BIM. Other applications' signatures share the layout.
        if (!prMemEquals(entry, "8BIM", 4) && !prMemEquals(entry, "MeSa", 4) && !prMemEquals(entry, "AgHg", 4) &&
                !prMemEquals(entry, "PHUT", 4) && !prMemEquals(entry, "DCSR", 4))
            return;
        const uint32_t id = prMemReadBeU16(entry + 4);
        // The name is a Pascal string, padded to an even size with its length byte.
        const size_t name = (size_t(entry[6]) + 2) & ~size_t(1);
        if (block.size - at - 6 < name + 4)
            return;
        at += 6 + name;
        const uint64_t size = prMemReadBeU32(block.data + at);
        at += 4;
        if (size > block.size - at)
            return;
        const PrByteView data{block.data + at, size_t(size)};
        at += size_t(size);
        at += at % 2 != 0 && at < block.size ? 1 : 0;
        switch (id) {
            case kResourceProfile:
                info->profile = data;
                break;
            case kResourceExif:
                info->exif = data;
                break;
            case kResourceThumbnail:
                // The format, which is 1 for JPEG, then the extent and sizes of the thumbnail, and its JFIF data.
                if (data.size > 28 && prMemReadBeU32(data.data) == 1)
                    info->thumbnail = PrByteView{data.data + 28, data.size - 28};
                break;
            case kResourceResolution:
                // Fixed 16.16 resolutions, each with its unit: 1 for pixels an inch, and 2 for pixels a centimeter.
                if (data.size >= 16) {
                    const float scales[2] = {1 / kMetersPerInch, 100};
                    const uint32_t xUnit = prMemReadBeU16(data.data + 4);
                    const uint32_t yUnit = prMemReadBeU16(data.data + 12);
                    if ((xUnit == 1 || xUnit == 2) && (yUnit == 1 || yUnit == 2)) {
                        info->pixelsPerMeter = PrVec2{
                            float(prMemReadBeU32(data.data)) / 65536.0f * scales[xUnit - 1],
                            float(prMemReadBeU32(data.data + 8)) / 65536.0f * scales[yUnit - 1],
                        };
                    }
                }
                break;
            case kResourceVersion:
                // A version, then whether the file has a real merged image.
                if (data.size >= 5)
                    *merged = data.data[4] != 0;
                break;
            default:
                break;
        }
    }
}

// Reads the sections of the document in `bytes` up to its merged image, as prPsdDecoderCreate describes.
PrStatus readDocument(PrByteView bytes, Document* doc, PrPsdInfo* info) {
    if (!prPsdHasSignature(bytes) || bytes.size < kPrPsdHeaderBytes)
        return PrStatus_InvalidData;
    const uint8_t* at = bytes.data;
    doc->bytes = bytes;
    doc->psb = prMemReadBeU16(at + 4) == 2;
    doc->storedChannels = prMemReadBeU16(at + 12);
    info->extent = PrUVec2{prMemReadBeU32(at + 18), prMemReadBeU32(at + 14)};
    info->depth = uint16_t(prMemReadBeU16(at + 22));
    const uint32_t mode = prMemReadBeU16(at + 24);
    const uint32_t most = doc->psb ? 300000 : 30000;
    if (doc->storedChannels == 0 || doc->storedChannels > 56 || info->extent.x == 0 || info->extent.y == 0 ||
            info->extent.x > most || info->extent.y > most)
        return PrStatus_InvalidData;
    const uint32_t depth = info->depth;
    const bool deep = depth == 8 || depth == 16 || depth == 32;
    switch (mode) {
        case PrPsdMode_Bitmap:
            if (depth != 1)
                return PrStatus_InvalidData;
            info->colors = PrPsdColors_Gray;
            doc->storedColors = 1;
            break;
        case PrPsdMode_Grayscale:
        case PrPsdMode_Duotone:
            if (!deep)
                return PrStatus_InvalidData;
            info->colors = PrPsdColors_Gray;
            doc->storedColors = 1;
            break;
        case PrPsdMode_Indexed:
            if (depth != 8)
                return PrStatus_InvalidData;
            info->colors = PrPsdColors_Rgb;
            doc->storedColors = 1;
            break;
        case PrPsdMode_Rgb:
            if (!deep)
                return PrStatus_InvalidData;
            info->colors = PrPsdColors_Rgb;
            doc->storedColors = 3;
            break;
        case PrPsdMode_Cmyk:
            if (depth != 8 && depth != 16)
                return PrStatus_InvalidData;
            info->colors = PrPsdColors_Cmyk;
            doc->storedColors = 4;
            break;
        case PrPsdMode_Lab:
            if (depth != 8 && depth != 16)
                return PrStatus_InvalidData;
            info->colors = PrPsdColors_Lab;
            doc->storedColors = 3;
            break;
        case PrPsdMode_Multichannel:
            return PrStatus_NotSupported;
        default:
            return PrStatus_InvalidData;
    }
    if (doc->storedChannels < doc->storedColors)
        return PrStatus_InvalidData;
    info->mode = PrPsdMode(mode);

    // The color mode data, the image resources, and the layer and mask information, each after its length.
    size_t pos = kPrPsdHeaderBytes;
    auto section = [&](uint32_t lengthBytes, PrByteView* out) {
        if (bytes.size - pos < lengthBytes)
            return false;
        const uint64_t length = lengthBytes == 8 ? prMemReadBeU64(bytes.data + pos) : prMemReadBeU32(bytes.data + pos);
        pos += lengthBytes;
        if (length > bytes.size - pos)
            return false;
        *out = PrByteView{bytes.data + pos, size_t(length)};
        pos += size_t(length);
        return true;
    };
    PrByteView colorData{};
    PrByteView resources{};
    PrByteView layers{};
    if (!section(4, &colorData) || !section(4, &resources) || !section(doc->psb ? 8 : 4, &layers))
        return PrStatus_InvalidData;
    if (mode == PrPsdMode_Indexed) {
        if (colorData.size < 768)
            return PrStatus_InvalidData;
        doc->table = colorData;
    }
    bool merged = true;
    readResources(resources, info, &merged);
    if (!merged)
        return PrStatus_NotSupported;
    // The layer information's layer count is negative if the first extra channel holds the merged image's transparency.
    const uint32_t infoBytes = doc->psb ? 8 : 4;
    bool transparency = false;
    if (layers.size >= infoBytes + 2) {
        const uint64_t layerInfo = doc->psb ? prMemReadBeU64(layers.data) : prMemReadBeU32(layers.data);
        transparency = layerInfo >= 2 && int16_t(prMemReadBeU16(layers.data + infoBytes)) < 0;
    }
    info->alpha = transparency && doc->storedChannels > doc->storedColors;

    if (bytes.size - pos < 2)
        return PrStatus_InvalidData;
    doc->compression = prMemReadBeU16(bytes.data + pos);
    doc->dataAt = pos + 2;
    if (doc->compression != kCompressionRaw && doc->compression != kCompressionPackBits)
        return doc->compression <= 3 ? PrStatus_NotSupported : PrStatus_InvalidData;

    const uint32_t colorChannels = info->colors == PrPsdColors_Gray ? 1 : info->colors == PrPsdColors_Cmyk ? 4 : 3;
    info->channels = uint8_t(colorChannels + (info->alpha ? 1 : 0));
    info->sample = depth == 16 ? PrPsdSample_U16 : depth == 32 ? PrPsdSample_F32 : PrPsdSample_U8;
    if (info->colors == PrPsdColors_Lab)
        info->sample = PrPsdSample_F32;
    doc->readChannels = doc->storedColors + (info->alpha ? 1 : 0);
    doc->rowBytes = (uint64_t(info->extent.x) * depth + 7) / 8;
    return PrStatus_Ok;
}

uint32_t sampleBytes(PrPsdSample sample) {
    return sample == PrPsdSample_U8 ? 1 : sample == PrPsdSample_U16 ? 2 : 4;
}

// Writes the stored row `row` of channel `channel` into the pixels of `output` at row `y`.
void storeRow(const Document& doc, const PrPsdInfo& info, uint32_t channel, const uint8_t* row, uint8_t* out) {
    const uint32_t size = sampleBytes(info.sample);
    const uint32_t pixel = info.channels * size;
    // The output channel of a stored channel: the colors in turn, and the transparency last.
    const uint32_t to = channel < doc.storedColors ? channel : info.channels - 1u;
    const bool cmyk = info.colors == PrPsdColors_Cmyk;
    for (uint32_t x = 0; x < info.extent.x; ++x) {
        uint8_t* at = out + size_t(x) * pixel;
        if (info.depth == 1) {
            // A set bit is black.
            at[to] = (row[x / 8] >> (7 - x % 8) & 1) != 0 ? 0 : 255;
        } else if (info.mode == PrPsdMode_Indexed && channel == 0) {
            const uint32_t index = row[x];
            at[0] = doc.table.data[index];
            at[1] = doc.table.data[256 + index];
            at[2] = doc.table.data[512 + index];
        } else if (info.colors == PrPsdColors_Lab) {
            // L spans [0, 100], and a and b are offset by 128, at either depth. Extra channels are in [0, 1].
            const float stored = info.depth == 16 ? float(prMemReadBeU16(row + size_t(x) * 2)) : float(row[x]);
            const float most = info.depth == 16 ? 65535.0f : 255.0f;
            float value = stored / most;
            if (channel == 0)
                value = stored * 100.0f / most;
            else if (channel < 3)
                value = info.depth == 16 ? stored / 257.0f - 128.0f : stored - 128.0f;
            prMemCopy(at + to * 4, &value, 4);
        } else if (info.depth == 8) {
            at[to] = uint8_t(cmyk && channel < 4 ? 255 - row[x] : row[x]);
        } else if (info.depth == 16) {
            const uint32_t stored = prMemReadBeU16(row + size_t(x) * 2);
            const uint16_t value = uint16_t(cmyk && channel < 4 ? 65535 - stored : stored);
            prMemCopy(at + to * 2, &value, 2);
        } else {
            const uint32_t bits = prMemReadBeU32(row + size_t(x) * 4);
            prMemCopy(at + to * 4, &bits, 4);
        }
    }
}

// Writes zero to channel `channel` of row `y` of `output`.
void clearRow(const Document& doc, const PrPsdInfo& info, uint32_t channel, uint8_t* out) {
    const uint32_t size = sampleBytes(info.sample);
    const uint32_t pixel = info.channels * size;
    const uint32_t first = channel < doc.storedColors ? channel : info.channels - 1u;
    const uint32_t count = info.mode == PrPsdMode_Indexed && channel == 0 ? 3 : 1;
    for (uint32_t x = 0; x < info.extent.x; ++x)
        prMemZero(out + size_t(x) * pixel + first * size, size_t(count) * size);
}

// Unpacks one PackBits row of `rowBytes` bytes from `in` into `out`, and writes the bytes that it reads to `*read`.
// Returns InvalidData if a run passes the end of the row, and Truncated if the bytes end first.
PrStatus unpackRow(PrByteView in, uint8_t* out, uint64_t rowBytes, size_t* read) {
    size_t at = 0;
    uint64_t written = 0;
    *read = 0;
    while (written < rowBytes) {
        if (at >= in.size)
            return PrStatus_Truncated;
        const int32_t header = int8_t(in.data[at++]);
        if (header >= 0) {
            const uint64_t count = uint64_t(header) + 1;
            if (count > rowBytes - written)
                return PrStatus_InvalidData;
            if (count > in.size - at)
                return PrStatus_Truncated;
            prMemCopy(out + written, in.data + at, size_t(count));
            at += size_t(count);
            written += count;
        } else if (header != -128) {
            const uint64_t count = uint64_t(1 - header);
            if (count > rowBytes - written)
                return PrStatus_InvalidData;
            if (at >= in.size)
                return PrStatus_Truncated;
            for (uint64_t i = 0; i < count; ++i)
                out[written + i] = in.data[at];
            ++at;
            written += count;
        }
    }
    *read = at;
    return PrStatus_Ok;
}

// Unmattes the colors of `output` from the white that Photoshop composites the merged image over.
void unmatte(const PrPsdInfo& info, PrPixels output) {
    const uint32_t colors = info.channels - 1u;
    for (uint32_t y = 0; y < info.extent.y; ++y) {
        uint8_t* row = output.data + size_t(y) * output.stride;
        for (uint32_t x = 0; x < info.extent.x; ++x) {
            float values[4];
            for (uint32_t c = 0; c <= colors; ++c) {
                if (info.sample == PrPsdSample_U8) {
                    values[c] = float(row[size_t(x) * info.channels + c]) / 255.0f;
                } else if (info.sample == PrPsdSample_U16) {
                    uint16_t v = 0;
                    prMemCopy(&v, row + (size_t(x) * info.channels + c) * 2, 2);
                    values[c] = float(v) / 65535.0f;
                } else {
                    prMemCopy(&values[c], row + (size_t(x) * info.channels + c) * 4, 4);
                }
            }
            const float alpha = values[colors];
            if (!(alpha > 0) || alpha >= 1)
                continue;
            for (uint32_t c = 0; c < colors; ++c) {
                float v = (values[c] - (1 - alpha)) / alpha;
                v = v < 0 ? 0 : v;
                if (info.sample == PrPsdSample_U8) {
                    row[size_t(x) * info.channels + c] = uint8_t((v > 1 ? 1 : v) * 255.0f + 0.5f);
                } else if (info.sample == PrPsdSample_U16) {
                    const uint16_t u = uint16_t((v > 1 ? 1 : v) * 65535.0f + 0.5f);
                    prMemCopy(row + (size_t(x) * info.channels + c) * 2, &u, 2);
                } else {
                    prMemCopy(row + (size_t(x) * info.channels + c) * 4, &v, 4);
                }
            }
        }
    }
}

} // namespace

// A document open for reading, and the row that a read unpacks into.
struct PsdDecoder {
    Document doc;
    PrPsdInfo info;
    uint8_t* PR_NULLABLE row;
};

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrPsdDecoder : PsdDecoder {};

bool prPsdHasSignature(PrByteView bytes) {
    return bytes.size >= 6 && prMemEquals(bytes.data, "8BPS", 4) &&
        (prMemReadBeU16(bytes.data + 4) == 1 || prMemReadBeU16(bytes.data + 4) == 2);
}

PrPsdLimits prPsdLimitsDefault(void) {
    return PrPsdLimits{
        .pixels = 64u * 1024u * 1024u,
        .workingBytes = 512u * 1024u * 1024u,
        .outputBytes = 512u * 1024u * 1024u,
        .work = 1000000000u,
    };
}

PrStatus prPsdDecoderCreate(PrByteView bytes, const PrPsdLimits* PR_NULLABLE limits, uint64_t held,
        PrPsdDecoder* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    const PrPsdLimits bounds = limits != nullptr ? *limits : prPsdLimitsDefault();
    Document doc{};
    PrPsdInfo info{};
    PrStatus status = readDocument(bytes, &doc, &info);
    if (status != PrStatus_Ok)
        return status;
    const uint64_t pixels = uint64_t(info.extent.x) * info.extent.y;
    // A read holds one unpacked row of one channel.
    info.workingBytes = held + sizeof(PrPsdDecoder) + doc.rowBytes;
    if (pixels > bounds.pixels || pixels * doc.readChannels > bounds.work ||
            pixels * info.channels * sampleBytes(info.sample) > bounds.outputBytes ||
            info.workingBytes > bounds.workingBytes)
        return PrStatus_Limit;
    PrPsdDecoder* decoder = allocZeroed<PrPsdDecoder>();
    decoder->doc = doc;
    decoder->info = info;
    decoder->row = static_cast<uint8_t*>(prMemoryAlloc(size_t(doc.rowBytes)));
    *out = decoder;
    return PrStatus_Ok;
}

void prPsdDecoderDestroy(PrPsdDecoder* PR_NULLABLE decoder) {
    if (decoder == nullptr)
        return;
    prMemoryFree(decoder->row);
    prMemoryFree(decoder);
}

const PrPsdInfo* prPsdDecoderInfo(const PrPsdDecoder* decoder) {
    return &decoder->info;
}

PrStatus prPsdDecoderRead(PrPsdDecoder* decoder, PrPixels output) {
    const Document& doc = decoder->doc;
    const PrPsdInfo& info = decoder->info;
    PR_ASSERT(output.extent == info.extent);
    const PrByteView bytes = doc.bytes;
    const uint32_t height = info.extent.y;
    PrStatus status = PrStatus_Ok;
    // A PackBits image begins with the byte count of each row of each channel, and the rows follow in that order.
    const uint32_t countBytes = doc.psb ? 4 : 2;
    const uint64_t rows = uint64_t(doc.storedChannels) * height;
    uint64_t at = doc.dataAt;
    if (doc.compression == kCompressionPackBits)
        at += rows * countBytes;
    for (uint32_t channel = 0; channel < doc.readChannels; ++channel) {
        for (uint32_t y = 0; y < height; ++y) {
            uint8_t* out = output.data + size_t(y) * output.stride;
            const uint8_t* row = nullptr;
            if (doc.compression == kCompressionRaw) {
                const uint64_t start = doc.dataAt + (uint64_t(channel) * height + y) * doc.rowBytes;
                if (start <= bytes.size && bytes.size - start >= doc.rowBytes)
                    row = bytes.data + start;
            } else {
                const uint64_t entry = doc.dataAt + (uint64_t(channel) * height + y) * countBytes;
                if (entry + countBytes <= bytes.size) {
                    uint64_t count = prMemReadBeU16(bytes.data + entry);
                    if (countBytes == 4)
                        count = prMemReadBeU32(bytes.data + entry);
                    // A row begins where the one before it ends. A count that is too small is taken as wrong, as some
                    // writers' counts are, and a larger one as padding after the row.
                    if (at <= bytes.size) {
                        size_t read = 0;
                        const PrStatus unpacked =
                            unpackRow(PrByteView{bytes.data + at, size_t(bytes.size - at)}, decoder->row, doc.rowBytes, &read);
                        if (unpacked == PrStatus_InvalidData)
                            return unpacked;
                        if (unpacked == PrStatus_Ok)
                            row = decoder->row;
                        at += count >= read && count <= bytes.size - at ? count : read;
                    }
                }
            }
            if (row == nullptr) {
                clearRow(doc, info, channel, out);
                status = PrStatus_Truncated;
                continue;
            }
            storeRow(doc, info, channel, row, out);
        }
    }
    if (status == PrStatus_Ok && info.alpha && (info.colors == PrPsdColors_Gray || info.colors == PrPsdColors_Rgb))
        unmatte(info, output);
    return status;
}
