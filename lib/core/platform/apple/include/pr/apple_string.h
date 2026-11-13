// The conversions between UTF-8 and the strings of Apple's frameworks, CoreFoundation's CFString and Foundation's
// NSString, which are one type, and between paths and file URLs, CFURL and NSURL: a helper of Apple's platforms, which
// core provides there for any code that calls those frameworks. The CoreFoundation conversions serve C and C++, and the
// Foundation ones Objective-C, with or without automatic reference counting. A string that is ill-formed, UTF-8 that
// does not decode or UTF-16 with a lone surrogate, does not convert.
//
// prAppleStringToUtf8 returns how many bytes the whole conversion has, and writes them to `out` if `capacity` holds
// them all, so that a call with no capacity measures and a second call fills. It does not write a NUL.

#pragma once

#include "pr/base.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

// CoreFoundation's string and URL, as CoreFoundation declares them, and Foundation's, which are the same objects.
typedef const struct __CFString* CFStringRef;
typedef const struct __CFURL* CFURLRef;
#if defined(__OBJC__)
@class NSString;
@class NSURL;
#endif

#if defined(__cplusplus)
extern "C" {
#endif

// What prAppleStringToUtf8 returns for a string that is ill-formed.
enum { kPrAppleStringInvalid = 0xffffffff };

// Returns `text` as a CoreFoundation string, which the caller releases, or null if it is not UTF-8.
CFStringRef PR_NULLABLE prAppleStringCreate(PrStringView text);

// Converts `string` to UTF-8.
uint32_t prAppleStringToUtf8(CFStringRef string, char* PR_NULLABLE out, uint32_t capacity);

// Returns `string` as UTF-8, or the null string if it is ill-formed.
PrRawString prAppleStringToRawString(CFStringRef string);

// Returns the file URL of `path`, which the caller releases, or null if the path does not convert.
CFURLRef PR_NULLABLE prAppleFileUrlCreate(PrStringView path);

// Returns the path of the file URL `url`, or the null string if the URL has none.
PrRawString prAppleFileUrlToPath(CFURLRef url);

#if defined(__OBJC__)
// Returns `text` as an autoreleased NSString, or nil if it is not UTF-8.
NSString* PR_NULLABLE prAppleNSString(PrStringView text);

// Converts `string` to UTF-8, as prAppleStringToUtf8 does.
uint32_t prAppleNSStringToUtf8(NSString* string, char* PR_NULLABLE out, uint32_t capacity);

// Returns `string` as UTF-8, or the null string if it is ill-formed.
PrRawString prAppleNSStringToRawString(NSString* string);

// Returns the file URL of `path`, autoreleased, or nil if the path does not convert.
NSURL* PR_NULLABLE prAppleNSFileUrl(PrStringView path);
#endif

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
