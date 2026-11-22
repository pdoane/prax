// Basis Universal UASTC. It is one of the two formats that a KTX 2.0 texture can hold in place of a format that a GPU
// samples. A texture is blocks of 16 bytes. Each block is a subset of ASTC 4x4 with a different bit layout.
//
// UASTC LDR 4x4 Texture Specification - https://github.com/BinomialLLC/basis_universal/wiki

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Decodes the 16-byte UASTC block at `block` to its 16 texels at `texels`. The texels are in rows, starting with the
// first row. Each texel is red, green, blue and alpha. Returns false if the block has the mode that the format reserves
// or a pattern that the format does not define.
bool prBasisUastcDecodeBlock(const uint8_t* block, uint8_t* texels);

// Transcodes the UASTC block at `block` to a 16-byte BC7 block at `out`. The BC7 block decodes to nearly the same
// texels. It uses the BC7 mode that has the subsets and planes of the UASTC block. The endpoints are rounded to the
// number of bits that the mode has. The weights are kept if BC7 has weights of the same number of bits. Returns false
// for the same blocks as prBasisUastcDecodeBlock.
bool prBasisUastcTranscodeBc7(const uint8_t* block, uint8_t* out);

// Transcodes the UASTC block at `block` to a 16-byte ASTC 4x4 block at `out`. The ASTC block decodes to the same
// texels. It holds the endpoints and weights of the UASTC block in the ASTC bit layout. If ASTC would read the
// endpoints of a subset with blue contraction, the function swaps those endpoints and inverts the weights of the
// subset. Returns false for the same blocks as prBasisUastcDecodeBlock.
bool prBasisUastcTranscodeAstc(const uint8_t* block, uint8_t* out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
