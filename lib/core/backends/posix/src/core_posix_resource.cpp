// The POSIX backend of resources.

#include "pr/core_posix_resource.h"

#include "pr/raw_string.h"

#include "core/backends/shared/src/core_shared_resource.h"

#include <limits.h>
#include <unistd.h>

namespace pr_impl {

namespace {

// Returns the path of the process's executable, or the empty string if the system does not name it.
PrRawString executablePath() {
    char path[PATH_MAX];
    ssize_t size = readlink("/proc/self/exe", path, sizeof(path));
    if (size <= 0 || size_t(size) >= sizeof(path))
        return prRawStringEmpty();
    return prRawStringCopy(PrStringView{path, size_t(size)});
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrBlob* PR_NULLABLE prCorePosixResourceOpen(PrStringView name) {
    PrRawString executable = executablePath();
    PrRawString directory = resourceDirectoryBeside(prRawStringView(&executable));
    PrBlob* blob = resourceFileOpen(prRawStringView(&directory), name);
    prRawStringReset(&directory);
    prRawStringReset(&executable);
    return blob;
}
