// The POSIX backend of the environment.

#include "pr/core_posix_environment.h"

#include "pr/memory.h"
#include "pr/posix_string.h"
#include "pr/utf8.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

PrRawString prCorePosixEnvironmentVariable(PrStringView name) {
    char* terminated = prPosixStringCopy(name);
    if (terminated == nullptr)
        return prRawStringEmpty();
    const char* value = getenv(terminated);
    prMemoryFree(terminated);
    if (value == nullptr)
        return prRawStringEmpty();
    PrStringView text{value, strlen(value)};
    if (prUtf8Validate(text) != text.size)
        return prRawStringEmpty();
    return prRawStringCopy(text);
}

bool prCorePosixEnvironmentSetVariable(PrStringView name, PrStringView value) {
    char* terminatedName = prPosixStringCopy(name);
    char* terminatedValue = value.data != nullptr ? prPosixStringCopy(value) : nullptr;
    bool done = false;
    if (terminatedName != nullptr && value.data == nullptr)
        done = unsetenv(terminatedName) == 0;
    else if (terminatedName != nullptr && terminatedValue != nullptr)
        done = setenv(terminatedName, terminatedValue, 1) == 0;
    prMemoryFree(terminatedName);
    prMemoryFree(terminatedValue);
    return done;
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
