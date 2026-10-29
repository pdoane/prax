// Highlights
//
// css-highlight-api-1 - https://drafts.csswg.org/css-highlight-api-1/

#include "css/src/css_highlight.h"

#include "pr/alloc.h"
#include "pr/string_ops.h"

#include "dom/src/dom_range.h"

namespace pr_impl {

namespace {

void destroyEntry(CssHighlightEntry* entry) {
    prStringReset(&entry->name);
    weakReset(&entry->highlight);
    prMemoryFree(entry);
}

} // namespace

PrCssHighlight* cssHighlightCreate() {
    PrCssHighlight* highlight = allocZeroed<PrCssHighlight>();
    highlight->references = 1;
    highlight->type = PrCssHighlightType_Highlight;
    return highlight;
}

void cssHighlightRetain(PrCssHighlight* highlight) {
    ++highlight->references;
}

void cssHighlightRelease(PrCssHighlight* PR_NULLABLE highlight) {
    if (highlight == nullptr || --highlight->references != 0)
        return;
    weakClearAll(&highlight->weak);
    for (uint32_t i = 0; i < highlight->ranges.count; ++i)
        domRangeRelease(highlight->ranges.data[i]);
    highlight->ranges.reset();
    prMemoryFree(highlight);
}

void cssHighlightAdd(PrCssHighlight* highlight, PrDomRange* range) {
    if (highlight->ranges.find(range) != kArrayMissing)
        return;
    domRangeRetain(range);
    highlight->ranges.push(range);
    ++highlight->version;
}

bool cssHighlightDelete(PrCssHighlight* highlight, PrDomRange* range) {
    if (!highlight->ranges.removeFirst(range))
        return false;
    domRangeRelease(range);
    ++highlight->version;
    return true;
}

void cssHighlightClear(PrCssHighlight* highlight) {
    if (highlight->ranges.count == 0)
        return;
    for (uint32_t i = 0; i < highlight->ranges.count; ++i)
        domRangeRelease(highlight->ranges.data[i]);
    highlight->ranges.count = 0;
    ++highlight->version;
}

void cssHighlightRegistryDestroy(PrCssHighlightRegistry* PR_NULLABLE registry) {
    if (registry == nullptr)
        return;
    for (uint32_t i = 0; i < registry->entries.count; ++i)
        destroyEntry(registry->entries.data[i]);
    registry->entries.reset();
    prMemoryFree(registry);
}

PrCssHighlightRegistry* cssHighlightRegistryLive(PrCssHighlightRegistry* registry) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < registry->entries.count; ++i) {
        CssHighlightEntry* entry = registry->entries.data[i];
        if (entry->highlight.target != nullptr)
            registry->entries.data[kept++] = entry;
        else
            destroyEntry(entry);
    }
    if (kept != registry->entries.count) {
        registry->entries.count = kept;
        ++registry->version;
    }
    return registry;
}

uint32_t cssHighlightRegistryFind(const PrCssHighlightRegistry* registry, PrStringView name) {
    for (uint32_t i = 0; i < registry->entries.count; ++i) {
        if (prStringView(&registry->entries.data[i]->name) == name)
            return i;
    }
    return kArrayMissing;
}

void cssHighlightRegistrySet(PrCssHighlightRegistry* registry, PrStringView name,
                             PrCssHighlight* highlight) {
    uint32_t at = cssHighlightRegistryFind(registry, name);
    CssHighlightEntry* entry = nullptr;
    if (at != kArrayMissing) {
        entry = registry->entries.data[at];
    } else {
        entry = allocZeroed<CssHighlightEntry>();
        entry->name = prStringCopy(name);
        registry->entries.push(entry);
    }
    weakSet(&entry->highlight, &highlight->weak, highlight);
    ++registry->version;
}

bool cssHighlightRegistryDelete(PrCssHighlightRegistry* registry, PrStringView name) {
    uint32_t at = cssHighlightRegistryFind(registry, name);
    if (at == kArrayMissing)
        return false;
    destroyEntry(registry->entries.data[at]);
    registry->entries.erase(at);
    ++registry->version;
    return true;
}

void cssHighlightRegistryClear(PrCssHighlightRegistry* registry) {
    if (registry->entries.count == 0)
        return;
    for (uint32_t i = 0; i < registry->entries.count; ++i)
        destroyEntry(registry->entries.data[i]);
    registry->entries.count = 0;
    ++registry->version;
}

} // namespace pr_impl
