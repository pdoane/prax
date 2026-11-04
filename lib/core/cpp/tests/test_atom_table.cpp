#include "pr/atom_table.h"
#include "pr/string_ops.h"

#include "tools/harness/test.h"

#include <string>
#include <vector>

namespace pr_impl {

namespace {

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an atom table answers one atom for each text, whose text stays where it was put") {
    AtomTable table = {};
    CHECK(atomTableFind(&table, "a") == kAtomNone);

    // Enough text to fill several pages, and one text longer than a page.
    std::vector<std::string> texts;
    for (int i = 0; i < 3000; ++i)
        texts.push_back("text-" + std::to_string(i));
    texts.push_back(std::string(kAtomPageSize + 10, 'x'));
    texts.push_back("");
    std::vector<uint32_t> atoms;
    std::vector<const char*> where;
    for (const std::string& text : texts) {
        atoms.push_back(atomTableAdd(&table, text));
        where.push_back(atomTableText(&table, atoms.back()).data);
    }
    for (size_t i = 0; i < texts.size(); ++i) {
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
