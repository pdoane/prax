// Media: reading audio and video resources, and decoding their audio and the frames of their video.

#include "pr/media.h"

#include "pr/ascii.h"
#include "pr/assert.h"

#if defined(PR_MEDIA_HAS_APPLE)
#include "pr/media_apple.h"
#endif
#if defined(PR_MEDIA_HAS_BUILTIN)
#include "pr/media_builtin.h"
#endif
#if defined(PR_MEDIA_HAS_WINDOWS)
#include "pr/media_windows.h"
#endif

namespace pr_impl {

namespace {

constexpr PrStringView kBackendNames[PrMediaCodecBackend_Count] = {"default", "apple", "windows", "builtin"};

// The backends that the build has, the platform's own first, which Default creates the first of, and then Default, so
// that a build without backends creates none.
constexpr PrMediaCodecBackend kBuilt[] = {
#if defined(PR_MEDIA_HAS_APPLE)
    PrMediaCodecBackend_Apple,
#endif
#if defined(PR_MEDIA_HAS_WINDOWS)
    PrMediaCodecBackend_Windows,
#endif
#if defined(PR_MEDIA_HAS_BUILTIN)
    PrMediaCodecBackend_Builtin,
#endif
    PrMediaCodecBackend_Default,
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrMediaCodecBackend prMediaCodecBackendFromName(PrStringView name) {
    for (PrMediaCodecBackend backend = 0; backend < PrMediaCodecBackend_Count; ++backend) {
        if (asciiEqualsFold(kBackendNames[backend], name))
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
    switch (backend == PrMediaCodecBackend_Default ? kBuilt[0] : backend) {
#if defined(PR_MEDIA_HAS_APPLE)
        case PrMediaCodecBackend_Apple: return prMediaAppleCreate();
#endif
#if defined(PR_MEDIA_HAS_WINDOWS)
        case PrMediaCodecBackend_Windows: return prMediaWindowsCreate();
#endif
#if defined(PR_MEDIA_HAS_BUILTIN)
        case PrMediaCodecBackend_Builtin: return prMediaBuiltinCreate();
#endif
        default: return nullptr;
    }
}

void prMediaCodecDestroy(PrMediaCodec* PR_NULLABLE codec) {
    if (codec != nullptr && codec->destroy != nullptr)
        codec->destroy(codec);
}

PrMediaSupport prMediaCodecSupportsType(PrMediaCodec* codec, PrStringView type) {
    return codec->supportsType(codec, type);
}

PrStatus prMediaDecoderCreate(PrMediaCodec* codec, PrByteView bytes, PrMediaDecoder* PR_NULLABLE* PR_NONNULL out,
        PrMediaInfo* info) {
    *out = nullptr;
    *info = PrMediaInfo{};
    PrStatus status = codec->decoderCreate(codec, bytes, out, info);
    PR_ASSERT((status == PrStatus_Ok) == (*out != nullptr));
    if (status == PrStatus_Ok)
        (*out)->codec = codec;
    return status;
}

void prMediaDecoderDestroy(PrMediaDecoder* PR_NULLABLE decoder) {
    if (decoder != nullptr)
        decoder->codec->decoderDestroy(decoder->codec, decoder);
}

bool prMediaDecoderHasFrames(const PrMediaDecoder* decoder) {
    return decoder->codec->decoderFrame != nullptr;
}

bool prMediaDecoderFrame(PrMediaDecoder* decoder, double seconds, PrMediaFrame* out) {
    *out = PrMediaFrame{};
    PrMediaCodec* codec = decoder->codec;
    if (codec->decoderFrame == nullptr) {
        out->current = true;
        return false;
    }
    return codec->decoderFrame(codec, decoder, seconds, out);
}

bool prMediaDecoderAudioStart(PrMediaDecoder* decoder, uint32_t sampleRate, uint32_t channels, double seconds) {
    PrMediaCodec* codec = decoder->codec;
    return codec->decoderAudioStart != nullptr && codec->decoderAudioStart(codec, decoder, sampleRate, channels, seconds);
}

void prMediaDecoderAudioSeek(PrMediaDecoder* decoder, double seconds) {
    decoder->codec->decoderAudioSeek(decoder->codec, decoder, seconds);
}

PrMediaAudioChunk prMediaDecoderAudioRead(PrMediaDecoder* decoder, float* samples, uint32_t frames) {
    return decoder->codec->decoderAudioRead(decoder->codec, decoder, samples, frames);
}
