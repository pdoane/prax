// The conversions between UTF-8 and the strings of Apple's frameworks, and between paths and file URLs.

#include "pr/apple_string.h"

#include "pr/memory.h"

#include "core/src/raw_string.h"

#import <Foundation/Foundation.h>

using namespace pr_impl;

CFStringRef PR_NULLABLE prAppleStringCreate(PrStringView text) {
    return CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(text.data), CFIndex(text.size),
        kCFStringEncodingUTF8, false);
}

uint32_t prAppleStringToUtf8(CFStringRef string, char* PR_NULLABLE out, uint32_t capacity) {
    CFIndex length = CFStringGetLength(string);
    CFRange all = CFRangeMake(0, length);
    // A lone surrogate does not convert, so a conversion that stops short of the end is of an ill-formed string.
    CFIndex size = 0;
    if (CFStringGetBytes(string, all, kCFStringEncodingUTF8, 0, false, nullptr, 0, &size) != length ||
            size >= CFIndex(kPrAppleStringInvalid))
        return kPrAppleStringInvalid;
    if (out != nullptr && capacity >= uint32_t(size))
        CFStringGetBytes(string, all, kCFStringEncodingUTF8, 0, false, reinterpret_cast<UInt8*>(out), size, nullptr);
    return uint32_t(size);
}

PrRawString prAppleStringToRawString(CFStringRef string) {
    uint32_t size = prAppleStringToUtf8(string, nullptr, 0);
    if (size == kPrAppleStringInvalid)
        return prRawStringNull();
    if (size == 0)
        return prRawStringEmpty();
    char* data = static_cast<char*>(prMemoryAlloc(size));
    prAppleStringToUtf8(string, data, size);
    return rawStringAdopt(data, size, size);
}

CFURLRef PR_NULLABLE prAppleFileUrlCreate(PrStringView path) {
    return CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.data),
        CFIndex(path.size), false);
}

PrRawString prAppleFileUrlToPath(CFURLRef url) {
    // The path as the URL holds it, in the normalization that it was made with. The file system's own form would be
    // decomposed, and the file systems find a file by either.
    CFStringRef path = CFURLCopyFileSystemPath(url, kCFURLPOSIXPathStyle);
    if (path == nullptr)
        return prRawStringNull();
    PrRawString text = prAppleStringToRawString(path);
    CFRelease(path);
    return text;
}

NSString* PR_NULLABLE prAppleNSString(PrStringView text) {
    return CFBridgingRelease(prAppleStringCreate(text));
}

uint32_t prAppleNSStringToUtf8(NSString* string, char* PR_NULLABLE out, uint32_t capacity) {
    return prAppleStringToUtf8((__bridge CFStringRef)string, out, capacity);
}

PrRawString prAppleNSStringToRawString(NSString* string) {
    return prAppleStringToRawString((__bridge CFStringRef)string);
}

NSURL* PR_NULLABLE prAppleNSFileUrl(PrStringView path) {
    return CFBridgingRelease(prAppleFileUrlCreate(path));
}
