// Tests of the conversions between UTF-8 and the strings of Apple's frameworks, and between paths and file URLs.

#include "pr/apple_string.h"
#include "pr/raw_array.h"
#include "pr/string.h"

#include "tools/harness/test.h"

#import <Foundation/Foundation.h>

namespace pr_impl {

namespace {

// Returns the UTF-8 of `string`, converted by measuring and then filling.
String utf8Of(CFStringRef string) {
    uint32_t size = prAppleStringToUtf8(string, nullptr, 0);
    REQUIRE(size != kPrAppleStringInvalid);
    RawArray<char> bytes;
    bytes.resize(size);
    CHECK(prAppleStringToUtf8(string, bytes.data, size) == size);
    String text(PrStringView{bytes.data, size});
    bytes.reset();
    return text;
}

// A string with a lone surrogate, which is not well-formed UTF-16.
NSString* loneSurrogate() {
    const unichar units[] = {'a', 0xd800, 'b'};
    return [NSString stringWithCharacters:units length:3];
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("UTF-8 converts to a CoreFoundation string and back") {
    CFStringRef string = prAppleStringCreate("caf\xc3\xa9 \xf0\x9f\x98\x80");
    REQUIRE(string != nullptr);
    // The emoji is two UTF-16 units.
    CHECK(CFStringGetLength(string) == 7);
    CHECK(utf8Of(string) == "caf\xc3\xa9 \xf0\x9f\x98\x80");
    String raw = String::adopt(prAppleStringToRawString(string));
    CHECK(raw == "caf\xc3\xa9 \xf0\x9f\x98\x80");
    CFRelease(string);
}

TEST_CASE("the empty string converts both ways") {
    CFStringRef string = prAppleStringCreate("");
    REQUIRE(string != nullptr);
    CHECK(prAppleStringToUtf8(string, nullptr, 0) == 0);
    PrRawString raw = prAppleStringToRawString(string);
    CHECK(!prRawStringIsNull(&raw));
    CHECK(prRawStringIsEmpty(&raw));
    CFRelease(string);
}

TEST_CASE("text that is not UTF-8 makes no string") {
    CHECK(prAppleStringCreate("\xff") == nullptr);
    CHECK(prAppleStringCreate("\xed\xa0\x80") == nullptr);
    CHECK((prAppleNSString("\xff") == nil));
}

TEST_CASE("a conversion to UTF-8 writes nothing to a capacity that does not hold it all") {
    CFStringRef string = prAppleStringCreate("abc");
    REQUIRE(string != nullptr);
    char out[3] = {'x', 'x', 'x'};
    CHECK(prAppleStringToUtf8(string, out, 2) == 3);
    CHECK(out[0] == 'x');
    CFRelease(string);
}

TEST_CASE("a string with a lone surrogate does not convert to UTF-8") {
    NSString* string = loneSurrogate();
    CHECK(prAppleStringToUtf8((__bridge CFStringRef)string, nullptr, 0) == kPrAppleStringInvalid);
    CHECK(prAppleNSStringToUtf8(string, nullptr, 0) == kPrAppleStringInvalid);
    PrRawString raw = prAppleNSStringToRawString(string);
    CHECK(prRawStringIsNull(&raw));
}

TEST_CASE("UTF-8 converts to an NSString and back") {
    NSString* string = prAppleNSString("caf\xc3\xa9");
    REQUIRE((string != nil));
    CHECK([string isEqualToString:@"café"]);
    CHECK(prAppleNSStringToUtf8(string, nullptr, 0) == 5);
    String raw = String::adopt(prAppleNSStringToRawString(string));
    CHECK(raw == "caf\xc3\xa9");
}

TEST_CASE("a path converts to a file URL and back") {
    PrStringView path = "/tmp/caf\xc3\xa9 folder/a b.pdf";
    CFURLRef url = prAppleFileUrlCreate(path);
    REQUIRE(url != nullptr);
    String back = String::adopt(prAppleFileUrlToPath(url));
    CHECK(back == path);
    CFRelease(url);

    NSURL* nsUrl = prAppleNSFileUrl(path);
    REQUIRE((nsUrl != nil));
    CHECK(nsUrl.isFileURL);
    CHECK([nsUrl.path isEqualToString:@"/tmp/café folder/a b.pdf"]);
    // The URL escapes what a URL cannot hold.
    CHECK([nsUrl.absoluteString isEqualToString:@"file:///tmp/caf%C3%A9%20folder/a%20b.pdf"]);
}

TEST_CASE("a URL with no path has none") {
    CFURLRef url = CFURLCreateWithString(nullptr, CFSTR("mailto:"), nullptr);
    REQUIRE(url != nullptr);
    PrRawString path = prAppleFileUrlToPath(url);
    CHECK(prRawStringIsNull(&path));
    CFRelease(url);
}
