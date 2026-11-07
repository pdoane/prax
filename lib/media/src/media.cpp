// Media: reading audio and video resources, and decoding their audio and the frames of their video.

#include "pr/media.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/media_backend.h"

#include "media/src/media_source_decoder.h"

#if defined(PR_MEDIA_HAS_APPLE)
#include "pr/media_apple.h"
#endif
#if defined(PR_MEDIA_HAS_BUILTIN)
#include "pr/media_builtin.h"
#endif
#if defined(PR_MEDIA_HAS_WEB)
#include "pr/media_web.h"
#endif
#if defined(PR_MEDIA_HAS_WINDOWS)
#include "pr/media_windows.h"
#endif

namespace pr_impl {

namespace {

constexpr PrStringView kBackendNames[PrMediaCodecBackend_Count] = {"default", "apple", "windows", "web", "builtin"};

// The backends in the build, the platform's own first, and then Default. Default creates the first of these that it
// can. Listing Default last means that a build without backends creates none.
constexpr PrMediaCodecBackend kBuilt[] = {
#if defined(PR_MEDIA_HAS_APPLE)
    PrMediaCodecBackend_Apple,
#endif
#if defined(PR_MEDIA_HAS_WINDOWS)
    PrMediaCodecBackend_Windows,
#endif
#if defined(PR_MEDIA_HAS_WEB)
    PrMediaCodecBackend_Web,
#endif
#if defined(PR_MEDIA_HAS_BUILTIN)
    PrMediaCodecBackend_Builtin,
#endif
    PrMediaCodecBackend_Default,
};

// Creates a codec by `backend`, which is not Default.
PrMediaCodec* PR_NULLABLE create(PrMediaCodecBackend backend) {
    switch (backend) {
#if defined(PR_MEDIA_HAS_APPLE)
        case PrMediaCodecBackend_Apple: return prMediaAppleCreate();
#endif
#if defined(PR_MEDIA_HAS_WINDOWS)
        case PrMediaCodecBackend_Windows: return prMediaWindowsCreate();
#endif
#if defined(PR_MEDIA_HAS_WEB)
        case PrMediaCodecBackend_Web: return prMediaWebCreate();
#endif
#if defined(PR_MEDIA_HAS_BUILTIN)
        case PrMediaCodecBackend_Builtin: return prMediaBuiltinCreate();
#endif
        default: return nullptr;
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrMediaCodecBackend prMediaCodecBackendFromName(PrStringView name) {
    for (PrMediaCodecBackend backend = 0; backend < PrMediaCodecBackend_Count; ++backend) {
        if (prAsciiEqualsFold(kBackendNames[backend], name))
            return backend;
    }
    return PrMediaCodecBackend_Count;
}

PrStringView prMediaCodecBackendName(PrMediaCodecBackend backend) {
    return backend < PrMediaCodecBackend_Count ? kBackendNames[backend] : PrStringView{};
}

bool prMediaCodecBackendIsBuilt(PrMediaCodecBackend backend) {
    for (PrMediaCodecBackend built : kBuilt) {
        if (built != PrMediaCodecBackend_Default && (built == backend || backend == PrMediaCodecBackend_Default))
            return true;
    }
    return false;
}

PrMediaCodec* PR_NULLABLE prMediaCodecCreate(PrMediaCodecBackend backend) {
    if (backend != PrMediaCodecBackend_Default)
        return create(backend);
    // Default takes the first backend whose constructor succeeds. The web's constructor succeeds only in a page.
    for (PrMediaCodecBackend built : kBuilt) {
        if (built == PrMediaCodecBackend_Default)
            break;
        if (PrMediaCodec* codec = create(built))
            return codec;
    }
    return nullptr;
}

void prMediaCodecDestroy(PrMediaCodec* PR_NULLABLE codec) {
    if (codec != nullptr && codec->destroyFn != nullptr)
        codec->destroyFn(codec);
}

PrMediaSupport prMediaCodecSupportsType(PrMediaCodec* codec, PrStringView type) {
    return codec->supportsTypeFn(codec, type);
}

PrMediaSupport prMediaCodecSupportsSourceType(PrMediaCodec* codec, PrStringView type) {
    if (codec->supportsSourceTypeFn != nullptr)
        return codec->supportsSourceTypeFn(codec, type);
    return codec->supportsTypeFn(codec, type);
}

PrStatus prMediaDecoderCreate(PrMediaCodec* codec, PrByteView bytes, PrMediaDecoder* PR_NULLABLE* PR_NONNULL out,
        PrMediaInfo* info) {
    *out = nullptr;
    *info = PrMediaInfo{};
    PrStatus status = codec->decoderCreateFn(codec, bytes, out, info);
    PR_ASSERT((status == PrStatus_Ok) == (*out != nullptr));
    return status;
}

PrStatus prMediaDecoderCreateFromSource(PrMediaCodec* codec, PrMediaSourceBuffer* PR_NONNULL const* PR_NONNULL buffers,
        uint32_t count, PrMediaDecoder* PR_NULLABLE* PR_NONNULL out, PrMediaInfo* info) {
    *info = PrMediaInfo{};
    return mediaSourceDecoderCreate(codec, buffers, count, out, info);
}

void prMediaDecoderSetSourceEnded(PrMediaDecoder* decoder, bool ended) {
    mediaSourceDecoderSetEnded(decoder, ended);
}

void prMediaDecoderDestroy(PrMediaDecoder* PR_NULLABLE decoder) {
    if (decoder != nullptr)
        decoder->destroyFn(decoder);
}

bool prMediaDecoderHasFrames(const PrMediaDecoder* decoder) {
    return decoder->frameFn != nullptr;
}

bool prMediaDecoderFrame(PrMediaDecoder* decoder, double seconds, PrMediaFrame* out) {
    *out = PrMediaFrame{};
    if (decoder->frameFn == nullptr) {
        out->current = true;
        return false;
    }
    if (!decoder->frameFn(decoder, seconds, out))
        return false;
    PR_ASSERT(out->image.handle != nullptr);
    return true;
}

bool prMediaDecoderAudioStart(PrMediaDecoder* decoder, uint32_t sampleRate, uint32_t channels, double seconds) {
    return decoder->audioStartFn != nullptr && decoder->audioStartFn(decoder, sampleRate, channels, seconds);
}

void prMediaDecoderAudioSeek(PrMediaDecoder* decoder, double seconds) {
    decoder->audioSeekFn(decoder, seconds);
}

PrMediaAudioChunk prMediaDecoderAudioRead(PrMediaDecoder* decoder, float* samples, uint32_t frames) {
    return decoder->audioReadFn(decoder, samples, frames);
}

void prMediaDecoderAudioWait(PrMediaDecoder* decoder, uint32_t frames) {
    if (decoder->audioWaitFn != nullptr)
        decoder->audioWaitFn(decoder, frames);
}
