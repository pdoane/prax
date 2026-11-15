// The Apple backend of resources.

#include "pr/core_apple_resource.h"

#include "pr/apple_string.h"
#include "pr/raw_string.h"

#include "core/backends/shared/src/core_shared_resource.h"

#import <Foundation/Foundation.h>

namespace pr_impl {

namespace {

// Returns the directory of the program's resources: its application bundle's resources directory, or for a program
// outside a bundle, <executable>.resources beside its executable. Foundation gives a program outside a bundle the
// directory that holds its executable as its main bundle, with no Info.plist and so no identifier, which every
// application bundle has.
PrRawString findResourceDirectory() {
    @autoreleasepool {
        NSBundle* bundle = NSBundle.mainBundle;
        if (bundle.bundleIdentifier != nil)
            return bundle.resourcePath != nil ? prAppleNSStringToRawString(bundle.resourcePath) : prRawStringEmpty();
        if (bundle.executablePath == nil)
            return prRawStringEmpty();
        PrRawString executable = prAppleNSStringToRawString(bundle.executablePath);
        PrRawString beside = resourceDirectoryBeside(prRawStringView(&executable));
        prRawStringReset(&executable);
        return beside;
    }
}

// The directory of the program's resources, which is the same for the life of the process.
PrStringView resourceDirectory() {
    static const PrRawString directory = findResourceDirectory();
    return prRawStringView(&directory);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrBlob* PR_NULLABLE prCoreAppleResourceOpen(PrStringView name) {
    return resourceFileOpen(resourceDirectory(), name);
}
