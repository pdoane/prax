// Files through the platform.

#include "pr/file.h"

#include "pr/utf8.h"

#include "core/src/config.h"

#if !defined(PR_PLATFORM_FILE_MAP)
#error "a build without the default platform binds PR_PLATFORM_FILE_MAP"
#endif

namespace pr_impl {

namespace {

// The caller's callback, which the entries with UTF-8 names are passed on to.
struct EntryFilter {
    PrFileEntryFn fn;
    void* PR_NULLABLE context;
};

void filterEntry(void* PR_NULLABLE context, const PrFileEntry* entry) {
    const EntryFilter* filter = static_cast<const EntryFilter*>(context);
    if (prUtf8Validate(entry->name) == entry->name.size)
        filter->fn(filter->context, entry);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

bool prFileMap(PrStringView path, PrFileMapping* out) {
    if (PR_PLATFORM_FILE_MAP(path, out))
        return true;
    *out = PrFileMapping{nullptr, 0};
    return false;
}

void prFileUnmap(PrFileMapping* mapping) {
    if (mapping->data != nullptr)
        PR_PLATFORM_FILE_UNMAP(mapping);
    *mapping = PrFileMapping{nullptr, 0};
}

bool prFileInfo(PrStringView path, PrFileInfo* out) {
    if (PR_PLATFORM_FILE_INFO(path, out))
        return true;
    *out = PrFileInfo{};
    return false;
}

bool prFileSame(PrStringView a, PrStringView b) {
    return PR_PLATFORM_FILE_SAME(a, b);
}

bool prFileEachEntry(PrStringView path, PrFileEntryFn fn, void* PR_NULLABLE context) {
    EntryFilter filter{fn, context};
    return PR_PLATFORM_FILE_EACH_ENTRY(path, filterEntry, &filter);
}

bool prFileWrite(PrStringView path, PrByteView bytes) {
    return PR_PLATFORM_FILE_WRITE(path, bytes);
}

bool prFileRemove(PrStringView path) {
    return PR_PLATFORM_FILE_REMOVE(path);
}

bool prFileCreateDirectory(PrStringView path) {
    return path.size != 0 && PR_PLATFORM_FILE_CREATE_DIRECTORY(path);
}

PrRawString prFileCacheDirectory(void) {
    return PR_PLATFORM_FILE_CACHE_DIRECTORY();
}

PrRawString prFileDataDirectory(void) {
    return PR_PLATFORM_FILE_DATA_DIRECTORY();
}
