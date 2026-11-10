// Files through the platform.

#include "pr/file.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/string_ops.h"
#include "pr/utf8.h"

#include "core/src/config.h"

#if !defined(PR_PLATFORM_FILE_MAP)
#error "a build without the default platform binds PR_PLATFORM_FILE_MAP"
#endif

// Windows' paths have drive letters and backslashes. So may the web's, whose file system can be a Windows host's
// through Node.js.
#if defined(_WIN32) || defined(__EMSCRIPTEN__)
#define PR_FILE_WINDOWS_PATHS 1
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

// Returns true if `c` separates the parts of a path.
bool isSeparator(char c) {
#if defined(PR_FILE_WINDOWS_PATHS)
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

// Returns the offset of the last separator of `path`, or its size if it has none.
size_t lastSeparator(PrStringView path) {
    for (size_t at = path.size; at-- > 0;) {
        if (isSeparator(path.data[at]))
            return at;
    }
    return path.size;
}

// Returns the length of the root that begins `path`, or zero if it is relative. A root is a separator, and on Windows
// also two separators, which begin a UNC path, or a drive letter and a colon with or without a separator after them.
size_t rootLength(PrStringView path) {
#if defined(PR_FILE_WINDOWS_PATHS)
    if (path.size >= 2 && prAsciiIsLetter(path.data[0]) && path.data[1] == ':')
        return path.size >= 3 && isSeparator(path.data[2]) ? 3 : 2;
    if (path.size >= 2 && isSeparator(path.data[0]) && isSeparator(path.data[1]))
        return 2;
#endif
    if (path.size >= 1 && isSeparator(path.data[0]))
        return 1;
    return 0;
}

// Appends the root of `path`, whose length is `root`, with its separators written as slashes. A drive's root ends in a
// slash whether or not `path` has one there.
void appendRoot(PrTextBuffer* out, PrStringView path, size_t root) {
    for (size_t i = 0; i < root; ++i)
        prTextBufferAppendChar(out, isSeparator(path.data[i]) ? '/' : path.data[i]);
    if (root == 2 && path.data[1] == ':')
        prTextBufferAppendChar(out, '/');
}

// Appends the parts of `path` from `start` to `out`, normalized, after the root that `out` holds up to `rootEnd`.
void appendParts(PrTextBuffer* out, uint32_t rootEnd, PrStringView path, size_t start) {
    size_t at = start;
    while (at < path.size) {
        size_t end = at;
        while (end < path.size && !isSeparator(path.data[end]))
            ++end;
        PrStringView part = prStringSlice(path, at, end - at);
        at = end + 1;
        if (part.isEmpty() || part == ".")
            continue;
        if (part == "..") {
            // The last part goes, with the separator before it. At the root, there is none to remove.
            uint32_t cut = out->size;
            while (cut > rootEnd && out->data[cut - 1] != '/')
                --cut;
            out->size = cut > rootEnd ? cut - 1 : rootEnd;
            continue;
        }
        if (out->size > rootEnd)
            prTextBufferAppendChar(out, '/');
        prTextBufferAppend(out, part);
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

bool prFileMap(PrStringView path, PrByteView* out) {
    if (PR_PLATFORM_FILE_MAP(path, out))
        return true;
    *out = PrByteView{};
    return false;
}

void prFileUnmap(PrByteView* mapping) {
    if (mapping->data != nullptr)
        PR_PLATFORM_FILE_UNMAP(mapping);
    *mapping = PrByteView{};
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

PrRawString prFileUserDirectory(PrFileUserDirectory directory) {
    PR_ASSERT(directory < PrFileUserDirectory_Count);
    return PR_PLATFORM_FILE_USER_DIRECTORY(directory);
}

PrRawString prFileWorkingDirectory(void) {
    return PR_PLATFORM_FILE_WORKING_DIRECTORY();
}

bool prFileStreamWrite(PrFileStream stream, PrByteView bytes) {
    PR_ASSERT(stream == PrFileStream_Output || stream == PrFileStream_Error);
    return PR_PLATFORM_FILE_STREAM_WRITE(stream, bytes);
}

size_t prFileStreamRead(PrFileStream stream, void* out, size_t capacity) {
    PR_ASSERT(stream == PrFileStream_Input);
    return PR_PLATFORM_FILE_STREAM_READ(stream, out, capacity);
}

PrStringView prFilePathName(PrStringView path) {
    size_t separator = lastSeparator(path);
    if (separator == path.size)
        return path;
    return prStringSlice(path, separator + 1, path.size - separator - 1);
}

PrStringView prFilePathParent(PrStringView path) {
    size_t separator = lastSeparator(path);
    if (separator == path.size)
        return prStringSlice(path, 0, 0);
    size_t end = separator;
    while (end > 0 && isSeparator(path.data[end - 1]))
        --end;
    // A part directly under the root keeps the root's separator.
    return prStringSlice(path, 0, end == 0 ? 1 : end);
}

PrStringView prFilePathExtension(PrStringView path) {
    PrStringView name = prFilePathName(path);
    size_t dot = prStringFindLastByte(name, '.');
    if (dot == name.size || dot == 0)
        return prStringSlice(name, name.size, 0);
    return prStringFrom(name, dot);
}

void prFilePathAppend(PrTextBuffer* path, PrStringView part) {
    if (path->size != 0 && !isSeparator(path->data[path->size - 1]))
        prTextBufferAppendChar(path, '/');
    prTextBufferAppend(path, part);
}

bool prFilePathIsAbsolute(PrStringView path) {
    size_t root = rootLength(path);
#if defined(_WIN32)
    // On Windows, one separator is the root of the base's drive.
    return root > 1;
#else
    return root != 0;
#endif
}

PrRawString prFilePathAbsolute(PrStringView path, PrStringView base) {
    if (path.isEmpty())
        return prRawStringNull();
    size_t root = rootLength(path);
    PrTextBuffer out = {};
    if (prFilePathIsAbsolute(path)) {
        appendRoot(&out, path, root);
        appendParts(&out, out.size, path, root);
        return prTextBufferTake(&out);
    }
    // The absolute directory to resolve against: the working directory if `base` is empty, and otherwise `base` made
    // absolute.
    PrRawString directory = base.isEmpty() ? PR_PLATFORM_FILE_WORKING_DIRECTORY() : prFilePathAbsolute(base, "");
    PrStringView parent = prRawStringView(&directory);
    size_t parentRoot = rootLength(parent);
    if (parentRoot == 0) {
        prRawStringReset(&directory);
        return prRawStringNull();
    }
    appendRoot(&out, parent, parentRoot);
    uint32_t rootEnd = out.size;
    if (root == 0)
        appendParts(&out, rootEnd, parent, parentRoot);
    appendParts(&out, rootEnd, path, root);
    prRawStringReset(&directory);
    return prTextBufferTake(&out);
}
