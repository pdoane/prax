// The selector for lib/image's codecs.

#include "pr/image_select.h"

#include "pr/assert.h"
#include "pr/count_of.h"
#if defined(PR_IMAGE_HAS_BUILTIN)
#include "pr/image_emjc.h"
#include "pr/image_exr.h"
#include "pr/image_gif.h"
#include "pr/image_hdr.h"
#include "pr/image_ico.h"
#include "pr/image_jpeg.h"
#include "pr/image_png.h"
#include "pr/image_tiff.h"
#include "pr/image_webp.h"
#endif
#if defined(PR_IMAGE_HAS_IMAGEIO)
#include "pr/image_imageio.h"
#endif

namespace pr_impl {

namespace {

constexpr PrStringView kNames[PrImageCodecBackend_Count] = {"default", "builtin", "imageio"};

#if defined(PR_IMAGE_HAS_BUILTIN)
constexpr uint64_t kBuiltin = 1u << PrImageCodecBackend_Builtin;
#else
constexpr uint64_t kBuiltin = 0;
#endif
#if defined(PR_IMAGE_HAS_IMAGEIO)
constexpr uint64_t kWithImageio = kBuiltin | 1u << PrImageCodecBackend_ImageIO;
#else
constexpr uint64_t kWithImageio = kBuiltin;
#endif

// The formats that Prax has.
enum {
    ImageKind_Png,
    ImageKind_Jpeg,
    ImageKind_Gif,
    ImageKind_Webp,
    ImageKind_Ico,
    ImageKind_Tiff,
    ImageKind_Hdr,
    ImageKind_Exr,
    ImageKind_Emjc,
};
typedef uint8_t ImageKind;

// A format that Prax has: its media type, its kind, and the descriptor of its codecs.
struct ImageSelectFormat {
    PrStringView type;
    ImageKind kind;
    PrBackendInterface interface;
};

// The formats, in the order of a set of every format. ImageIO does not decode EMJC.
constexpr ImageSelectFormat kFormats[] = {
    {"image/png", ImageKind_Png, {.name = "image_png", .names = kNames, .count = 3, .built = kWithImageio}},
    {"image/jpeg", ImageKind_Jpeg, {.name = "image_jpeg", .names = kNames, .count = 3, .built = kWithImageio}},
    {"image/gif", ImageKind_Gif, {.name = "image_gif", .names = kNames, .count = 3, .built = kWithImageio}},
    {"image/webp", ImageKind_Webp, {.name = "image_webp", .names = kNames, .count = 3, .built = kWithImageio}},
    {"image/x-icon", ImageKind_Ico, {.name = "image_ico", .names = kNames, .count = 3, .built = kWithImageio}},
    {"image/tiff", ImageKind_Tiff, {.name = "image_tiff", .names = kNames, .count = 3, .built = kWithImageio}},
    {"image/vnd.radiance", ImageKind_Hdr, {.name = "image_hdr", .names = kNames, .count = 3, .built = kWithImageio}},
    {"image/x-exr", ImageKind_Exr, {.name = "image_exr", .names = kNames, .count = 3, .built = kWithImageio}},
    {"image/x-emjc", ImageKind_Emjc, {.name = "image_emjc", .names = kNames, .count = 3, .built = kBuiltin}},
};

const ImageSelectFormat* PR_NULLABLE formatOf(PrStringView type) {
    for (const ImageSelectFormat& format : kFormats) {
        if (format.type == type)
            return &format;
    }
    return nullptr;
}

#if defined(PR_IMAGE_HAS_BUILTIN)
PrImageCodec* PR_NULLABLE createBuiltin(ImageKind kind, const PrCompressCodecSet* compress) {
    switch (kind) {
        case ImageKind_Png: return prImagePngCreate(compress);
        case ImageKind_Jpeg: return prImageJpegCreate();
        case ImageKind_Gif: return prImageGifCreate();
        case ImageKind_Webp: return prImageWebpCreate();
        case ImageKind_Ico: return prImageIcoCreate();
        case ImageKind_Tiff: return prImageTiffCreate();
        case ImageKind_Hdr: return prImageHdrCreate();
        case ImageKind_Exr: return prImageExrCreate(compress);
        case ImageKind_Emjc: return prImageEmjcCreate(compress);
    }
    PR_ASSERT(false && "a format of kFormats");
    return nullptr;
}
#endif

// Returns the backend that Default stands for without a choice for `format`: Prax's own, or else the first that the
// build includes.
PrImageCodecBackend defaultBackend(const ImageSelectFormat& format) {
    for (uint32_t backend = PrImageCodecBackend_Builtin; backend < PrImageCodecBackend_Count; ++backend) {
        if (prBackendIsBuilt(&format.interface, backend))
            return PrImageCodecBackend(backend);
    }
    return PrImageCodecBackend_Count;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

const PrBackendInterface* PR_NULLABLE prImageCodecSelectInterface(PrStringView type) {
    const ImageSelectFormat* format = formatOf(type);
    return format != nullptr ? &format->interface : nullptr;
}

PrImageCodec* PR_NULLABLE prImageCodecCreate(PrImageCodecBackend backend, PrStringView type,
        const PrCompressCodecSet* compress) {
    const ImageSelectFormat* format = formatOf(type);
    if (format == nullptr)
        return nullptr;
    if (backend == PrImageCodecBackend_Default) {
        uint32_t chosen = 0;
        if (prBackendFromEnvironment(&format->interface, &chosen, nullptr) != PrStatus_Ok)
            return nullptr;
        backend = PrImageCodecBackend(chosen);
        if (backend == PrImageCodecBackend_Default)
            backend = defaultBackend(*format);
    }
    if (!prBackendIsBuilt(&format->interface, backend))
        return nullptr;
    switch (backend) {
#if defined(PR_IMAGE_HAS_BUILTIN)
        case PrImageCodecBackend_Builtin: return createBuiltin(format->kind, compress);
#endif
#if defined(PR_IMAGE_HAS_IMAGEIO)
        case PrImageCodecBackend_ImageIO: return prImageImageIOCreate(type);
#endif
        default: return nullptr;
    }
}

PrImageCodecSet* PR_NULLABLE prImageCodecSetCreateDefault(const PrCompressCodecSet* compress) {
    // A format with no backend in the build is left out.
    PrImageCodec* codecs[countOf(kFormats)] = {};
    uint32_t count = 0;
    for (const ImageSelectFormat& format : kFormats) {
        if (!prBackendIsBuilt(&format.interface, PrImageCodecBackend_Default))
            continue;
        PrImageCodec* codec = prImageCodecCreate(PrImageCodecBackend_Default, format.type, compress);
        if (codec == nullptr) {
            for (uint32_t made = 0; made < count; ++made)
                prImageCodecDestroy(codecs[made]);
            return nullptr;
        }
        codecs[count++] = codec;
    }
    return prImageCodecSetCreate(codecs, count);
}
