// The process's environment through the platform.

#include "pr/environment.h"

#include "core/src/config.h"

#if !defined(PR_PLATFORM_ENVIRONMENT_VARIABLE)
#error "a build without the default platform binds PR_PLATFORM_ENVIRONMENT_VARIABLE"
#endif

PrRawString prEnvironmentVariable(PrStringView name) {
    if (name.isEmpty())
        return prRawStringEmpty();
    return PR_PLATFORM_ENVIRONMENT_VARIABLE(name);
}
