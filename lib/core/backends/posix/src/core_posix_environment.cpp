// The POSIX backend of the environment.

#include "pr/core_posix_environment.h"

#include "pr/utf8.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

PrRawString prCorePosixEnvironmentVariable(PrStringView name) {
    char* terminated = static_cast<char*>(malloc(name.size + 1));
    if (terminated == nullptr)
        return prRawStringEmpty();
    memcpy(terminated, name.data, name.size);
    terminated[name.size] = 0;
    const char* value = getenv(terminated);
    free(terminated);
    if (value == nullptr)
        return prRawStringEmpty();
    PrStringView text{value, strlen(value)};
    if (prUtf8Validate(text) != text.size)
        return prRawStringEmpty();
    return prRawStringCopy(text);
}

PrRawString prCorePosixEnvironmentHostName(void) {
    // POSIX limits a host name to 255 bytes. gethostname may truncate a name that fills the buffer and leave out its
    // terminator, so the last byte is set to zero.
    char name[256];
    if (gethostname(name, sizeof(name)) != 0)
        return prRawStringEmpty();
    name[sizeof(name) - 1] = 0;
    PrStringView text{name, strlen(name)};
    if (prUtf8Validate(text) != text.size)
        return prRawStringEmpty();
    return prRawStringCopy(text);
}
