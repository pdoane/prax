#include "pr/memory.h"
#include "pr/string_view.h"

#include "tools/harness/test.h"

// The calls of test_web_string.js.
extern "C" char* prCoreWebTestRoundTrip(const char* data, uint32_t size, uint32_t* outSize);
extern "C" char* prCoreWebTestUnits(const char* data, uint32_t size);

TEST_CASE("the page reads the program's text as a string and writes it back, lone surrogates and all") {
    const PrStringView texts[] = {"", "caf\xc3\xa9", "a\xf0\x9f\x98\x80", "a\xed\xa0\x80" "b"};
    for (PrStringView text : texts) {
        uint32_t size = 0;
        char* back = prCoreWebTestRoundTrip(text.data, uint32_t(text.size), &size);
        CHECK(PrStringView{back, size} == text);
        prMemoryFree(back);
    }
    char* units = prCoreWebTestUnits("a\xed\xa0\x80\xf0\x9f\x98\x80\xff", 9);
    CHECK(PrStringView(units) == "61,d800,d83d,de00,fffd");
    prMemoryFree(units);
}
