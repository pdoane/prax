// The web media codec: a resource played in a page's video element (media_web.js), whose frames are the page's images.

#include "pr/media_web.h"

#include "pr/alloc.h"
#include "pr/float.h"
#include "pr/media_backend.h"
#include "pr/memory.h"

#if defined(PR_MEDIA_HAS_BUILTIN)
#include "pr/media_builtin.h"
#endif

// The page's side of the codec (media_web.js). A player plays the bytes of a resource of the MIME type `type` in a
// video element. A frame is the PrGpuCanvas of an entry of the page's images (prGpuCanvasAddImage), which the player
// keeps until its next frame.
extern "C" bool prMediaWebAvailable(void);
extern "C" uint32_t prMediaWebCanPlay(const char* PR_NULLABLE type, size_t typeSize);
extern "C" uint32_t prMediaWebOpen(const uint8_t* bytes, size_t size, const char* type, size_t typeSize);
extern "C" void prMediaWebClose(uint32_t player);
extern "C" uint32_t prMediaWebFrame(uint32_t player, double seconds, double* time, uint32_t* serial);

namespace pr_impl {

namespace {

// The codec, and the built-in codec that reads its resources' metadata.
struct WebCodec : PrMediaCodec {
    PrMediaCodec* PR_NULLABLE metadata;
};

struct WebDecoder : PrMediaDecoder {
    uint32_t player;
};

void destroy(PrMediaCodec* codec) {
    WebCodec* c = static_cast<WebCodec*>(codec);
    prMediaCodecDestroy(c->metadata);
    prMemoryFree(c);
}

// The page's answer (html/#dom-navigator-canplaytype): "probably", "maybe" or nothing.
PrMediaSupport supportsType(PrMediaCodec*, PrStringView type) {
    return PrMediaSupport(prMediaWebCanPlay(type.data, type.size));
}

// Returns the MIME type of a resource by its signature, MP4's ftyp box or WebM's EBML header, or the null view if it
// has neither.
PrStringView typeOf(PrByteView bytes) {
    if (bytes.size >= 8 && bytes.data[4] == 'f' && bytes.data[5] == 't' && bytes.data[6] == 'y' && bytes.data[7] == 'p')
        return "video/mp4";
    if (bytes.size >= 4 && bytes.data[0] == 0x1a && bytes.data[1] == 0x45 && bytes.data[2] == 0xdf && bytes.data[3] == 0xa3)
        return "video/webm";
    return PrStringView{};
}

void decoderDestroy(PrMediaDecoder* decoder) {
    WebDecoder* d = static_cast<WebDecoder*>(decoder);
    prMediaWebClose(d->player);
    prMemoryFree(d);
}

// The frame that the video shows, once it has one: an image of the page, with no pixels.
bool decoderFrame(PrMediaDecoder* decoder, double seconds, PrMediaFrame* out) {
    WebDecoder* d = static_cast<WebDecoder*>(decoder);
    double time = 0;
    uint32_t serial = 0;
    uint32_t image = prMediaWebFrame(d->player, seconds, &time, &serial);
    if (image == 0)
        return false;
    *out = PrMediaFrame{
        .image = PrGpuImport{
            .kind = PrGpuImportKind_WebImage,
            .handle = reinterpret_cast<const void*>(uintptr_t(image)),
        },
        .time = time,
        .serial = serial,
        .current = true,
    };
    return true;
}

PrStatus decoderCreate(PrMediaCodec* codec, PrByteView bytes, PrMediaDecoder* PR_NULLABLE* PR_NONNULL out, PrMediaInfo* info) {
    WebCodec* c = static_cast<WebCodec*>(codec);
    *out = nullptr;
    PrStringView type = typeOf(bytes);
    if (type.isNull())
        return PrStatus_NotSupported;
    // The built-in codec reads the resource's metadata. The page loads the video's own metadata later.
    *info = PrMediaInfo{.duration = kPrF64Nan, .hasVideo = true};
    PrMediaDecoder* metadata = nullptr;
    if (c->metadata != nullptr && prMediaDecoderCreate(c->metadata, bytes, &metadata, info) == PrStatus_Ok)
        prMediaDecoderDestroy(metadata);
    uint32_t player = prMediaWebOpen(bytes.data, bytes.size, type.data, type.size);
    if (player == 0)
        return PrStatus_NotSupported;
    WebDecoder* d = allocZeroed<WebDecoder>();
    d->destroyFn = decoderDestroy;
    d->frameFn = decoderFrame;
    d->player = player;
    *out = d;
    return PrStatus_Ok;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrMediaCodec* PR_NULLABLE prMediaWebCreate(void) {
    if (!prMediaWebAvailable())
        return nullptr;
    WebCodec* codec = allocZeroed<WebCodec>();
    codec->destroyFn = destroy;
    codec->supportsTypeFn = supportsType;
    codec->decoderCreateFn = decoderCreate;
#if defined(PR_MEDIA_HAS_BUILTIN)
    codec->metadata = prMediaBuiltinCreate();
#endif
    return codec;
}
