// The built-in image backend.

#include "pr/image_builtin.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/mem.h"
#include "pr/string_ops.h"

#include "image/backends/builtin/src/gif.h"
#include "image/backends/builtin/src/ico.h"
#include "image/backends/builtin/src/jpeg.h"
#include "image/backends/builtin/src/metadata.h"
#include "image/backends/builtin/src/png.h"
#include "image/backends/builtin/src/tiff.h"

namespace pr_impl {

namespace {

constexpr uint8_t kPngSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

bool supportsType(PrImageCodec*, PrStringView type) {
    return type == "image/png" || type == "image/apng" || type == "image/jpeg" || type == "image/gif" ||
        type == "image/tiff" || type == "image/x-icon" || type == "image/vnd.microsoft.icon";
}

enum {
    BuiltinKind_Png,
    BuiltinKind_Jpeg,
    BuiltinKind_Gif,
    BuiltinKind_Ico,
    BuiltinKind_Tiff,
};
typedef uint8_t BuiltinKind;

struct BuiltinBackend : PrImageCodec {
    PrCompressCodec* compressCodec;
};

struct BuiltinDecoder : PrImageReader {
    PrCompressCodec* compressCodec;
    uint8_t* bytes;
    uint8_t* scratch;
    size_t size;
    size_t rowBytes;
    size_t pixelsSize;
    PrImageInfo info;
    GifAnimation* gif;
    PngAnimation* png;
    IcoImage icon;
    TiffImage tiff;
    uint8_t* PR_NULLABLE profile;   // the ICC profile that the image's color names, expanded
    BuiltinKind kind;
    PrStatus openStatus;
};

bool startsWith(PrByteView bytes, PrByteView prefix) {
    return bytes.size >= prefix.size && prMemEquals(bytes.data, prefix.data, prefix.size);
}

bool isType(const uint8_t* bytes, const char* type) {
    return bytes[0] == uint8_t(type[0]) && bytes[1] == uint8_t(type[1]) &&
        bytes[2] == uint8_t(type[2]) && bytes[3] == uint8_t(type[3]);
}

bool addChecked(uint64_t a, uint64_t b, uint64_t* out) {
    if (a > 0xFFFFFFFFFFFFFFFFull - b) {
        return false;
    }
    *out = a + b;
    return true;
}

bool multiplyChecked(uint64_t a, uint64_t b, uint64_t* out) {
    if (a != 0 && b > 0xFFFFFFFFFFFFFFFFull / a) {
        return false;
    }
    *out = a * b;
    return true;
}

PrImageColor unknownColor() {
    PrImageColor color = {};
    color.encoding = PrImageColorEncoding_Unknown;
    return color;
}

PrImageColor srgbColor() {
    PrImageColor color = {};
    color.encoding = PrImageColorEncoding_Named;
    color.space = PrColorSpace_Srgb;
    return color;
}

uint32_t colorChannels(PrColorModel model) {
    if (model == PrColorModel_Gray) {
        return 1;
    }
    return model == PrColorModel_Cmyk ? 4 : 3;
}

uint32_t formatChannels(PrImageFormat format) {
    return colorChannels(format.model) + (format.alpha == PrImageAlpha_None ? 0 : 1);
}

PrStatus finishInfo(PrImageInfo* info, const PrImageLimits& limits, uint64_t inputSize, uint64_t factor,
        uint64_t fixedBytes) {
    uint64_t pixels = 0;
    uint64_t rowBytes = 0;
    uint64_t output = 0;
    const uint64_t sampleBytes = info->format.sample == PrImageSample_U16 ? 2 : 1;
    if (info->frameCount > limits.frames || !multiplyChecked(info->extent.x, info->extent.y, &pixels) ||
            pixels > limits.pixels ||
            !multiplyChecked(info->extent.x, uint64_t(formatChannels(info->format)) * sampleBytes,
                &rowBytes) || !multiplyChecked(rowBytes, info->extent.y, &output) || output > limits.outputBytes) {
        return PrStatus_Limit;
    }
    uint64_t working = 0;
    uint64_t scaled = 0;
    if (!multiplyChecked(pixels, factor, &scaled) || !addChecked(inputSize, scaled, &working) ||
            !addChecked(working, fixedBytes, &working) || working > limits.workingBytes) {
        return PrStatus_Limit;
    }
    info->workingBytes = working;
    return PrStatus_Ok;
}

PrStatus inspectPng(PrCompressCodec* compressCodec, const uint8_t* bytes, size_t size,
        const PrImageLimits& limits, PrImageInfo* info) {
    if (limits.work == 0) {
        return PrStatus_Limit;
    }
    if (size < 29) {
        return PrStatus_InvalidData;
    }
    if (prMemReadBeU32(bytes + 8) != 13 || !isType(bytes + 12, "IHDR")) {
        return PrStatus_InvalidData;
    }
    info->extent.x = prMemReadBeU32(bytes + 16);
    info->extent.y = prMemReadBeU32(bytes + 20);
    const uint8_t depth = bytes[24];
    const uint8_t colorType = bytes[25];
    if (info->extent.x == 0 || info->extent.y == 0) {
        return PrStatus_InvalidData;
    }
    const bool depthValid = (colorType == 0 && (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)) ||
        (colorType == 2 && (depth == 8 || depth == 16)) ||
        (colorType == 3 && (depth == 1 || depth == 2 || depth == 4 || depth == 8)) ||
        (colorType == 4 && (depth == 8 || depth == 16)) || (colorType == 6 && (depth == 8 || depth == 16));
    if (!depthValid || bytes[26] != 0 || bytes[27] != 0 || bytes[28] > 1) {
        return PrStatus_NotSupported;
    }
    info->format.model = colorType == 0 || colorType == 4 ? PrColorModel_Gray : PrColorModel_Rgb;
    info->format.sample = depth == 16 ? PrImageSample_U16 : PrImageSample_U8;
    const bool explicitAlpha = colorType == 4 || colorType == 6;
    info->format.alpha = explicitAlpha ? PrImageAlpha_Straight : PrImageAlpha_None;
    info->format.color = unknownColor();
    info->frameCount = 1;
    info->playCount = 1;
    if (size < 33) {
        uint64_t retainedAndChunks = 0;
        if (!multiplyChecked(size, 8, &retainedAndChunks)) {
            return PrStatus_Limit;
        }
        const PrStatus bounded = finishInfo(info, limits, retainedAndChunks, 32, 2u << 20);
        return bounded == PrStatus_Ok ? PrStatus_Truncated : bounded;
    }
    bool palette = false;
    bool transparency = false;
    bool data = false;
    bool end = false;
    bool namedSrgb = false;
    bool hasIcc = false;
    uint64_t metadata = 0;
    uint64_t work = 1;
    size_t at = 33;
    while (at < size) {
        if (++work > limits.work) {
            return PrStatus_Limit;
        }
        if (size - at < 12) {
            break;
        }
        const uint32_t length = prMemReadBeU32(bytes + at);
        if (length > 0x7FFFFFFFu || size_t(length) + 12 > size - at) {
            break;
        }
        const uint8_t* type = bytes + at + 4;
        if (isType(type, "IHDR")) {
            return PrStatus_InvalidData;
        }
        if (isType(type, "PLTE")) {
            palette = length >= 3 && length <= 768 && length % 3 == 0;
            if (!palette) {
                return PrStatus_InvalidData;
            }
        } else if (isType(type, "tRNS")) {
            transparency = length != 0;
        } else if (isType(type, "IDAT")) {
            data = true;
        } else if (isType(type, "fdAT")) {
            // Animation pixels count against input and decode work, not metadata.
        } else if (isType(type, "IEND")) {
            end = length == 0;
            if (!end) {
                return PrStatus_InvalidData;
            }
            at += size_t(length) + 12;
            break;
        } else if (isType(type, "iCCP")) {
            hasIcc = true;
            if (!addChecked(metadata, length, &metadata) || metadata > limits.metadataBytes) {
                return PrStatus_Limit;
            }
        } else {
            if (!addChecked(metadata, length, &metadata) || metadata > limits.metadataBytes) {
                return PrStatus_Limit;
            }
            namedSrgb = namedSrgb || isType(type, "sRGB");
        }
        at += size_t(length) + 12;
    }
    if (colorType == 3 && !palette && end) {
        return PrStatus_InvalidData;
    }
    info->format.alpha = explicitAlpha || transparency ? PrImageAlpha_Straight : PrImageAlpha_None;
    info->format.color = namedSrgb && !hasIcc ? srgbColor() : unknownColor();
    // Coding-independent code points name the space, and take precedence over a profile.
    PrColorSpace named = pngCicpSpace(compressCodec, PrByteView{bytes, size});
    if (named != PrColorSpace_Count) {
        info->format.color.encoding = PrImageColorEncoding_Named;
        info->format.color.space = named;
    }
    uint64_t pixelWork = 0;
    uint64_t retainedAndChunks = 0;
    if (!multiplyChecked(uint64_t(info->extent.x) * info->extent.y, formatChannels(info->format), &pixelWork) ||
            !addChecked(pixelWork, size, &pixelWork) || pixelWork > limits.work ||
            !multiplyChecked(size, 8, &retainedAndChunks)) {
        return PrStatus_Limit;
    }
    const PrStatus bounded = finishInfo(info, limits, retainedAndChunks, 32, 2u << 20);
    if (bounded != PrStatus_Ok)
        return bounded;
    const uint64_t metadataLeft = limits.metadataBytes - metadata;
    const uint64_t workLeft = limits.work - pixelWork;
    const uint64_t textLimit = metadataLeft < workLeft ? metadataLeft : workLeft;
    ImageMetadata imageMetadata = {};
    const PrStatus status = pngMetadata(compressCodec, {bytes, size}, size_t(textLimit), &imageMetadata);
    if (status != PrStatus_Ok)
        return status;
    info->orientation = imageMetadata.orientation;
    info->pixelsPerMeter.x = imageMetadata.pixelsPerMeterX;
    info->pixelsPerMeter.y = imageMetadata.pixelsPerMeterY;
    return end && data ? PrStatus_Ok : PrStatus_Truncated;
}

bool jpegStandalone(uint8_t marker) {
    return marker == 0x00 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD9);
}

bool jpegFrame(uint8_t marker) {
    return marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
}

PrStatus inspectJpeg(const uint8_t* bytes, size_t size, const PrImageLimits& limits, PrImageInfo* info) {
    if (limits.work == 0) {
        return PrStatus_Limit;
    }
    size_t at = 2;
    uint32_t components = 0;
    bool frame = false;
    bool scan = false;
    bool end = false;
    uint64_t scans = 0;
    uint64_t metadata = 0;
    uint64_t work = 0;
    while (at < size) {
        if (++work > limits.work) {
            return PrStatus_Limit;
        }
        while (at < size && bytes[at] != 0xFF) {
            ++at;
        }
        while (at < size && bytes[at] == 0xFF) {
            ++at;
        }
        if (at >= size) {
            break;
        }
        const uint8_t marker = bytes[at++];
        if (marker == 0xD9) {
            end = true;
            break;
        }
        if (jpegStandalone(marker)) {
            continue;
        }
        if (size - at < 2) {
            break;
        }
        const uint32_t length = prMemReadBeU16(bytes + at);
        if (length < 2) {
            return PrStatus_InvalidData;
        }
        if (length > size - at) {
            break;
        }
        const uint8_t* segment = bytes + at + 2;
        const uint32_t segmentSize = length - 2;
        if (jpegFrame(marker)) {
            if (marker != 0xC0 && marker != 0xC1 && marker != 0xC2) {
                return PrStatus_NotSupported;
            }
            if (frame || segmentSize < 6 || segment[0] != 8) {
                return segmentSize >= 1 && segment[0] != 8 ? PrStatus_NotSupported : PrStatus_InvalidData;
            }
            info->extent.y = prMemReadBeU16(segment + 1);
            info->extent.x = prMemReadBeU16(segment + 3);
            components = segment[5];
            if (info->extent.x == 0 || info->extent.y == 0 || (components != 1 && components != 3 && components != 4) ||
                    segmentSize < 6 + components * 3) {
                return PrStatus_InvalidData;
            }
            frame = true;
        } else if (marker == 0xDA) {
            scan = true;
            scans += 1;
        }
        if ((marker >= 0xE0 && marker <= 0xEF) || marker == 0xFE) {
            if (!addChecked(metadata, segmentSize, &metadata) || metadata > limits.metadataBytes) {
                return PrStatus_Limit;
            }
        }
        at += length;
    }
    if (!frame) {
        return PrStatus_InvalidData;
    }
    info->format.model = components == 1   ? PrColorModel_Gray : components == 4 ? PrColorModel_Cmyk : PrColorModel_Rgb;
    info->format.sample = PrImageSample_U8;
    info->format.alpha = PrImageAlpha_None;
    info->format.color = unknownColor();
    info->frameCount = 1;
    info->playCount = 1;
    ImageMetadata imageMetadata = {};
    jpegMetadata({bytes, size}, &imageMetadata);
    info->orientation = imageMetadata.orientation;
    info->pixelsPerMeter.x = imageMetadata.pixelsPerMeterX;
    info->pixelsPerMeter.y = imageMetadata.pixelsPerMeterY;
    uint64_t paddedArea = 0;
    uint64_t pixelWork = 0;
    uint64_t scanWork = 0;
    if (limits.work == 0 || !multiplyChecked(uint64_t(info->extent.x) + 31, uint64_t(info->extent.y) + 31, &paddedArea) ||
            !multiplyChecked(paddedArea, components, &pixelWork) ||
            !multiplyChecked(pixelWork, scans == 0 ? 1 : scans, &scanWork) ||
            !addChecked(scanWork, size, &scanWork) || scanWork > limits.work) {
        return PrStatus_Limit;
    }
    uint64_t decodeStorage = 0;
    if (!multiplyChecked(paddedArea, 16, &decodeStorage) || !addChecked(decodeStorage, size, &decodeStorage)) {
        return PrStatus_Limit;
    }
    const PrStatus bounded = finishInfo(info, limits, decodeStorage, 0, 1u << 20);
    return bounded == PrStatus_Ok ? (end && scan ? PrStatus_Ok : PrStatus_Truncated) : bounded;
}

bool skipGifBlocks(const uint8_t* bytes, size_t size, size_t* at) {
    while (*at < size) {
        const uint32_t count = bytes[(*at)++];
        if (count == 0) {
            return true;
        }
        if (count > size - *at) {
            return false;
        }
        *at += count;
    }
    return false;
}

PrStatus inspectGif(const uint8_t* bytes, size_t size, const PrImageLimits& limits, PrImageInfo* info) {
    if (limits.work == 0) {
        return PrStatus_Limit;
    }
    if (size < 13) {
        return PrStatus_InvalidData;
    }
    info->extent.x = prMemReadLeU16(bytes + 6);
    info->extent.y = prMemReadLeU16(bytes + 8);
    if (info->extent.x == 0 || info->extent.y == 0) {
        return PrStatus_InvalidData;
    }
    size_t at = 13;
    const uint8_t packed = bytes[10];
    if ((packed & 0x80) != 0) {
        const size_t paletteBytes = size_t(2u << (packed & 7)) * 3;
        if (paletteBytes > size - at) {
            return PrStatus_InvalidData;
        }
        at += paletteBytes;
    }
    uint32_t frames = 0;
    uint32_t playCount = 1;
    bool end = false;
    uint64_t metadata = 0;
    uint64_t work = 0;
    uint64_t maxFrameArea = 0;
    uint64_t decodeWork = 0;
    uint8_t disposal = 0;
    if (!addChecked(size, uint64_t(info->extent.x) * info->extent.y, &decodeWork)) {
        return PrStatus_Limit;
    }
    if (limits.work == 0 || decodeWork > limits.work) {
        return PrStatus_Limit;
    }
    while (at < size) {
        if (++work > limits.work) {
            return PrStatus_Limit;
        }
        const uint8_t block = bytes[at++];
        if (block == 0x3B) {
            end = true;
            break;
        }
        if (block == 0x21) {
            if (at >= size) {
                break;
            }
            const uint8_t label = bytes[at++];
            const size_t start = at;
            if (label == 0xFF && size - at >= 16 && bytes[at] == 11 &&
                    prMemEquals(bytes + at + 1, "NETSCAPE2.0", 11) && bytes[at + 12] == 3 && bytes[at + 13] == 1) {
                const uint32_t repeats = prMemReadLeU16(bytes + at + 14);
                // NETSCAPE2.0 counts the repeats after the first play. Zero means indefinitely.
                playCount = repeats == 0 ? 0 : repeats + 1;
            }
            if (label == 0xF9 && size - at >= 6 && bytes[at] == 4) {
                disposal = (bytes[at + 1] >> 2) & 7;
            }
            if (!skipGifBlocks(bytes, size, &at)) {
                break;
            }
            if (!addChecked(metadata, at - start, &metadata) || metadata > limits.metadataBytes) {
                return PrStatus_Limit;
            }
            continue;
        }
        if (block != 0x2C) {
            return PrStatus_InvalidData;
        }
        if (size - at < 9) {
            break;
        }
        const uint32_t width = prMemReadLeU16(bytes + at + 4);
        const uint32_t height = prMemReadLeU16(bytes + at + 6);
        const uint8_t imagePacked = bytes[at + 8];
        if (width == 0 || height == 0) {
            return PrStatus_InvalidData;
        }
        uint64_t frameWork = 0;
        if (!multiplyChecked(width, height, &frameWork) || frameWork > limits.pixels ||
                !multiplyChecked(frameWork, 2, &frameWork) ||
                !addChecked(decodeWork, frameWork, &decodeWork) || decodeWork > limits.work) {
            return PrStatus_Limit;
        }
        if (disposal == 3) {
            uint64_t snapshotWork = 0;
            if (!multiplyChecked(uint64_t(info->extent.x) * info->extent.y, 2, &snapshotWork) ||
                    !addChecked(decodeWork, snapshotWork, &decodeWork) || decodeWork > limits.work) {
                return PrStatus_Limit;
            }
        }
        disposal = 0;
        const uint64_t frameArea = uint64_t(width) * height;
        maxFrameArea = frameArea > maxFrameArea ? frameArea : maxFrameArea;
        at += 9;
        if ((imagePacked & 0x80) != 0) {
            const size_t paletteBytes = size_t(2u << (imagePacked & 7)) * 3;
            if (paletteBytes > size - at) {
                break;
            }
            at += paletteBytes;
        }
        if (at >= size) {
            break;
        }
        ++at;
        if (!skipGifBlocks(bytes, size, &at)) {
            break;
        }
        if (++frames > limits.frames) {
            return PrStatus_Limit;
        }
    }
    info->format.model = PrColorModel_Rgb;
    info->format.sample = PrImageSample_U8;
    info->format.alpha = PrImageAlpha_Straight;
    info->format.color = srgbColor();
    info->frameCount = frames == 0 ? 1 : frames;
    info->playCount = playCount;
    uint64_t frameBytes = 0;
    uint64_t extra = 0;
    if (!multiplyChecked(frames, 256, &frameBytes) || !multiplyChecked(maxFrameArea, 2, &extra) ||
            !addChecked(extra, frameBytes, &extra) || !addChecked(extra, 2u << 20, &extra)) {
        return PrStatus_Limit;
    }
    const PrStatus bounded = finishInfo(info, limits, size, 32, extra);
    return bounded == PrStatus_Ok ? (end && frames != 0 ? PrStatus_Ok : PrStatus_Truncated) : bounded;
}

PrStatus inspect(PrCompressCodec* compressCodec, BuiltinKind kind, const uint8_t* bytes, size_t size,
        const PrImageLimits& limits, PrImageInfo* info) {
    *info = {};
    if (kind == BuiltinKind_Png) {
        return inspectPng(compressCodec, bytes, size, limits, info);
    }
    if (kind == BuiltinKind_Jpeg) {
        return inspectJpeg(bytes, size, limits, info);
    }
    return inspectGif(bytes, size, limits, info);
}

PrStatus inspectTiff(BuiltinDecoder& decoder, const PrImageLimits& limits) {
    if (limits.work == 0)
        return PrStatus_Limit;
    PrStatus status = tiffImage({decoder.bytes, decoder.size}, &decoder.tiff);
    if (status != PrStatus_Ok)
        return status;
    const TiffImage& tiff = decoder.tiff;
    PrImageInfo& info = decoder.info;
    info.extent.x = tiff.width;
    info.extent.y = tiff.height;
    info.format.model = tiff.gray ? PrColorModel_Gray : PrColorModel_Rgb;
    info.format.sample = PrImageSample_U8;
    info.format.alpha = tiff.alpha;
    info.format.color = unknownColor();
    info.frameCount = 1;
    info.playCount = 1;
    if (uint64_t(tiff.width) * tiff.height > limits.work)
        return PrStatus_Limit;
    return finishInfo(&info, limits, decoder.size, 0, sizeof(BuiltinDecoder));
}

bool isIcon(const uint8_t* bytes, size_t size) {
    return size >= 6 && prMemReadLeU16(bytes) == 0 && prMemReadLeU16(bytes + 2) == 1;
}

PrStatus inspectIcon(BuiltinDecoder& decoder, const PrImageLimits& limits) {
    if (decoder.size >= 6 && prMemReadLeU16(decoder.bytes + 4) > limits.work) {
        return PrStatus_Limit;
    }
    if (!icoSelectImage({decoder.bytes, decoder.size}, &decoder.icon)) {
        return PrStatus_InvalidData;
    }
    const IcoImage& icon = decoder.icon;
    if (icon.png) {
        const PrStatus status = inspectPng(decoder.compressCodec, icon.bytes, icon.size, limits, &decoder.info);
        if (status != PrStatus_Ok && status != PrStatus_Truncated) {
            return status;
        }
        const uint64_t retained = decoder.size - icon.size;
        if (retained > limits.workingBytes - decoder.info.workingBytes) {
            return PrStatus_Limit;
        }
        decoder.info.workingBytes += retained;
        return status;
    }
    PrImageInfo& info = decoder.info;
    info.extent.x = icon.width;
    info.extent.y = icon.height;
    info.format.model = PrColorModel_Rgb;
    info.format.sample = PrImageSample_U8;
    info.format.alpha = PrImageAlpha_Straight;
    info.format.color = srgbColor();
    info.frameCount = 1;
    info.playCount = 1;
    if (uint64_t(icon.width) * icon.height * 2 > limits.work) {
        return PrStatus_Limit;
    }
    return finishInfo(&info, limits, decoder.size, 8, sizeof(BuiltinDecoder));
}

bool recognize(PrImageCodec*, PrByteView prefix) {
    const uint8_t jpeg[2] = {0xFF, 0xD8};
    return startsWith(prefix, {kPngSignature, sizeof(kPngSignature)}) ||
        startsWith(prefix, {jpeg, sizeof(jpeg)}) || gifHasSignature(prefix) || tiffHasSignature(prefix) ||
        isIcon(prefix.data, prefix.size);
}

PrStatus open(PrImageCodec* imageCodec, const PrImageSource* source, const PrImageLimits* limits,
        PrImageReader* PR_NULLABLE* PR_NONNULL outReader, PrImageInfo* outInfo) {
    *outReader = nullptr;
    if (prImageSourceSize(source) > limits->inputBytes || prImageSourceSize(source) > size_t(-1) ||
            prImageSourceSize(source) > limits->workingBytes ||
            sizeof(BuiltinDecoder) > limits->workingBytes - prImageSourceSize(source)) {
        return PrStatus_Limit;
    }
    BuiltinBackend* backend = static_cast<BuiltinBackend*>(imageCodec);
    BuiltinDecoder* decoder = allocZeroed<BuiltinDecoder>();
    decoder->codec = imageCodec;
    decoder->compressCodec = backend->compressCodec;
    decoder->size = size_t(prImageSourceSize(source));
    decoder->bytes = static_cast<uint8_t*>(prMemoryAlloc(decoder->size));
    const PrStatus read = prImageSourceRead(source, 0, decoder->bytes, decoder->size);
    if (read != PrStatus_Ok) {
        prMemoryFree(decoder->bytes);
        prMemoryFree(decoder);
        return read;
    }
    const uint8_t jpeg[2] = {0xFF, 0xD8};
    if (startsWith({decoder->bytes, decoder->size}, {kPngSignature, sizeof(kPngSignature)}))
        decoder->kind = BuiltinKind_Png;
    else if (startsWith({decoder->bytes, decoder->size}, {jpeg, sizeof(jpeg)}))
        decoder->kind = BuiltinKind_Jpeg;
    else if (tiffHasSignature({decoder->bytes, decoder->size}))
        decoder->kind = BuiltinKind_Tiff;
    else if (isIcon(decoder->bytes, decoder->size))
        decoder->kind = BuiltinKind_Ico;
    else
        decoder->kind = BuiltinKind_Gif;
    PrStatus status;
    if (decoder->kind == BuiltinKind_Ico)
        status = inspectIcon(*decoder, *limits);
    else if (decoder->kind == BuiltinKind_Tiff)
        status = inspectTiff(*decoder, *limits);
    else
        status = inspect(decoder->compressCodec, decoder->kind, decoder->bytes, decoder->size, *limits, &decoder->info);
    if (status != PrStatus_Ok && status != PrStatus_Truncated) {
        prMemoryFree(decoder->bytes);
        prMemoryFree(decoder);
        return status;
    }
    if (decoder->info.extent.x == 0 || decoder->info.extent.y == 0 || decoder->info.frameCount == 0) {
        prMemoryFree(decoder->bytes);
        prMemoryFree(decoder);
        return PrStatus_Truncated;
    }
    // A PNG's profile describes its color unless code points name its space.
    if (decoder->kind == BuiltinKind_Png && decoder->info.format.color.encoding == PrImageColorEncoding_Unknown) {
        size_t profileSize = 0;
        if (pngIccProfile(decoder->compressCodec, {decoder->bytes, decoder->size}, limits->metadataBytes,
                &decoder->profile, &profileSize))
            decoder->info.format.color = PrImageColor{
                .encoding = PrImageColorEncoding_Icc,
                .profile = PrByteView{decoder->profile, profileSize},
            };
    }
    if (decoder->kind == BuiltinKind_Png) {
        const PrStatus animated = pngAnimationOpen(decoder->compressCodec, {decoder->bytes, decoder->size}, *limits,
            decoder->info, &decoder->png);
        if (animated != PrStatus_Ok) {
            prMemoryFree(decoder->profile);
            prMemoryFree(decoder->bytes);
            prMemoryFree(decoder);
            return animated;
        }
    }
    decoder->rowBytes = size_t(decoder->info.extent.x) * formatChannels(decoder->info.format) *
        (decoder->info.format.sample == PrImageSample_U16 ? 2 : 1);
    decoder->pixelsSize = decoder->rowBytes * decoder->info.extent.y;
    decoder->scratch = static_cast<uint8_t*>(prMemoryAlloc(decoder->pixelsSize));
    decoder->openStatus = status;
    if (decoder->kind == BuiltinKind_Gif) {
        decoder->gif = gifAnimationOpen({decoder->bytes, decoder->size});
        if (decoder->gif == nullptr && status != PrStatus_Truncated) {
            prMemoryFree(decoder->scratch);
            prMemoryFree(decoder->profile);
            prMemoryFree(decoder->bytes);
            prMemoryFree(decoder);
            return PrStatus_InvalidData;
        }
    }
    *outInfo = decoder->info;
    *outReader = decoder;
    return status;
}

void close(PrImageCodec*, PrImageReader* reader) {
    BuiltinDecoder* decoder = static_cast<BuiltinDecoder*>(reader);
    gifAnimationClose(decoder->gif);
    pngAnimationClose(decoder->png);
    prMemoryFree(decoder->profile);
    prMemoryFree(decoder->scratch);
    prMemoryFree(decoder->bytes);
    prMemoryFree(decoder);
}

PrStatus frameInfo(PrImageCodec*, PrImageReader* reader, uint32_t frame, PrImageFrameInfo* info) {
    BuiltinDecoder* decoder = static_cast<BuiltinDecoder*>(reader);
    if (frame >= decoder->info.frameCount && !(frame == kPrImageFrameCover && decoder->info.hasCover)) {
        return PrStatus_InvalidData;
    }
    if (decoder->kind == BuiltinKind_Gif && decoder->gif == nullptr) {
        return PrStatus_InvalidData;
    }
    if (decoder->png != nullptr)
        info->durationMs = pngAnimationDelay(*decoder->png, frame);
    else if (decoder->gif == nullptr)
        info->durationMs = 0;
    else
        info->durationMs = gifAnimationDelay(*decoder->gif, frame);
    return PrStatus_Ok;
}

PrStatus decode(PrImageCodec*, PrImageReader* reader, uint32_t frame, PrPixels output) {
    BuiltinDecoder* decoder = static_cast<BuiltinDecoder*>(reader);
    PR_ASSERT(frame < decoder->info.frameCount || (frame == kPrImageFrameCover && decoder->info.hasCover));
    PR_ASSERT(output.extent.x == decoder->info.extent.x && output.extent.y == decoder->info.extent.y);
    PR_ASSERT(output.stride >= decoder->rowBytes);
    bool ok = false;
    if (decoder->kind == BuiltinKind_Png) {
        if (decoder->png == nullptr)
            ok = pngDecode(decoder->compressCodec, {decoder->bytes, decoder->size}, decoder->scratch,
                decoder->pixelsSize);
        else
            ok = pngAnimationFrame(*decoder->png, frame, decoder->scratch, decoder->pixelsSize);
    } else if (decoder->kind == BuiltinKind_Jpeg) {
        ok = jpegDecode({decoder->bytes, decoder->size}, decoder->scratch, decoder->pixelsSize);
    } else if (decoder->kind == BuiltinKind_Tiff) {
        ok = tiffDecode(decoder->tiff, decoder->scratch, decoder->pixelsSize);
    } else if (decoder->kind == BuiltinKind_Ico) {
        if (decoder->icon.png)
            ok = pngDecode(decoder->compressCodec, {decoder->icon.bytes, decoder->icon.size}, decoder->scratch,
                decoder->pixelsSize);
        else
            ok = icoDecodeBitmap(decoder->icon, decoder->scratch, decoder->pixelsSize);
    } else if (decoder->gif != nullptr) {
        ok = gifAnimationFrame(*decoder->gif, frame, decoder->scratch, decoder->pixelsSize);
    }
    if (!ok) {
        return PrStatus_InvalidData;
    }
    for (uint32_t row = 0; row < output.extent.y; ++row) {
        prMemCopy(output.data + size_t(row) * output.stride,
            decoder->scratch + size_t(row) * decoder->rowBytes, decoder->rowBytes);
    }
    return decoder->openStatus;
}

PrStatus encode(PrImageCodec* imageCodec, PrStringView type, PrPixelView pixels, uint8_t* PR_NULLABLE out,
        size_t capacity, size_t* size) {
    if (type != "image/png")
        return PrStatus_NotSupported;
    *size = pngEncode(pixels, static_cast<BuiltinBackend*>(imageCodec)->compressCodec, out, capacity);
    return PrStatus_Ok;
}

void destroy(PrImageCodec* imageCodec) {
    prMemoryFree(imageCodec);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrImageCodec* prImageBuiltinCreate(PrCompressCodec* compressCodec) {
    BuiltinBackend* backend = allocZeroed<BuiltinBackend>();
    backend->destroy = destroy;
    backend->headerBytes = 8;
    backend->supportsType = supportsType;
    backend->recognize = recognize;
    backend->decoderOpen = open;
    backend->decoderClose = close;
    backend->decoderFrameInfo = frameInfo;
    backend->decoderRead = decode;
    backend->encode = encode;
    backend->compressCodec = compressCodec;
    return backend;
}
