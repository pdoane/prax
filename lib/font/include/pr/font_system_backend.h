// The interface that system font backends implement. Callers use the functions of pr/font_set.h, which call these. Only
// backends include this header.

#pragma once

#include "pr/base.h"
#include "pr/font_set.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The functions of a font system's backend, at the start of its state. A null `destroyFn` means that there is nothing
// to free.
struct PrFontSystem {
    void (*PR_NULLABLE destroyFn)(PrFontSystem* system);

    // Adds the files of `family` to `set`. Returns false if the system has no such family.
    bool (*addFamilyFn)(PrFontSystem* system, PrFontSet* set, PrStringView family);

    // Returns the system's family for `generic`, or the null string if it has none. The caller releases it with
    // prRawStringReset.
    PrRawString (*genericFamilyFn)(PrFontSystem* system, PrFontGeneric generic);

    // Adds to `set` the files of a family that serves the cluster `text` in `locale`. The family suits `generic`, or is
    // the system's UI font if `generic` is None. Returns the family, or the null string if the system has none. The
    // caller releases it with prRawStringReset.
    PrRawString (*addFallbackFn)(PrFontSystem* system, PrFontSet* set, PrStringView text, PrLocale locale, PrFontGeneric generic);
};

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
