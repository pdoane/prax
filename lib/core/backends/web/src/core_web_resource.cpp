// The web backend of resources.

#include "pr/core_web_resource.h"

#include "pr/raw_string.h"

#include "core/backends/shared/src/core_shared_resource.h"

#include <stdlib.h>
#include <string.h>

// The path of the script that Node.js runs, in memory from malloc, or null on a page (core_web_resource.js).
extern "C" char* PR_NULLABLE prCoreWebResourceScript(void);

namespace pr_impl {

namespace {

// Returns the directory of the program's resources: <script>.resources beside the script under Node.js, and /resources
// on a page.
PrRawString resourceDirectory() {
    char* script = prCoreWebResourceScript();
    if (script == nullptr)
        return prRawStringCopy("/resources");
    PrRawString directory = resourceDirectoryBeside(PrStringView{script, strlen(script)});
    free(script);
    return directory;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrBlob* PR_NULLABLE prCoreWebResourceOpen(PrStringView name) {
    PrRawString directory = resourceDirectory();
    PrBlob* blob = resourceFileOpen(prRawStringView(&directory), name);
    prRawStringReset(&directory);
    return blob;
}
