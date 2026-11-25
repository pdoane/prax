// Views of text as UTF-8, for a function that takes text where a USVString is meant.

#pragma once

#include "pr/assert.h"
#include "pr/base.h"
#include "pr/string_view.h"
#include "pr/text_buffer.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// Up to `Count` texts read as Unicode scalar values (prTextBufferUtf8FromWtf8): each view is its text itself unless the
// text holds a lone surrogate or an ill-formed sequence, and a buffer of its own holds the converted text otherwise.
// The views are valid until `reset`, which frees the buffers.
template <uint32_t Count>
struct Utf8Views {
    PrTextBuffer buffers[Count] = {};
    uint32_t used = 0;

    PrStringView of(PrStringView text) {
        PR_ASSERT(used < Count);
        return prTextBufferUtf8FromWtf8(&buffers[used++], text);
    }

    void reset() {
        for (PrTextBuffer& buffer : buffers)
            prTextBufferReset(&buffer);
        used = 0;
    }
};

} // namespace pr

PR_ASSUME_NONNULL_END
