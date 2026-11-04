// The interface of syntax: the registry's lookups and categories, and the dispatch to a language's trees.

#include "pr/alloc.h"
#include "pr/float.h"
#include "pr/memory.h"
#include "pr/syntax.h"

#include "tools/harness/test.h"

#include <string>

extern "C" int prTestSyntaxCApi(void);

namespace pr_impl {

namespace {

// A language whose tree names each run of digits `number`, and counts its updates.
struct DigitTree : PrSyntaxTree {
    PrSyntaxRegistry* registry;
    PrTextEditBuffer* buffer;
    uint32_t updates;
};

void digitDestroy(PrSyntaxTree* tree) {
    prMemoryFree(tree);
}

bool digitUpdate(PrSyntaxTree* tree, double) {
    static_cast<DigitTree*>(tree)->updates += 1;
    return true;
}

uint32_t digitSpans(PrSyntaxTree* tree, uint32_t start, uint32_t end, PrSyntaxSpan* PR_NULLABLE out, uint32_t capacity) {
    DigitTree* h = static_cast<DigitTree*>(tree);
    PrStringView text = prTextEditBufferText(h->buffer);
    PrSyntaxCategory number = prSyntaxRegistryCategoryIntern(h->registry, "number");
    uint32_t count = 0;
    uint32_t i = start;
    while (i < end) {
        if (text.data[i] < '0' || text.data[i] > '9') {
            ++i;
            continue;
        }
        uint32_t from = i;
        while (i < text.size && text.data[i] >= '0' && text.data[i] <= '9')
            ++i;
        if (count < capacity)
            out[count] = PrSyntaxSpan{from, i, number};
        ++count;
    }
    return count;
}

PrSyntaxTree* digitCreate(const PrSyntaxLanguage*, PrSyntaxRegistry* registry, PrTextEditBuffer* buffer) {
    DigitTree* h = allocZeroed<DigitTree>();
    h->destroy = digitDestroy;
    h->update = digitUpdate;
    h->spans = digitSpans;
    h->registry = registry;
    h->buffer = buffer;
    return h;
}

const PrStringView kDigitExtensions[] = {"num", "dig"};
const PrStringView kDigitFileNames[] = {"Numbers"};

const PrSyntaxLanguage kDigits{
    .name = "digits",
    .extensions = kDigitExtensions,
    .extensionCount = 2,
    .fileNames = kDigitFileNames,
    .fileNameCount = 1,
    .create = digitCreate,
};

const PrSyntaxLanguage kOther{
    .name = "other",
    .extensions = kDigitExtensions,
    .extensionCount = 1,
    .create = digitCreate,
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public header compiles as C") {
    CHECK(prTestSyntaxCApi() == 0);
}

TEST_CASE("a registry finds a language by name, by file name and by extension, in any case") {
    PrSyntaxRegistry* registry = prSyntaxRegistryCreate();
    CHECK(prSyntaxRegistryFind(registry, "digits") == nullptr);
    prSyntaxRegistryAdd(registry, &kDigits);
    CHECK(prSyntaxRegistryFind(registry, "Digits") == &kDigits);
    CHECK(prSyntaxRegistryFromPath(registry, "/a/b/Numbers") == &kDigits);
    CHECK(prSyntaxRegistryFromPath(registry, "c:\\x\\file.DIG") == &kDigits);
    CHECK(prSyntaxRegistryFromPath(registry, "file.txt") == nullptr);
    CHECK(prSyntaxRegistryFromPath(registry, ".num") == nullptr);
    CHECK(prSyntaxRegistryFromPath(registry, "num") == nullptr);
    // A language added later wins what both claim.
    prSyntaxRegistryAdd(registry, &kOther);
    CHECK(prSyntaxRegistryFromPath(registry, "a.num") == &kOther);
    CHECK(prSyntaxRegistryFromPath(registry, "a.dig") == &kDigits);
    prSyntaxRegistryDestroy(registry);
}

TEST_CASE("a registry gives each category name one number") {
    PrSyntaxRegistry* registry = prSyntaxRegistryCreate();
    PrSyntaxCategory keyword = prSyntaxRegistryCategoryIntern(registry, "keyword");
    PrSyntaxCategory method = prSyntaxRegistryCategoryIntern(registry, "function.method");
    CHECK(keyword != method);
    CHECK(prSyntaxRegistryCategoryIntern(registry, "keyword") == keyword);
    CHECK(prSyntaxRegistryCategoryCount(registry) == 2);
    CHECK(prSyntaxRegistryCategoryName(registry, method) == "function.method");
    prSyntaxRegistryDestroy(registry);
}

TEST_CASE("a tree calls its language's functions") {
    PrSyntaxRegistry* registry = prSyntaxRegistryCreate();
    PrTextEditBuffer* buffer = prTextEditBufferCreate("a 12 b 345");
    PrSyntaxTree* h = prSyntaxTreeCreate(&kDigits, registry, buffer);
    CHECK(prSyntaxTreeUpdate(h, kPrF64Infinity));
    CHECK(static_cast<DigitTree*>(h)->updates == 1);
    CHECK_FALSE(prSyntaxTreeIsPending(h));
    PrSyntaxSpan spans[4] = {};
    REQUIRE(prSyntaxTreeSpans(h, 0, 10, spans, 4) == 2);
    CHECK(spans[1].start == 7);
    CHECK(spans[1].end == 10);
    CHECK(prSyntaxRegistryCategoryName(registry, spans[0].category) == "number");
    prSyntaxTreeDestroy(h);
    prSyntaxTreeDestroy(nullptr);
    prTextEditBufferDestroy(buffer);
    prSyntaxRegistryDestroy(registry);
}
