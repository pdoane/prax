#include "pr/print.h"

#include "tools/harness/test.h"

using namespace pr_impl;

TEST_CASE("printing to a standard stream reports that the stream took the text") {
    CHECK(print(""));
    CHECK(printError("{}", ""));
    CHECK(printTo(PrFileStream_Output, "{}", PrStringView{nullptr, 0}));
    {
        StreamWriter writer(PrFileStream_Error);
        CHECK(writer.flush());
    }
}
