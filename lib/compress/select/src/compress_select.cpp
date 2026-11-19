// The selector for lib/compress's codecs.

#include "pr/compress_select.h"

#include "pr/assert.h"
#include "pr/count_of.h"
#if defined(PR_COMPRESS_HAS_BUILTIN)
#include "pr/compress_brotli.h"
#include "pr/compress_deflate.h"
#include "pr/compress_lz4.h"
#include "pr/compress_lzfse.h"
#include "pr/compress_zstd.h"
#endif
#if defined(PR_COMPRESS_HAS_APPLE)
#include "pr/compress_apple.h"
#endif

namespace pr_impl {

namespace {

constexpr PrStringView kNames[PrCompressCodecBackend_Count] = {"default", "builtin", "apple"};

constexpr uint64_t builtMask() {
    uint64_t mask = 0;
#if defined(PR_COMPRESS_HAS_BUILTIN)
    mask |= 1u << PrCompressCodecBackend_Builtin;
#endif
#if defined(PR_COMPRESS_HAS_APPLE)
    mask |= 1u << PrCompressCodecBackend_Apple;
#endif
    return mask;
}

constexpr uint64_t kBuilt = builtMask();

// The descriptors of the bare formats, each with its own variable.
constexpr PrBackendInterface kDeflate{.name = "compress_deflate", .names = kNames, .count = 3, .built = kBuilt};
constexpr PrBackendInterface kBrotli{.name = "compress_brotli", .names = kNames, .count = 3, .built = kBuilt};
constexpr PrBackendInterface kLzfse{.name = "compress_lzfse", .names = kNames, .count = 3, .built = kBuilt};

// LZ4 and Zstandard have only Prax's codecs.
constexpr uint64_t kBuiltinOnly = kBuilt & (1u << PrCompressCodecBackend_Builtin);
constexpr PrBackendInterface kLz4{.name = "compress_lz4", .names = kNames, .count = 3, .built = kBuiltinOnly};
constexpr PrBackendInterface kZstd{.name = "compress_zstd", .names = kNames, .count = 3, .built = kBuiltinOnly};

// The bare formats, in the order of a set of every format.
constexpr PrCompressFormat kBare[] = {
    PrCompressFormat_Deflate, PrCompressFormat_Brotli, PrCompressFormat_Lzfse, PrCompressFormat_Lz4,
    PrCompressFormat_Zstd,
};

#if defined(PR_COMPRESS_HAS_BUILTIN)
PrCompressCodec* PR_NULLABLE createBuiltin(PrCompressFormat format) {
    switch (format) {
        case PrCompressFormat_Deflate: return prCompressDeflateCreate();
        case PrCompressFormat_Brotli: return prCompressBrotliCreate();
        case PrCompressFormat_Lzfse: return prCompressLzfseCreate();
        case PrCompressFormat_Lz4: return prCompressLz4Create();
        case PrCompressFormat_Zstd: return prCompressZstdCreate();
    }
    PR_ASSERT(false && "a bare format");
    return nullptr;
}
#endif

// Returns the backend that Default stands for without a choice: Prax's own, or else the first that the build includes.
PrCompressCodecBackend defaultBackend() {
    for (uint32_t backend = PrCompressCodecBackend_Builtin; backend < PrCompressCodecBackend_Count; ++backend) {
        if ((kBuilt >> backend & 1) != 0)
            return PrCompressCodecBackend(backend);
    }
    return PrCompressCodecBackend_Count;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

const PrBackendInterface* prCompressCodecSelectInterface(PrCompressFormat format) {
    switch (format) {
        case PrCompressFormat_Deflate: return &kDeflate;
        case PrCompressFormat_Brotli: return &kBrotli;
        case PrCompressFormat_Lzfse: return &kLzfse;
        case PrCompressFormat_Lz4: return &kLz4;
        case PrCompressFormat_Zstd: return &kZstd;
    }
    PR_ASSERT(false && "a bare format");
    return &kDeflate;
}

PrCompressCodec* PR_NULLABLE prCompressCodecCreate(PrCompressCodecBackend backend, PrCompressFormat format) {
    if (backend == PrCompressCodecBackend_Default) {
        uint32_t chosen = 0;
        if (prBackendFromEnvironment(prCompressCodecSelectInterface(format), &chosen, nullptr) != PrStatus_Ok)
            return nullptr;
        backend = PrCompressCodecBackend(chosen);
        if (backend == PrCompressCodecBackend_Default)
            backend = defaultBackend();
    }
    if (!prBackendIsBuilt(prCompressCodecSelectInterface(format), backend))
        return nullptr;
    switch (backend) {
#if defined(PR_COMPRESS_HAS_BUILTIN)
        case PrCompressCodecBackend_Builtin: return createBuiltin(format);
#endif
#if defined(PR_COMPRESS_HAS_APPLE)
        case PrCompressCodecBackend_Apple: return prCompressAppleCreate(format);
#endif
        default: return nullptr;
    }
}

PrCompressCodecSet* PR_NULLABLE prCompressCodecSetCreateDefault(void) {
    // A format with no backend in the build is left out.
    PrCompressCodec* codecs[countOf(kBare)] = {};
    uint32_t count = 0;
    for (PrCompressFormat format : kBare) {
        if (!prBackendIsBuilt(prCompressCodecSelectInterface(format), PrCompressCodecBackend_Default))
            continue;
        PrCompressCodec* codec = prCompressCodecCreate(PrCompressCodecBackend_Default, format);
        if (codec == nullptr) {
            for (uint32_t made = 0; made < count; ++made)
                prCompressCodecDestroy(codecs[made]);
            return nullptr;
        }
        codecs[count++] = codec;
    }
    return prCompressCodecSetCreate(codecs, count);
}
