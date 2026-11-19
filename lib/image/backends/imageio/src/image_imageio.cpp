// The ImageIO codec: one format that Apple's ImageIO decodes, as the image interface describes it. ImageIO composes the
// frames of an animation, and Core Graphics draws them into the caller's pixels.

#include "pr/image_imageio.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/image_backend.h"
#include "pr/mem.h"

#include "image/backends/shared/src/image_shared.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

namespace pr_impl {

namespace {

// A media type, and ImageIO's identifier of its format.
struct ImageioType {
    PrStringView type;
    const char* identifier;
};

constexpr ImageioType kTypes[] = {
    {"image/png", "public.png"},
    {"image/jpeg", "public.jpeg"},
    {"image/gif", "com.compuserve.gif"},
    {"image/tiff", "public.tiff"},
    {"image/x-icon", "com.microsoft.ico"},
    {"image/vnd.microsoft.icon", "com.microsoft.ico"},
    {"image/bmp", "com.microsoft.bmp"},
    {"image/heic", "public.heic"},
    {"image/heif", "public.heif"},
    {"image/webp", "org.webmproject.webp"},
    {"image/avif", "public.avif"},
    {"image/vnd.radiance", "public.radiance"},
    {"image/x-exr", "com.ilm.openexr-image"},
};

// The bytes from the start of an image from which ImageIO tells its format.
constexpr uint32_t kHeaderBytes = 64;

struct ImageioCodec : PrImageCodec {
    PrStringView type;              // one of kTypes
    CFStringRef identifier;
};

struct ImageioReader : PrImageReader {
    uint8_t* bytes;
    CFDataRef data;                 // the bytes, without a copy
    CGImageSourceRef source;
    CGColorSpaceRef space;          // the space that the pixels are drawn in
    CFDataRef PR_NULLABLE profile;  // the ICC profile of `space`, unless it is sRGB
    PrUVec2 extent;
    bool alpha;
    PrStatus openStatus;            // Ok, or Truncated for a file that ends early
};

// Returns the number in `dictionary` at `key`, or `otherwise`.
double numberIn(CFDictionaryRef PR_NULLABLE dictionary, CFStringRef key, double otherwise) {
    if (dictionary == nullptr)
        return otherwise;
    const void* value = CFDictionaryGetValue(dictionary, key);
    double number = otherwise;
    if (value != nullptr && CFGetTypeID(value) == CFNumberGetTypeID())
        CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberDoubleType, &number);
    return number;
}

// Returns the dictionary in `dictionary` at `key`, or null.
CFDictionaryRef PR_NULLABLE dictionaryIn(CFDictionaryRef PR_NULLABLE dictionary, CFStringRef key) {
    if (dictionary == nullptr)
        return nullptr;
    const void* value = CFDictionaryGetValue(dictionary, key);
    if (value == nullptr || CFGetTypeID(value) != CFDictionaryGetTypeID())
        return nullptr;
    return static_cast<CFDictionaryRef>(value);
}

// Returns the duration of a frame with the properties `frame`, in milliseconds as encoded.
uint32_t durationOf(CFDictionaryRef PR_NULLABLE frame) {
    double seconds = numberIn(dictionaryIn(frame, kCGImagePropertyGIFDictionary), kCGImagePropertyGIFUnclampedDelayTime, -1);
    if (seconds < 0)
        seconds = numberIn(dictionaryIn(frame, kCGImagePropertyPNGDictionary), kCGImagePropertyAPNGUnclampedDelayTime, -1);
    if (seconds < 0)
        seconds = numberIn(dictionaryIn(frame, kCGImagePropertyWebPDictionary), kCGImagePropertyWebPUnclampedDelayTime, -1);
    return seconds > 0 ? uint32_t(seconds * 1000 + 0.5) : 0;
}

// Returns the number of times the frames of an image with the properties `image` play, or zero for forever.
uint32_t playCountOf(CFDictionaryRef PR_NULLABLE image) {
    double count = numberIn(dictionaryIn(image, kCGImagePropertyGIFDictionary), kCGImagePropertyGIFLoopCount, -1);
    if (count < 0)
        count = numberIn(dictionaryIn(image, kCGImagePropertyPNGDictionary), kCGImagePropertyAPNGLoopCount, -1);
    if (count < 0)
        count = numberIn(dictionaryIn(image, kCGImagePropertyWebPDictionary), kCGImagePropertyWebPLoopCount, -1);
    return count >= 0 ? uint32_t(count) : 1;
}

void close(PrImageReader* reader) {
    ImageioReader* imageio = static_cast<ImageioReader*>(reader);
    if (imageio->profile != nullptr)
        CFRelease(imageio->profile);
    CGColorSpaceRelease(imageio->space);
    CFRelease(imageio->source);
    CFRelease(imageio->data);
    prMemoryFree(imageio->bytes);
    prMemoryFree(imageio);
}

PrStatus frameInfo(PrImageReader* reader, uint32_t frame, PrImageFrameInfo* info) {
    ImageioReader* imageio = static_cast<ImageioReader*>(reader);
    CFDictionaryRef properties = CGImageSourceCopyPropertiesAtIndex(imageio->source, frame, nullptr);
    info->durationMs = durationOf(properties);
    if (properties != nullptr)
        CFRelease(properties);
    return PrStatus_Ok;
}

PrStatus read(PrImageReader* reader, uint32_t frame, uint32_t, PrPixels output) {
    ImageioReader* imageio = static_cast<ImageioReader*>(reader);
    CGImageRef image = CGImageSourceCreateImageAtIndex(imageio->source, frame, nullptr);
    if (image == nullptr)
        return PrStatus_InvalidData;
    const size_t width = imageio->extent.x;
    const size_t height = imageio->extent.y;
    // Core Graphics draws four bytes a pixel. An image without alpha is drawn with a fourth byte that is then dropped.
    uint8_t* pixels = output.data;
    size_t stride = output.stride;
    uint32_t info = uint32_t(kCGImageAlphaPremultipliedLast) | uint32_t(kCGBitmapByteOrder32Big);
    if (!imageio->alpha) {
        stride = width * 4;
        pixels = static_cast<uint8_t*>(prMemoryAlloc(stride * height));
        info = uint32_t(kCGImageAlphaNoneSkipLast) | uint32_t(kCGBitmapByteOrder32Big);
    }
    CGContextRef context = CGBitmapContextCreate(pixels, width, height, 8, stride, imageio->space, info);
    if (context == nullptr) {
        if (pixels != output.data)
            prMemoryFree(pixels);
        CGImageRelease(image);
        return PrStatus_InvalidData;
    }
    // A frame is drawn at its own size from the top left, so that pixels are copied, not resampled.
    const CGFloat drawnWidth = CGFloat(CGImageGetWidth(image));
    const CGFloat drawnHeight = CGFloat(CGImageGetHeight(image));
    CGContextSetBlendMode(context, kCGBlendModeCopy);
    CGContextSetInterpolationQuality(context, kCGInterpolationNone);
    CGContextClearRect(context, CGRectMake(0, 0, CGFloat(width), CGFloat(height)));
    CGContextDrawImage(context, CGRectMake(0, CGFloat(height) - drawnHeight, drawnWidth, drawnHeight), image);
    CGContextRelease(context);
    CGImageRelease(image);
    if (pixels != output.data) {
        for (size_t row = 0; row < height; ++row) {
            const uint8_t* from = pixels + row * stride;
            uint8_t* to = output.data + row * output.stride;
            for (size_t x = 0; x < width; ++x)
                prMemCopy(to + x * 3, from + x * 4, 3);
        }
        prMemoryFree(pixels);
    }
    return imageio->openStatus;
}

void destroyCodec(PrImageCodec* codec) {
    ImageioCodec* imageio = static_cast<ImageioCodec*>(codec);
    CFRelease(imageio->identifier);
    prMemoryFree(imageio);
}

uint32_t headerBytes(const PrImageCodec*) {
    return kHeaderBytes;
}

bool supportsType(const PrImageCodec* codec, PrStringView type) {
    return type == static_cast<const ImageioCodec*>(codec)->type;
}

// Returns true if ImageIO reads `bytes` as the codec's format.
bool isFormat(const ImageioCodec& codec, PrByteView bytes) {
    CFDataRef data =
        CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, bytes.data, CFIndex(bytes.size), kCFAllocatorNull);
    CGImageSourceRef source = CGImageSourceCreateWithData(data, nullptr);
    bool matches = false;
    if (source != nullptr) {
        CFStringRef type = CGImageSourceGetType(source);
        matches = type != nullptr && CFEqual(type, codec.identifier);
        CFRelease(source);
    }
    CFRelease(data);
    return matches;
}

bool recognize(const PrImageCodec* codec, PrByteView prefix) {
    return prefix.size != 0 && isFormat(*static_cast<const ImageioCodec*>(codec), prefix);
}

// Chooses the space that the image's pixels are drawn in: the image's own if it is RGB, and sRGB otherwise. Writes the
// space to the reader, and the color that it means to `*color`.
void chooseSpace(ImageioReader& reader, CGImageRef PR_NULLABLE image, PrImageColor* color) {
    CGColorSpaceRef own = image != nullptr ? CGImageGetColorSpace(image) : nullptr;
    if (own != nullptr && CGColorSpaceGetModel(own) == kCGColorSpaceModelRGB) {
        CFStringRef name = CGColorSpaceCopyName(own);
        const bool srgb = name != nullptr && CFEqual(name, kCGColorSpaceSRGB);
        if (name != nullptr)
            CFRelease(name);
        CFDataRef profile = srgb ? nullptr : CGColorSpaceCopyICCData(own);
        if (srgb || profile != nullptr) {
            reader.space = CGColorSpaceRetain(own);
            reader.profile = profile;
            if (srgb) {
                *color = PrImageColor{.encoding = PrImageColorEncoding_Named, .space = PrColorSpace_Srgb};
            } else {
                *color = PrImageColor{
                    .encoding = PrImageColorEncoding_Icc,
                    .profile = PrByteView{CFDataGetBytePtr(profile), size_t(CFDataGetLength(profile))},
                };
            }
            return;
        }
    }
    reader.space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    *color = PrImageColor{.encoding = PrImageColorEncoding_Named, .space = PrColorSpace_Srgb};
}

PrStatus open(const PrImageCodec* codec, const PrImageCodecSet*, const PrImageSource* source, const PrImageLimits* limits,
        PrImageReader* PR_NULLABLE* PR_NONNULL out, PrImageInfo* info) {
    *out = nullptr;
    uint8_t* bytes = nullptr;
    size_t size = 0;
    PrStatus status = imageReadSource(source, *limits, sizeof(ImageioReader), &bytes, &size);
    if (status != PrStatus_Ok)
        return status;
    CFDataRef data = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, bytes, CFIndex(size), kCFAllocatorNull);
    CGImageSourceRef imageSource = CGImageSourceCreateWithData(data, nullptr);
    const size_t frames = imageSource != nullptr ? CGImageSourceGetCount(imageSource) : 0;
    CFDictionaryRef properties = frames != 0 ? CGImageSourceCopyPropertiesAtIndex(imageSource, 0, nullptr) : nullptr;
    const double width = numberIn(properties, kCGImagePropertyPixelWidth, 0);
    const double height = numberIn(properties, kCGImagePropertyPixelHeight, 0);
    if (!isFormat(*static_cast<const ImageioCodec*>(codec), PrByteView{bytes, size}))
        status = PrStatus_NotSupported;
    else if (frames == 0 || !(width >= 1) || !(height >= 1) || width > 0xffffffff || height > 0xffffffff)
        status = PrStatus_InvalidData;
    else if (width * height > double(limits->pixels) || frames > limits->frames)
        status = PrStatus_Limit;
    if (status != PrStatus_Ok) {
        if (properties != nullptr)
            CFRelease(properties);
        if (imageSource != nullptr)
            CFRelease(imageSource);
        CFRelease(data);
        prMemoryFree(bytes);
        return status;
    }
    ImageioReader* reader = allocZeroed<ImageioReader>();
    reader->closeFn = close;
    reader->frameInfoFn = frameInfo;
    reader->readFn = read;
    reader->bytes = bytes;
    reader->data = data;
    reader->source = imageSource;
    reader->extent = PrUVec2{uint32_t(width), uint32_t(height)};
    const void* hasAlpha = properties != nullptr ? CFDictionaryGetValue(properties, kCGImagePropertyHasAlpha) : nullptr;
    reader->alpha = hasAlpha != nullptr && CFGetTypeID(hasAlpha) == CFBooleanGetTypeID() &&
        CFBooleanGetValue(static_cast<CFBooleanRef>(hasAlpha));
    const CGImageSourceStatus sourceStatus = CGImageSourceGetStatus(imageSource);
    reader->openStatus = sourceStatus == kCGImageStatusComplete ? PrStatus_Ok : PrStatus_Truncated;

    CGImageRef first = CGImageSourceCreateImageAtIndex(imageSource, 0, nullptr);
    PrImageColor color{};
    chooseSpace(*reader, first, &color);
    if (first != nullptr)
        CGImageRelease(first);
    // EXIF's orientations 1 through 8 are PrImageOrientation's values in order.
    const double orientation = numberIn(properties, kCGImagePropertyOrientation, 1);
    const double dpiX = numberIn(properties, kCGImagePropertyDPIWidth, 0);
    const double dpiY = numberIn(properties, kCGImagePropertyDPIHeight, 0);
    CFDictionaryRef container = CGImageSourceCopyProperties(imageSource, nullptr);
    const uint64_t pixels = uint64_t(width) * uint64_t(height);
    *info = PrImageInfo{
        .extent = reader->extent,
        .format = PrImageFormat{
            .model = PrColorModel_Rgb,
            .sample = PrImageSample_U8,
            .alpha = PrImageAlpha(reader->alpha ? PrImageAlpha_Premultiplied : PrImageAlpha_None),
            .color = color,
        },
        .orientation = PrImageOrientation(orientation >= 1 && orientation <= 8 ? uint32_t(orientation) - 1 : 0),
        .pixelsPerMeter = PrVec2{float(dpiX / 0.0254), float(dpiY / 0.0254)},
        .frameCount = uint32_t(frames),
        .playCount = frames > 1 ? playCountOf(container) : 1,
        // The input, the reader, and the frame that ImageIO decodes before Core Graphics draws it.
        .workingBytes = size + sizeof(ImageioReader) + pixels * 4,
    };
    if (container != nullptr)
        CFRelease(container);
    CFRelease(properties);
    *out = reader;
    return reader->openStatus;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrImageCodec* PR_NULLABLE prImageImageIOCreate(PrStringView type) {
    const ImageioType* found = nullptr;
    for (const ImageioType& known : kTypes) {
        if (known.type == type)
            found = &known;
    }
    if (found == nullptr)
        return nullptr;
    CFStringRef identifier = CFStringCreateWithCString(kCFAllocatorDefault, found->identifier, kCFStringEncodingUTF8);
    // The types that ImageIO decodes depend on the system.
    CFArrayRef decoded = CGImageSourceCopyTypeIdentifiers();
    const bool decodes = CFArrayContainsValue(decoded, CFRangeMake(0, CFArrayGetCount(decoded)), identifier);
    CFRelease(decoded);
    if (!decodes) {
        CFRelease(identifier);
        return nullptr;
    }
    ImageioCodec* codec = allocZeroed<ImageioCodec>();
    codec->destroyFn = destroyCodec;
    codec->headerBytesFn = headerBytes;
    codec->supportsTypeFn = supportsType;
    codec->recognizeFn = recognize;
    codec->decoderOpenFn = open;
    codec->type = found->type;
    codec->identifier = identifier;
    return codec;
}
