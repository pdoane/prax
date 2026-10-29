// Arrays of owned strings.

#pragma once

#include "pr/array.h"
#include "pr/string.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// Resets each string of `strings`, and empties the array but keeps its storage.
inline void stringsClear(Array<PrString>* strings) {
    for (uint32_t i = 0; i < strings->count; ++i)
        prStringReset(&strings->data[i]);
    strings->clear();
}

// Resets each string of `strings`, and frees the storage of the array.
inline void stringsReset(Array<PrString>* strings) {
    stringsClear(strings);
    strings->reset();
}

} // namespace pr

PR_ASSUME_NONNULL_END
