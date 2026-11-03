// Arrays of owned strings.

#pragma once

#include "pr/raw_array.h"
#include "pr/raw_string.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// Resets each string of `strings`, and empties the array but keeps its storage.
inline void stringsClear(RawArray<PrRawString>* strings) {
    for (uint32_t i = 0; i < strings->count; ++i)
        prRawStringReset(&strings->data[i]);
    strings->clear();
}

// Resets each string of `strings`, and frees the storage of the array.
inline void stringsReset(RawArray<PrRawString>* strings) {
    stringsClear(strings);
    strings->reset();
}

} // namespace pr

PR_ASSUME_NONNULL_END
