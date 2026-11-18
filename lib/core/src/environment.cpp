// The process's environment through the platform.

#include "pr/environment.h"

#include "pr/string_ops.h"

#include "core/src/config.h"

#if !defined(PR_PLATFORM_ENVIRONMENT_VARIABLE) || !defined(PR_PLATFORM_ENVIRONMENT_SET_VARIABLE) || \
    !defined(PR_PLATFORM_ENVIRONMENT_HOST_NAME)
#error "a target without a default for the group binds PR_PLATFORM_ENVIRONMENT_VARIABLE, _SET_VARIABLE and _HOST_NAME"
#endif

namespace pr_impl {

namespace {

// Returns true if `name` can name a variable: it is not empty and has no `=` or NUL.
bool isName(PrStringView name) {
    return !name.isEmpty() && prStringFindByte(name, '=') == name.size && prStringFindByte(name, '\0') == name.size;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrRawString prEnvironmentVariable(PrStringView name) {
    if (name.isEmpty())
        return prRawStringEmpty();
    return PR_PLATFORM_ENVIRONMENT_VARIABLE(name);
}

bool prEnvironmentSetVariable(PrStringView name, PrStringView value) {
    if (!isName(name) || prStringFindByte(value, '\0') != value.size)
        return false;
    // The platform takes the null view as removal, so a null value is the empty one.
    return PR_PLATFORM_ENVIRONMENT_SET_VARIABLE(name, value.data != nullptr ? value : PrStringView(""));
}

bool prEnvironmentRemoveVariable(PrStringView name) {
    if (!isName(name))
        return false;
    return PR_PLATFORM_ENVIRONMENT_SET_VARIABLE(name, PrStringView{});
}

PrRawString prEnvironmentHostName(void) {
    return PR_PLATFORM_ENVIRONMENT_HOST_NAME();
}
