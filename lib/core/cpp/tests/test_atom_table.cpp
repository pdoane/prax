#include "pr/array.h"
#include "pr/atom_table.h"
#include "pr/string.h"
#include "pr/string_ops.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an atom table answers one atom for each text, whose text stays where it was put") {
    AtomTable table = {};
    CHECK(atomTableFind(&table, "a") == kAtomNone);

    // Enough text to fill several pages, and one text longer than a page.
    Array<String> texts;
    for (int i = 0; i < 3000; ++i)
        texts.push(format("text-{}", i));
    String longText;
    for (uint32_t i = 0; i < kAtomPageSize + 10; ++i)
        longText.append('x');
    texts.push(move(longText));
    texts.push("");
    Array<uint32_t> atoms;
    Array<const char*> where;
    for (const String& text : texts) {
        atoms.push(atomTableAdd(&table, text));
        where.push(atomTableText(&table, atoms.last()).data);
    }
    for (uint32_t i = 0; i < texts.count(); ++i) {
        CHECK(atomTableAdd(&table, texts[i]) == atoms[i]);
        CHECK(atomTableFind(&table, texts[i]) == atoms[i]);
        CHECK(atomTableText(&table, atoms[i]) == texts[i]);
        CHECK(atomTableText(&table, atoms[i]).data == where[i]);
    }
    CHECK(atoms[0] != atoms[1]);
    CHECK(atomTableBytes(&table) >= kAtomPageSize + 10);

    // Clearing forgets every atom and keeps the pages for reuse.
    uint32_t pages = table.pages.count;
    atomTableClear(&table);
    CHECK(atomTableFind(&table, "text-0") == kAtomNone);
    CHECK(atomTableBytes(&table) == 0);
    CHECK(atomTableAdd(&table, "again") == 0);
    CHECK(table.pages.count == pages);
    atomTableReset(&table);
}
