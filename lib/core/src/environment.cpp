// The process's environment through the platform.

#include "pr/environment.h"

#include "core/src/config.h"

#if !defined(PR_PLATFORM_ENVIRONMENT_VARIABLE) || !defined(PR_PLATFORM_ENVIRONMENT_HOST_NAME)
#error "a build without the default platform binds PR_PLATFORM_ENVIRONMENT_VARIABLE and _HOST_NAME"
#endif

PrRawString prEnvironmentVariable(PrStringView name) {
    if (name.isEmpty())
        return prRawStringEmpty();
    return PR_PLATFORM_ENVIRONMENT_VARIABLE(name);
}

PrRawString prEnvironmentHostName(void) {
    return PR_PLATFORM_ENVIRONMENT_HOST_NAME();
}
