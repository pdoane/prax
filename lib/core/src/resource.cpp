// Resources through the platform.

#include "pr/resource.h"

#include "core/src/config.h"

#if !defined(PR_PLATFORM_RESOURCE_OPEN)
#error "a target without a default for the group binds PR_PLATFORM_RESOURCE_OPEN"
#endif

bool prResourceNameIsValid(PrStringView name) {
    if (name.isEmpty())
        return false;
    size_t start = 0;
    for (size_t i = 0; i <= name.size; ++i) {
        if (i < name.size && name.data[i] == '\\')
            return false;
        if (i < name.size && name.data[i] != '/')
            continue;
        PrStringView part{name.data + start, i - start};
        if (part.isEmpty() || part == "." || part == "..")
            return false;
        start = i + 1;
    }
    return true;
}

PrBlob* PR_NULLABLE prResourceOpen(PrStringView name) {
    return prResourceNameIsValid(name) ? PR_PLATFORM_RESOURCE_OPEN(name) : nullptr;
}
