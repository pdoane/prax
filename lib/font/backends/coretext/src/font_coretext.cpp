// The CoreText backend of lib/font (font_coretext.h).

#include "pr/font_coretext.h"

#include "pr/memory.h"

#include <CoreText/CoreText.h>

namespace pr_impl {

namespace {

// Returns the UTF-8 text of `string` in `buffer` of `capacity` bytes, or none if it does not fit.
PrStringView utf8Of(CFStringRef string, char* buffer, size_t capacity) {
    CFIndex used = 0;
    CFIndex length = CFStringGetLength(string);
    if (CFStringGetBytes(string, CFRangeMake(0, length), kCFStringEncodingUTF8, 0, false,
            reinterpret_cast<UInt8*>(buffer), CFIndex(capacity), &used) != length)
        return PrStringView{nullptr, 0};
    return PrStringView{buffer, size_t(used)};
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

uint32_t prFontCoreTextInstall(PrFontSet* set) {
    // The collection of the available fonts describes them from the font server, so the files are not read.
    CTFontCollectionRef collection = CTFontCollectionCreateFromAvailableFonts(nullptr);
    if (collection == nullptr)
        return 0;
    CFArrayRef descriptors = CTFontCollectionCreateMatchingFontDescriptors(collection);
    CFRelease(collection);
    if (descriptors == nullptr)
        return 0;
    uint32_t before = prFontSetFaceCount(set);
    constexpr size_t kTextMaxBytes = 4096;
    char* path = static_cast<char*>(prMemoryAlloc(kTextMaxBytes));
    char* family = static_cast<char*>(prMemoryAlloc(kTextMaxBytes));
    CFIndex count = CFArrayGetCount(descriptors);
    for (CFIndex i = 0; i < count; ++i) {
        CTFontDescriptorRef descriptor = static_cast<CTFontDescriptorRef>(CFArrayGetValueAtIndex(descriptors, i));
        CFURLRef url = static_cast<CFURLRef>(CTFontDescriptorCopyAttribute(descriptor, kCTFontURLAttribute));
        CFStringRef name =
            static_cast<CFStringRef>(CTFontDescriptorCopyAttribute(descriptor, kCTFontFamilyNameAttribute));
        if (url != nullptr && name != nullptr &&
                CFURLGetFileSystemRepresentation(url, true, reinterpret_cast<UInt8*>(path), CFIndex(kTextMaxBytes))) {
            size_t size = 0;
            while (path[size] != 0)
                ++size;
            PrStringView text = utf8Of(name, family, kTextMaxBytes);
            if (text.size != 0)
                prFontSetAddFileFamily(set, text, PrStringView{path, size});
        }
        if (url != nullptr)
            CFRelease(url);
        if (name != nullptr)
            CFRelease(name);
    }
    prMemoryFree(family);
    prMemoryFree(path);
    CFRelease(descriptors);
    return prFontSetFaceCount(set) - before;
}
