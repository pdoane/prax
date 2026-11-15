// What the default backends of resources share.

#include "core/backends/shared/src/core_shared_resource.h"

#include "pr/file.h"
#include "pr/raw_string.h"
#include "pr/text_buffer.h"

namespace pr_impl {

PrBlob* PR_NULLABLE resourceFileOpen(PrStringView directory, PrStringView name) {
    if (directory.isEmpty())
        return nullptr;
    PrTextBuffer path{};
    prTextBufferAppend(&path, directory);
    prFilePathAppend(&path, name);
    PrBlob* blob = prFileOpen(prTextBufferView(&path));
    prTextBufferReset(&path);
    return blob;
}

PrRawString resourceDirectoryBeside(PrStringView executable) {
    if (executable.isEmpty())
        return prRawStringEmpty();
    PrStringView name = prFilePathName(executable);
    PrStringView extension = prFilePathExtension(name);
    PrStringView parent = prFilePathParent(executable);
    PrTextBuffer directory{};
    prTextBufferAppend(&directory, parent);
    prFilePathAppend(&directory, PrStringView{name.data, name.size - extension.size});
    prTextBufferAppend(&directory, ".resources");
    return prTextBufferTake(&directory);
}

} // namespace pr_impl
